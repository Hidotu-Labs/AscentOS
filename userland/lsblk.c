// lsblk - list block devices
// Reads from /sys/class/block/ (populated by sysfs_populate_block in the kernel)
// and cross-references /proc/partitions for partition info.
//
// Each subdirectory under /sys/class/block/ contains:
//   dev   - major:minor
//   size  - total bytes
//   removable - 0 or 1
//
// Output format:
//   NAME    MAJ:MIN  RM        SIZE  TYPE
//   ata0    8:0       0    1073741824  disk
//   ...

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCK_CLASS_PATH  "/sys/class/block"
#define PROC_PARTITIONS   "/proc/partitions"

// ── Helpers ───────────────────────────────────────────────────────────────

// Read a sysfs attribute file into buf (up to bufsz-1 bytes), strip newline.
// Returns 0 on success, -1 on failure.
static int read_sysfs_str(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;

  int n = (int)read(fd, buf, bufsz - 1);
  close(fd);
  if (n <= 0) {
    buf[0] = '\0';
    return -1;
  }
  buf[n] = '\0';

  // Strip trailing whitespace / newlines
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                   buf[n - 1] == ' '))
    buf[--n] = '\0';

  return 0;
}

// Format a byte count into a human-readable string (e.g. "512M", "4G").
static void fmt_size(uint64_t bytes, char *out, int outsz) {
  const char *units[] = { "B", "K", "M", "G", "T", "P" };
  int u = 0;
  uint64_t val = bytes;
  // Use integer scaling to avoid floating point dependency
  while (val >= 1024 && u < 5) {
    val /= 1024;
    u++;
  }
  snprintf(out, outsz, "%llu%s", (unsigned long long)val, units[u]);
}

// Parse a uint64 from a string (decimal).
static uint64_t parse_u64(const char *s) {
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (uint64_t)(*s++ - '0');
  return v;
}

// ── Partition table from /proc/partitions ─────────────────────────────────

#define MAX_PARTS 64

struct part_entry {
  int      major;
  int      minor;
  uint64_t blocks; // 1K blocks
  char     name[32];
};

static struct part_entry parts[MAX_PARTS];
static int nparts = 0;

static void load_partitions(void) {
  int fd = open(PROC_PARTITIONS, O_RDONLY);
  if (fd < 0)
    return;

  char buf[4096];
  int n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return;
  buf[n] = '\0';

  // Format: "major minor #blocks name" (skip header lines)
  char *line = buf;
  int lineno = 0;
  while (*line) {
    // Find end of line
    char *end = line;
    while (*end && *end != '\n')
      end++;
    char saved = *end;
    *end = '\0';

    lineno++;
    if (lineno > 2) { // skip "major minor ..." header and blank line
      // Parse fields
      int major = 0, minor = 0;
      uint64_t blocks = 0;
      char name[32] = {0};
      // sscanf equivalent via manual parse
      char *p = line;
      // skip leading spaces
      while (*p == ' ') p++;
      // major
      while (*p >= '0' && *p <= '9') major = major * 10 + (*p++ - '0');
      while (*p == ' ') p++;
      // minor
      while (*p >= '0' && *p <= '9') minor = minor * 10 + (*p++ - '0');
      while (*p == ' ') p++;
      // blocks
      while (*p >= '0' && *p <= '9') { blocks = blocks * 10 + (uint64_t)(*p - '0'); p++; }
      while (*p == ' ') p++;
      // name
      int ni = 0;
      while (*p && *p != ' ' && *p != '\n' && ni < 31)
        name[ni++] = *p++;
      name[ni] = '\0';

      if (ni > 0 && nparts < MAX_PARTS) {
        parts[nparts].major  = major;
        parts[nparts].minor  = minor;
        parts[nparts].blocks = blocks;
        strncpy(parts[nparts].name, name, 31);
        nparts++;
      }
    }

    if (!saved)
      break;
    line = end + 1;
  }
}

// Check whether a name looks like a partition of a disk (ends with a digit
// after at least one non-digit base, e.g. "sda1" but not "sda").
static int is_partition(const char *name) {
  int len = (int)strlen(name);
  if (len < 2)
    return 0;
  return (name[len - 1] >= '0' && name[len - 1] <= '9');
}

// Find a matching entry in /proc/partitions by name.
static const struct part_entry *find_part(const char *name) {
  for (int i = 0; i < nparts; i++) {
    if (strcmp(parts[i].name, name) == 0)
      return &parts[i];
  }
  return NULL;
}

// ── Device entry ──────────────────────────────────────────────────────────

struct blk_dev {
  char     name[64];
  char     maj_min[16];
  int      removable;
  uint64_t size_bytes;
};

#define MAX_DEVS 64
static struct blk_dev devs[MAX_DEVS];
static int ndevs = 0;

static int cmp_blk(const void *a, const void *b) {
  return strcmp(((const struct blk_dev *)a)->name,
                ((const struct blk_dev *)b)->name);
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(void) {
  load_partitions();

  DIR *root = opendir(BLOCK_CLASS_PATH);
  if (!root) {
    // Fall back to /proc/partitions only
    if (nparts == 0) {
      fprintf(stderr, "lsblk: cannot open %s or %s\n",
              BLOCK_CLASS_PATH, PROC_PARTITIONS);
      return 1;
    }
    // Print from /proc/partitions directly
    printf("%-16s  %6s  %2s  %12s  %s\n",
           "NAME", "MAJ:MIN", "RM", "SIZE", "TYPE");
    for (int i = 0; i < nparts; i++) {
      char maj_min[16];
      snprintf(maj_min, sizeof(maj_min), "%d:%d",
               parts[i].major, parts[i].minor);
      uint64_t bytes = parts[i].blocks * 1024ULL;
      char szstr[16];
      fmt_size(bytes, szstr, sizeof(szstr));
      printf("%-16s  %6s  %2d  %12s  %s\n",
             parts[i].name, maj_min, 0, szstr,
             is_partition(parts[i].name) ? "part" : "disk");
    }
    return 0;
  }

  // Collect all devices from sysfs
  struct dirent *ent;
  while ((ent = readdir(root)) != NULL && ndevs < MAX_DEVS) {
    if (ent->d_name[0] == '.')
      continue;

    char devpath[256];
    snprintf(devpath, sizeof(devpath), "%s/%s", BLOCK_CLASS_PATH, ent->d_name);

    char attr[320];

    // dev (major:minor)
    snprintf(attr, sizeof(attr), "%s/dev", devpath);
    char maj_min[16] = "?:?";
    read_sysfs_str(attr, maj_min, sizeof(maj_min));

    // size (bytes)
    snprintf(attr, sizeof(attr), "%s/size", devpath);
    char sizebuf[32] = "0";
    read_sysfs_str(attr, sizebuf, sizeof(sizebuf));
    uint64_t size_bytes = parse_u64(sizebuf);

    // removable
    snprintf(attr, sizeof(attr), "%s/removable", devpath);
    char rmbuf[4] = "0";
    read_sysfs_str(attr, rmbuf, sizeof(rmbuf));
    int removable = (rmbuf[0] == '1') ? 1 : 0;

    strncpy(devs[ndevs].name, ent->d_name, 63);
    strncpy(devs[ndevs].maj_min, maj_min, 15);
    devs[ndevs].removable   = removable;
    devs[ndevs].size_bytes  = size_bytes;
    ndevs++;
  }
  closedir(root);

  // Sort by name for stable output
  qsort(devs, (size_t)ndevs, sizeof(devs[0]), cmp_blk);

  // Header
  printf("%-16s  %7s  %2s  %8s  %s\n",
         "NAME", "MAJ:MIN", "RM", "SIZE", "TYPE");

  for (int i = 0; i < ndevs; i++) {
    char szstr[16];
    fmt_size(devs[i].size_bytes, szstr, sizeof(szstr));

    const char *type = is_partition(devs[i].name) ? "part" : "disk";

    printf("%-16s  %7s  %2d  %8s  %s\n",
           devs[i].name,
           devs[i].maj_min,
           devs[i].removable,
           szstr,
           type);

    // If this is a disk, print any matching partitions from /proc/partitions
    if (strcmp(type, "disk") == 0) {
      int base_len = (int)strlen(devs[i].name);
      for (int p = 0; p < nparts; p++) {
        // Partition names start with the disk name and end with a digit
        if (strncmp(parts[p].name, devs[i].name, (size_t)base_len) == 0 &&
            parts[p].name[base_len] != '\0' &&
            is_partition(parts[p].name)) {
          char pszstr[16];
          fmt_size(parts[p].blocks * 1024ULL, pszstr, sizeof(pszstr));
          char pmaj_min[16];
          snprintf(pmaj_min, sizeof(pmaj_min), "%d:%d",
                   parts[p].major, parts[p].minor);
          printf("  %-14s  %7s  %2d  %8s  part\n",
                 parts[p].name, pmaj_min, 0, pszstr);
        }
      }
    }
  }

  return 0;
}
