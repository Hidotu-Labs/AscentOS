/*
 * nvme_bench — synchronous block-I/O benchmark for AvoryOS (Phase 6).
 *
 * The kernel block path is synchronous, so a queue depth of N is modelled with
 * N worker threads, each issuing one request at a time.  Measuring qd 1 -> 4
 * exposes how well per-CPU NVMe queues scale; --compare runs the same workload
 * on a second device (typically the AHCI scratch disk) for a direct
 * comparison.
 *
 * Usage:
 *   nvme_bench <device> [--mode=seqread|seqwrite|randread|randwrite]
 *              [--block=BYTES] [--qd=N] [--seconds=S] [--bytes=N]
 *              [--sweep] [--min-scale=X] [--compare=DEV]
 *
 * Output markers (parsed by scripts/nvme/nvme-stress.sh):
 *   NVME-BENCH: dev=... mode=... block=... qd=... seconds=...
 *               iops=... mbps=...
 *   NVME-SCALE: PASS|FAIL qd1=... qd4=... ratio=...
 *   NVME-TEST: PASS|FAIL   (then power off)
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SECTOR_SIZE 512u
#define MAX_QD 64
/* sys_pread/pwrite64 truncate offsets to 32 bits until the phase 7 audit. */
#define TESTABLE_MAX (0xFFFFFFFFULL - (16ULL * 1024 * 1024))

struct bench_job {
  int fd;
  uint64_t block;
  uint64_t region;
  uint64_t thread_index;
  uint64_t threads;
  int random;
  int write;
  int target_cpu; /* -1 = no pinning */
  int ncpu;
  double seconds;
  uint64_t bytes;
  uint64_t ops;
  int error;
};

static uint64_t parse_u64(const char *s, uint64_t *out) {
  uint64_t v = 0;
  if (!s || !*s)
    return 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (uint64_t)(*s++ - '0');
  *out = v;
  return 1;
}

static uint64_t parse_size(const char *s) {
  uint64_t v = 0;
  parse_u64(s, &v);
  if (*s && (s[strlen(s) - 1] == 'k' || s[strlen(s) - 1] == 'K'))
    v *= 1024;
  else if (*s && (s[strlen(s) - 1] == 'm' || s[strlen(s) - 1] == 'M'))
    v *= 1024 * 1024;
  return v;
}

static uint64_t device_bytes(const char *path) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  char sysfs[128];
  snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%.31s/size", name);
  int fd = open(sysfs, O_RDONLY);
  if (fd >= 0) {
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
      uint64_t v = 0;
      if (parse_u64(buf, &v) && v > 0)
        return v;
    }
  }
  struct stat st;
  if (stat(path, &st) == 0 && st.st_size > 0)
    return (uint64_t)st.st_size;
  return 0;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int online_cpu_count(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0)
    return 1;
  int count = 0;
  for (int i = 0; i < 64; i++)
    if (CPU_ISSET(i, &set))
      count++;
  return count > 0 ? count : 1;
}

static uint64_t xorshift64(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

static void *bench_worker(void *opaque) {
  struct bench_job *job = (struct bench_job *)opaque;

  /* Pin worker i to CPU i so thread i submits on its own per-CPU NVMe queue;
   * without this the scheduler may pile every request onto one queue, which
   * hides queue scaling. */
  unsigned cpu_before = 99, cpu_after = 99;
  syscall(SYS_getcpu, &cpu_before, NULL, NULL);
  int set_rc = 0, get_rc = 0;
  if (job->target_cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(job->target_cpu, &set);
    set_rc = sched_setaffinity(0, sizeof(set), &set);
    /* Yield once so the scheduler re-places the thread on a CPU allowed by
     * the new mask instead of finishing this timeslice on the old one. */
    sched_yield();
  }
  syscall(SYS_getcpu, &cpu_after, NULL, NULL);
  cpu_set_t got;
  CPU_ZERO(&got);
  get_rc = sched_getaffinity(0, sizeof(got), &got);
  unsigned long long mask = 0;
  for (int i = 0; i < 64; i++)
    if (CPU_ISSET(i, &got))
      mask |= 1ULL << i;
  printf("NVME-BENCH-CPU: worker=%llu want=%d ncpu=%d set_rc=%d get_rc=%d "
         "mask=%llx cpu_before=%u cpu_after=%u\n",
         (unsigned long long)job->thread_index, job->target_cpu, job->ncpu,
         set_rc, get_rc, mask, cpu_before, cpu_after);
  fflush(stdout);

  uint8_t *buf = aligned_alloc(4096, job->block);
  if (!buf) {
    job->error = 1;
    return NULL;
  }
  for (uint64_t i = 0; i < job->block; i++)
    buf[i] = (uint8_t)(job->thread_index * 31u + i);

  uint64_t slots = job->region / job->block;
  uint64_t per_thread = slots / job->threads;
  if (per_thread == 0)
    per_thread = 1;
  uint64_t start = job->thread_index * per_thread;
  uint64_t cursor = 0;
  uint64_t rng = 0x9E3779B97F4A7C15ULL * (job->thread_index + 1);
  double deadline = now_seconds() + job->seconds;

  if (job->write) {
    if (job->random) {
      while (now_seconds() < deadline) {
        uint64_t slot = xorshift64(&rng) % slots;
        off_t off = (off_t)(slot * job->block);
        if (pwrite(job->fd, buf, (size_t)job->block, off) !=
            (ssize_t)job->block) {
          job->error = 1;
          break;
        }
        job->ops++;
      }
    } else {
      while (now_seconds() < deadline) {
        uint64_t slot = start + cursor;
        if (slot >= start + per_thread || slot >= slots) {
          cursor = 0;
          slot = start;
        }
        off_t off = (off_t)(slot * job->block);
        if (pwrite(job->fd, buf, (size_t)job->block, off) !=
            (ssize_t)job->block) {
          job->error = 1;
          break;
        }
        cursor++;
        job->ops++;
      }
    }
  } else {
    if (job->random) {
      while (now_seconds() < deadline) {
        uint64_t slot = xorshift64(&rng) % slots;
        off_t off = (off_t)(slot * job->block);
        if (pread(job->fd, buf, (size_t)job->block, off) !=
            (ssize_t)job->block) {
          job->error = 1;
          break;
        }
        job->ops++;
      }
    } else {
      while (now_seconds() < deadline) {
        uint64_t slot = start + cursor;
        if (slot >= start + per_thread || slot >= slots) {
          cursor = 0;
          slot = start;
        }
        off_t off = (off_t)(slot * job->block);
        if (pread(job->fd, buf, (size_t)job->block, off) !=
            (ssize_t)job->block) {
          job->error = 1;
          break;
        }
        cursor++;
        job->ops++;
      }
    }
  }
  job->bytes = job->ops * job->block;

  unsigned cpu_end = 99;
  syscall(SYS_getcpu, &cpu_end, NULL, NULL);
  cpu_set_t end_set;
  CPU_ZERO(&end_set);
  sched_getaffinity(0, sizeof(end_set), &end_set);
  unsigned long long end_mask = 0;
  for (int i = 0; i < 64; i++)
    if (CPU_ISSET(i, &end_set))
      end_mask |= 1ULL << i;
  printf("NVME-BENCH-END: worker=%llu ops=%llu cpu=%u mask=%llx\n",
         (unsigned long long)job->thread_index,
         (unsigned long long)job->ops, cpu_end, end_mask);
  fflush(stdout);

  free(buf);
  return NULL;
}

static int run_bench(const char *dev, const char *mode, uint64_t block,
                     uint64_t qd, uint64_t seconds, uint64_t region,
                     double *iops_out, double *mbps_out) {
  int random = strncmp(mode, "rand", 4) == 0;
  int write = strstr(mode, "write") != NULL;

  uint64_t dev_size = device_bytes(dev);
  if (dev_size == 0) {
    fprintf(stderr, "nvme_bench: cannot size %s\n", dev);
    return 1;
  }
  if (dev_size > TESTABLE_MAX)
    dev_size = TESTABLE_MAX;
  if (region == 0 || region > dev_size)
    region = dev_size;
  region = (region / block) * block;
  if (region < block) {
    fprintf(stderr, "nvme_bench: %s too small for block=%llu\n", dev,
            (unsigned long long)block);
    return 1;
  }

  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "nvme_bench: open %s failed\n", dev);
    return 1;
  }

  struct bench_job jobs[MAX_QD];
  pthread_t tids[MAX_QD];
  int ncpu = online_cpu_count();

  for (uint64_t i = 0; i < qd; i++) {
    jobs[i].fd = fd;
    jobs[i].block = block;
    jobs[i].region = region;
    jobs[i].thread_index = i;
    jobs[i].threads = qd;
    jobs[i].random = random;
    jobs[i].write = write;
    jobs[i].target_cpu = qd > 1 ? (int)(i % (uint64_t)ncpu) : -1;
    jobs[i].ncpu = ncpu;
    jobs[i].seconds = (double)seconds;
    jobs[i].bytes = 0;
    jobs[i].ops = 0;
    jobs[i].error = 0;
  }

  double start = now_seconds();
  for (uint64_t i = 0; i < qd; i++) {
    if (pthread_create(&tids[i], NULL, bench_worker, &jobs[i]) != 0) {
      fprintf(stderr, "nvme_bench: pthread_create failed\n");
      close(fd);
      return 1;
    }
  }

  uint64_t total_ops = 0;
  uint64_t total_bytes = 0;
  int failed = 0;
  for (uint64_t i = 0; i < qd; i++) {
    pthread_join(tids[i], NULL);
    total_ops += jobs[i].ops;
    total_bytes += jobs[i].bytes;
    if (jobs[i].error)
      failed = 1;
  }
  double elapsed = now_seconds() - start;
  if (elapsed <= 0)
    elapsed = 0.000001;
  close(fd);

  *iops_out = (double)total_ops / elapsed;
  *mbps_out = (double)total_bytes / elapsed / (1024.0 * 1024.0);

  printf("NVME-BENCH: dev=%s mode=%s block=%llu qd=%llu seconds=%.0f "
         "iops=%.0f mbps=%.1f\n",
         dev, mode, (unsigned long long)block, (unsigned long long)qd, elapsed,
         *iops_out, *mbps_out);
  fflush(stdout);
  return failed;
}

static void power_off(void) {
  syscall(SYS_reboot, 0xfee1deadu, 0x28121969u, 0x4321fedcu, 0);
  execl("/bin/shutdown", "shutdown", "-P", NULL);
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s <device> [--mode=seqread|seqwrite|randread|randwrite]\n"
          "       [--block=BYTES] [--qd=N] [--seconds=S] [--bytes=N]\n"
          "       [--sweep] [--min-scale=X] [--compare=DEV]\n",
          argv0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }
  const char *dev = argv[1];
  const char *mode = "seqread";
  const char *compare = NULL;
  uint64_t block = 4096;
  uint64_t qd = 1;
  uint64_t seconds = 5;
  uint64_t region = 256u * 1024 * 1024;
  int sweep = 0;
  double min_scale = 0.0;

  for (int i = 2; i < argc; i++) {
    uint64_t v = 0;
    if (strncmp(argv[i], "--mode=", 7) == 0)
      mode = argv[i] + 7;
    else if (strncmp(argv[i], "--block=", 8) == 0)
      block = parse_size(argv[i] + 8);
    else if (strncmp(argv[i], "--qd=", 5) == 0 && parse_u64(argv[i] + 5, &v))
      qd = v;
    else if (strncmp(argv[i], "--seconds=", 10) == 0 &&
             parse_u64(argv[i] + 10, &v))
      seconds = v;
    else if (strncmp(argv[i], "--bytes=", 8) == 0)
      region = parse_size(argv[i] + 8);
    else if (strncmp(argv[i], "--compare=", 10) == 0)
      compare = argv[i] + 10;
    else if (strncmp(argv[i], "--min-scale=", 12) == 0)
      min_scale = atof(argv[i] + 12);
    else if (strcmp(argv[i], "--sweep") == 0)
      sweep = 1;
    else {
      fprintf(stderr, "nvme_bench: unknown option %s\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
  }

  if (block < SECTOR_SIZE || (block & (block - 1)) != 0) {
    fprintf(stderr, "nvme_bench: block size must be a power of two >= 512\n");
    return 2;
  }
  if (qd < 1)
    qd = 1;
  if (qd > MAX_QD)
    qd = MAX_QD;

  int rc = 0;

  if (sweep && !compare) {
    static const uint64_t qds[] = {1, 2, 4, 8};
    double iops[4] = {0};
    double mbps[4] = {0};
    for (int i = 0; i < 4; i++) {
      if (run_bench(dev, mode, block, qds[i], seconds, region, &iops[i],
                    &mbps[i]) != 0)
        rc = 1;
    }
    if (rc == 0 && min_scale > 0.0) {
      double ratio = iops[0] > 0 ? iops[2] / iops[0] : 0.0;
      printf("NVME-SCALE: %s qd1=%.0f qd4=%.0f ratio=%.2f (min %.2f)\n",
             ratio >= min_scale ? "PASS" : "FAIL", iops[0], iops[2], ratio,
             min_scale);
      if (ratio < min_scale)
        rc = 1;
    }
  } else {
    double iops = 0, mbps = 0;
    if (run_bench(dev, mode, block, qd, seconds, region, &iops, &mbps) != 0)
      rc = 1;
    if (compare) {
      if (run_bench(compare, mode, block, qd, seconds, region, &iops, &mbps) !=
          0)
        rc = 1;
    }
  }

  printf("NVME-TEST: %s\n", rc == 0 ? "PASS" : "FAIL");
  fflush(stdout);
  power_off();
  return rc;
}
