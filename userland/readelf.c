// Simple readelf implementation for AscentOS
// Parses ELF64 binaries on Linux x86_64 / AscentOS
// Usage: readelf <file>  [same flags subset as GNU readelf]
//   -h   ELF header
//   -S   section headers
//   -l   program headers (segments)
//   -s   symbol table
//   -d   dynamic section
//   (default: show everything above)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

// ── ELF64 type definitions ───────────────────────────────────────────────────

#define EI_NIDENT 16

typedef struct {
  uint8_t  e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint64_t sh_flags;
  uint64_t sh_addr;
  uint64_t sh_offset;
  uint64_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint64_t sh_addralign;
  uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

typedef struct {
  uint32_t st_name;
  uint8_t  st_info;
  uint8_t  st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
} Elf64_Sym;

typedef struct {
  int32_t  d_tag;
  uint64_t d_val;
} Elf64_Dyn;

// ── ELF magic / constants ────────────────────────────────────────────────────

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1

// e_type
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
#define ET_CORE   4

// e_machine
#define EM_X86_64 62

// sh_type
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_DYNSYM  11

// p_type
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552
#define PT_GNU_EH_FRAME 0x6474e550

// p_flags
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

// st_info binding / type
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6

// Dynamic tags
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ   10
#define DT_SYMENT  11
#define DT_INIT    12
#define DT_FINI    13
#define DT_SONAME  14
#define DT_RPATH   15
#define DT_PLTREL  20
#define DT_JMPREL  23
#define DT_FLAGS   30

// ── Minimal read helpers ─────────────────────────────────────────────────────

static int read_at(int fd, void *buf, size_t count, off_t offset) {
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t n = read(fd, buf, count);
  return (n == (ssize_t)count) ? 0 : -1;
}

// ── Printing helpers ─────────────────────────────────────────────────────────

static void print_hex16(uint64_t v) { printf("0x%016lx", v); }
static void print_hex8(uint64_t v)  { printf("0x%08lx",   v); }
__attribute__((unused))
static void print_hex4(uint32_t v)  { printf("0x%04x",    v); }

static const char *elf_type_str(uint16_t t) {
  switch (t) {
    case ET_NONE: return "NONE (No file type)";
    case ET_REL:  return "REL (Relocatable file)";
    case ET_EXEC: return "EXEC (Executable file)";
    case ET_DYN:  return "DYN (Shared object file)";
    case ET_CORE: return "CORE (Core file)";
    default:      return "Unknown";
  }
}

static const char *elf_machine_str(uint16_t m) {
  switch (m) {
    case EM_X86_64: return "Advanced Micro Devices X86-64";
    case 3:         return "Intel 80386";
    case 40:        return "ARM";
    case 183:       return "AArch64";
    case 243:       return "RISC-V";
    default:        return "Unknown";
  }
}

static const char *sh_type_str(uint32_t t) {
  switch (t) {
    case SHT_NULL:     return "NULL";
    case SHT_PROGBITS: return "PROGBITS";
    case SHT_SYMTAB:   return "SYMTAB";
    case SHT_STRTAB:   return "STRTAB";
    case SHT_RELA:     return "RELA";
    case SHT_HASH:     return "HASH";
    case SHT_DYNAMIC:  return "DYNAMIC";
    case SHT_NOTE:     return "NOTE";
    case SHT_NOBITS:   return "NOBITS";
    case SHT_REL:      return "REL";
    case SHT_DYNSYM:   return "DYNSYM";
    default:           return "UNKNOWN";
  }
}

static const char *pt_type_str(uint32_t t) {
  switch (t) {
    case PT_NULL:         return "NULL";
    case PT_LOAD:         return "LOAD";
    case PT_DYNAMIC:      return "DYNAMIC";
    case PT_INTERP:       return "INTERP";
    case PT_NOTE:         return "NOTE";
    case PT_PHDR:         return "PHDR";
    case PT_TLS:          return "TLS";
    case PT_GNU_STACK:    return "GNU_STACK";
    case PT_GNU_RELRO:    return "GNU_RELRO";
    case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";
    default:              return "UNKNOWN";
  }
}

static void flags_str(uint32_t f, char out[4]) {
  out[0] = (f & PF_R) ? 'R' : ' ';
  out[1] = (f & PF_W) ? 'W' : ' ';
  out[2] = (f & PF_X) ? 'E' : ' ';
  out[3] = '\0';
}

static const char *sym_bind_str(uint8_t b) {
  switch (b) {
    case STB_LOCAL:  return "LOCAL";
    case STB_GLOBAL: return "GLOBAL";
    case STB_WEAK:   return "WEAK";
    default:         return "OTHER";
  }
}

static const char *sym_type_str(uint8_t t) {
  switch (t) {
    case STT_NOTYPE:  return "NOTYPE";
    case STT_OBJECT:  return "OBJECT";
    case STT_FUNC:    return "FUNC";
    case STT_SECTION: return "SECTION";
    case STT_FILE:    return "FILE";
    case STT_COMMON:  return "COMMON";
    case STT_TLS:     return "TLS";
    default:          return "OTHER";
  }
}

static const char *dyn_tag_str(int32_t tag) {
  switch (tag) {
    case DT_NULL:     return "NULL";
    case DT_NEEDED:   return "NEEDED";
    case DT_PLTRELSZ: return "PLTRELSZ";
    case DT_STRTAB:   return "STRTAB";
    case DT_SYMTAB:   return "SYMTAB";
    case DT_RELA:     return "RELA";
    case DT_RELASZ:   return "RELASZ";
    case DT_RELAENT:  return "RELAENT";
    case DT_STRSZ:    return "STRSZ";
    case DT_SYMENT:   return "SYMENT";
    case DT_INIT:     return "INIT";
    case DT_FINI:     return "FINI";
    case DT_SONAME:   return "SONAME";
    case DT_RPATH:    return "RPATH";
    case DT_PLTREL:   return "PLTREL";
    case DT_JMPREL:   return "JMPREL";
    case DT_FLAGS:    return "FLAGS";
    default:          return "UNKNOWN";
  }
}

// ── Section: ELF Header ──────────────────────────────────────────────────────

static void print_elf_header(const Elf64_Ehdr *h) {
  printf("ELF Header:\n");
  printf("  Magic:   ");
  for (int i = 0; i < EI_NIDENT; i++) printf("%02x ", h->e_ident[i]);
  printf("\n");
  printf("  Class:                             ELF%d\n",
         h->e_ident[4] == ELFCLASS64 ? 64 : 32);
  printf("  Data:                              %s\n",
         h->e_ident[5] == ELFDATA2LSB ? "2's complement, little endian"
                                       : "2's complement, big endian");
  printf("  Version:                           %d (current)\n", h->e_ident[6]);
  printf("  OS/ABI:                            %d\n", h->e_ident[7]);
  printf("  ABI Version:                       %d\n", h->e_ident[8]);
  printf("  Type:                              %s\n", elf_type_str(h->e_type));
  printf("  Machine:                           %s\n", elf_machine_str(h->e_machine));
  printf("  Version:                           0x%x\n", h->e_version);
  printf("  Entry point address:               "); print_hex16(h->e_entry); printf("\n");
  printf("  Start of program headers:          %lu (bytes into file)\n", h->e_phoff);
  printf("  Start of section headers:          %lu (bytes into file)\n", h->e_shoff);
  printf("  Flags:                             "); print_hex8(h->e_flags); printf("\n");
  printf("  Size of this header:               %d (bytes)\n", h->e_ehsize);
  printf("  Size of program headers:           %d (bytes)\n", h->e_phentsize);
  printf("  Number of program headers:         %d\n", h->e_phnum);
  printf("  Size of section headers:           %d (bytes)\n", h->e_shentsize);
  printf("  Number of section headers:         %d\n", h->e_shnum);
  printf("  Section header string table index: %d\n", h->e_shstrndx);
}

// ── Section: Section Headers ─────────────────────────────────────────────────

static void print_section_headers(int fd, const Elf64_Ehdr *h) {
  if (h->e_shnum == 0 || h->e_shoff == 0) {
    printf("\nThere are no section headers in this file.\n");
    return;
  }

  // Load all section headers
  Elf64_Shdr *shdrs = malloc(h->e_shnum * sizeof(Elf64_Shdr));
  if (!shdrs) { printf("readelf: out of memory\n"); return; }
  if (read_at(fd, shdrs, h->e_shnum * sizeof(Elf64_Shdr), (off_t)h->e_shoff) < 0) {
    printf("readelf: failed to read section headers\n");
    free(shdrs);
    return;
  }

  // Load section name string table
  char *shstrtab = NULL;
  if (h->e_shstrndx < h->e_shnum) {
    Elf64_Shdr *s = &shdrs[h->e_shstrndx];
    shstrtab = malloc(s->sh_size + 1);
    if (shstrtab && read_at(fd, shstrtab, s->sh_size, (off_t)s->sh_offset) < 0) {
      free(shstrtab);
      shstrtab = NULL;
    }
    if (shstrtab) shstrtab[s->sh_size] = '\0';
  }

  printf("\nSection Headers:\n");
  printf("  [Nr] %-18s %-10s %-16s %-8s %-6s %-5s Flg Lk Inf Al\n",
         "Name", "Type", "Address", "Offset", "Size", "ES");
  printf("  ----+------------------+----------+------------------+--------+------+-----+---+---+---+---\n");

  for (int i = 0; i < h->e_shnum; i++) {
    Elf64_Shdr *s = &shdrs[i];
    const char *name = (shstrtab && s->sh_name < shdrs[h->e_shstrndx].sh_size)
                        ? shstrtab + s->sh_name : "?";

    // Build flags string
    char flg[8] = "   ";
    int fi = 0;
    if (s->sh_flags & 0x2) flg[fi++] = 'A'; // SHF_ALLOC
    if (s->sh_flags & 0x1) flg[fi++] = 'X'; // SHF_EXECINSTR
    if (s->sh_flags & 0x4) flg[fi++] = 'W'; // SHF_WRITE... wait, SHF_WRITE=1, SHF_ALLOC=2, SHF_EXECINSTR=4
    // Redo properly:
    fi = 0; memset(flg, ' ', 3); flg[3] = '\0';
    if (s->sh_flags & 0x1) flg[fi++] = 'W'; // SHF_WRITE
    if (s->sh_flags & 0x2) flg[fi++] = 'A'; // SHF_ALLOC
    if (s->sh_flags & 0x4) flg[fi++] = 'X'; // SHF_EXECINSTR

    printf("  [%2d] %-18.18s %-10s %016lx  %08lx  %06lx  %05lx %3s %2d %3d %2lu\n",
           i, name, sh_type_str(s->sh_type),
           s->sh_addr, s->sh_offset, s->sh_size, s->sh_entsize,
           flg, s->sh_link, s->sh_info, s->sh_addralign);
  }
  printf("Key to Flags:\n"
         "  W (write), A (alloc), X (execute)\n");

  free(shdrs);
  if (shstrtab) free(shstrtab);
}

// ── Section: Program Headers ─────────────────────────────────────────────────

static void print_program_headers(int fd, const Elf64_Ehdr *h) {
  if (h->e_phnum == 0 || h->e_phoff == 0) {
    printf("\nThere are no program headers in this file.\n");
    return;
  }

  Elf64_Phdr *phdrs = malloc(h->e_phnum * sizeof(Elf64_Phdr));
  if (!phdrs) { printf("readelf: out of memory\n"); return; }
  if (read_at(fd, phdrs, h->e_phnum * sizeof(Elf64_Phdr), (off_t)h->e_phoff) < 0) {
    printf("readelf: failed to read program headers\n");
    free(phdrs);
    return;
  }

  printf("\nProgram Headers:\n");
  printf("  %-14s %-8s   %-16s %-16s %-8s %-8s Flg   Align\n",
         "Type", "Offset", "VirtAddr", "PhysAddr", "FileSiz", "MemSiz");
  printf("  --------------+----------+------------------+------------------+----------+----------+------+----------\n");

  for (int i = 0; i < h->e_phnum; i++) {
    Elf64_Phdr *p = &phdrs[i];
    char fl[4]; flags_str(p->p_flags, fl);
    printf("  %-14s 0x%06lx   0x%016lx 0x%016lx 0x%06lx 0x%06lx %-5s 0x%lx\n",
           pt_type_str(p->p_type),
           p->p_offset, p->p_vaddr, p->p_paddr,
           p->p_filesz, p->p_memsz, fl, p->p_align);

    // Print interpreter path for PT_INTERP
    if (p->p_type == PT_INTERP && p->p_filesz > 0 && p->p_filesz < 256) {
      char interp[256] = {0};
      if (read_at(fd, interp, p->p_filesz, (off_t)p->p_offset) == 0)
        printf("      [Requesting program interpreter: %s]\n", interp);
    }
  }

  free(phdrs);
}

// ── Section: Symbol Table ────────────────────────────────────────────────────

static void print_symbols_from_section(int fd, const Elf64_Ehdr *h __attribute__((unused)),
                                        Elf64_Shdr *shdrs,
                                        int symidx, const char *sec_label) {
  Elf64_Shdr *ssym  = &shdrs[symidx];
  Elf64_Shdr *sstr  = &shdrs[ssym->sh_link];

  uint64_t count = ssym->sh_size / sizeof(Elf64_Sym);
  Elf64_Sym *syms = malloc(ssym->sh_size);
  char *strtab    = malloc(sstr->sh_size + 1);

  if (!syms || !strtab) { free(syms); free(strtab); return; }
  if (read_at(fd, syms,   ssym->sh_size, (off_t)ssym->sh_offset) < 0 ||
      read_at(fd, strtab, sstr->sh_size, (off_t)sstr->sh_offset) < 0) {
    free(syms); free(strtab); return;
  }
  strtab[sstr->sh_size] = '\0';

  printf("\nSymbol table '%s' contains %lu entries:\n", sec_label, count);
  printf("   Num:    Value          Size  Type     Bind     Ndx  Name\n");
  printf("   ----+------------------+------+---------+--------+-----+----------------\n");

  for (uint64_t i = 0; i < count; i++) {
    Elf64_Sym *sym = &syms[i];
    uint8_t bind = sym->st_info >> 4;
    uint8_t type = sym->st_info & 0xf;
    const char *name = (sym->st_name < sstr->sh_size) ? strtab + sym->st_name : "";
    char ndx_str[8];
    if      (sym->st_shndx == 0xfff1) snprintf(ndx_str, 8, "ABS");
    else if (sym->st_shndx == 0xfff2) snprintf(ndx_str, 8, "COM");
    else if (sym->st_shndx == 0x0000) snprintf(ndx_str, 8, "UND");
    else snprintf(ndx_str, 8, "%d", sym->st_shndx);
    printf("   %4lu: %016lx  %5lu %-8s %-8s %-5s %s\n",
           i, sym->st_value, sym->st_size,
           sym_type_str(type), sym_bind_str(bind),
           ndx_str, name);
  }

  free(syms);
  free(strtab);
}

static void print_symbols(int fd, const Elf64_Ehdr *h) {
  if (h->e_shnum == 0 || h->e_shoff == 0) return;

  Elf64_Shdr *shdrs = malloc(h->e_shnum * sizeof(Elf64_Shdr));
  if (!shdrs) return;
  if (read_at(fd, shdrs, h->e_shnum * sizeof(Elf64_Shdr), (off_t)h->e_shoff) < 0) {
    free(shdrs); return;
  }

  int found = 0;
  for (int i = 0; i < h->e_shnum; i++) {
    if (shdrs[i].sh_type == SHT_SYMTAB) {
      print_symbols_from_section(fd, h, shdrs, i, ".symtab");
      found = 1;
    } else if (shdrs[i].sh_type == SHT_DYNSYM) {
      print_symbols_from_section(fd, h, shdrs, i, ".dynsym");
      found = 1;
    }
  }
  if (!found) printf("\nNo symbol tables found.\n");
  free(shdrs);
}

// ── Section: Dynamic Section ─────────────────────────────────────────────────

static void print_dynamic(int fd, const Elf64_Ehdr *h) {
  if (h->e_phnum == 0 || h->e_phoff == 0) return;

  Elf64_Phdr *phdrs = malloc(h->e_phnum * sizeof(Elf64_Phdr));
  if (!phdrs) return;
  if (read_at(fd, phdrs, h->e_phnum * sizeof(Elf64_Phdr), (off_t)h->e_phoff) < 0) {
    free(phdrs); return;
  }

  // Find PT_DYNAMIC segment
  Elf64_Phdr *dyn_seg = NULL;
  for (int i = 0; i < h->e_phnum; i++) {
    if (phdrs[i].p_type == PT_DYNAMIC) { dyn_seg = &phdrs[i]; break; }
  }

  if (!dyn_seg) {
    printf("\nThere is no dynamic section in this file.\n");
    free(phdrs); return;
  }

  uint64_t count = dyn_seg->p_filesz / sizeof(Elf64_Dyn);
  Elf64_Dyn *dyns = malloc(dyn_seg->p_filesz);
  if (!dyns) { free(phdrs); return; }
  if (read_at(fd, dyns, dyn_seg->p_filesz, (off_t)dyn_seg->p_offset) < 0) {
    free(dyns); free(phdrs); return;
  }

  printf("\nDynamic section at offset 0x%lx contains %lu entries:\n",
         dyn_seg->p_offset, count);
  printf("  %-12s %-16s Value\n", "Tag", "(Type)");
  printf("  ------------+------------------+-----------------------\n");

  for (uint64_t i = 0; i < count; i++) {
    printf("  0x%010x %-16s 0x%lx\n",
           dyns[i].d_tag, dyn_tag_str(dyns[i].d_tag), dyns[i].d_val);
    if (dyns[i].d_tag == DT_NULL) break;
  }

  free(dyns);
  free(phdrs);
}

// ── Main ─────────────────────────────────────────────────────────────────────

static void usage(const char *prog) {
  printf("Usage: %s [options] <elf-file>\n", prog);
  printf("Options:\n");
  printf("  -h   Display ELF header\n");
  printf("  -S   Display section headers\n");
  printf("  -l   Display program headers (segments)\n");
  printf("  -s   Display symbol tables\n");
  printf("  -d   Display dynamic section\n");
  printf("  -a   Display all (default when no flag given)\n");
  printf("  (combine e.g.: readelf -hSl foo.elf)\n");
}

int main(int argc, char *argv[]) {
  int opt_h = 0, opt_S = 0, opt_l = 0, opt_s = 0, opt_d = 0;
  const char *path = NULL;

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      for (int j = 1; argv[i][j]; j++) {
        switch (argv[i][j]) {
          case 'h': opt_h = 1; break;
          case 'S': opt_S = 1; break;
          case 'l': opt_l = 1; break;
          case 's': opt_s = 1; break;
          case 'd': opt_d = 1; break;
          case 'a': opt_h = opt_S = opt_l = opt_s = opt_d = 1; break;
          default:
            printf("readelf: unknown option '-%c'\n", argv[i][j]);
            usage(argv[0]);
            return 1;
        }
      }
    } else {
      path = argv[i];
    }
  }

  if (!path) {
    usage(argv[0]);
    return 1;
  }

  // If no flags given, show all sections
  if (!opt_h && !opt_S && !opt_l && !opt_s && !opt_d)
    opt_h = opt_S = opt_l = opt_s = opt_d = 1;

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("readelf: cannot open '%s'\n", path);
    return 1;
  }

  Elf64_Ehdr hdr;
  if (read_at(fd, &hdr, sizeof(hdr), 0) < 0) {
    printf("readelf: failed to read ELF header\n");
    close(fd);
    return 1;
  }

  // Verify magic
  if (hdr.e_ident[0] != ELFMAG0 || hdr.e_ident[1] != ELFMAG1 ||
      hdr.e_ident[2] != ELFMAG2 || hdr.e_ident[3] != ELFMAG3) {
    printf("readelf: not an ELF file\n");
    close(fd);
    return 1;
  }
  if (hdr.e_ident[4] != ELFCLASS64) {
    printf("readelf: only ELF64 is supported\n");
    close(fd);
    return 1;
  }

  if (opt_h) print_elf_header(&hdr);
  if (opt_S) print_section_headers(fd, &hdr);
  if (opt_l) print_program_headers(fd, &hdr);
  if (opt_s) print_symbols(fd, &hdr);
  if (opt_d) print_dynamic(fd, &hdr);

  close(fd);
  return 0;
}