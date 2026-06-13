# AscentOS

AscentOS is a hobby operating system kernel for the x86_64 architecture, written in C and Assembly. It can run linux programs. (i know its not so special.)

## My Goal
Making this a daily driveable Operating System (I know im not going to use it anyways lol)

## Prerequisites

To build the project on Linux, you'll need:

*   `make`, `gcc`/`clang`, `nasm`
*   `git`, `curl`
*   `xorriso` (for ISO creation)
*   `qemu-system-x86_64` (for emulation)
*   `e2fsprogs` (`mkfs.ext3` and `debugfs` are required for disk image generation)
*   `rustc` & `cargo` 

## Building and Running

1.  **Build everything:**
    ```bash
    make
    ```
    This prepares the musl-based cross-toolchain, builds the kernel, and generates the bootable ISO.

ALSO IMPORTANT YOU GOTTA BUILD PORTED SOFTWARE TOO
 
## Building Ported Software

To build the full suite of ported tools (Bash, X11, etc.), run these scripts in order:

```bash
./scripts/build-bash.sh      && \
./scripts/build-coreutils.sh && \
./scripts/build-tar.sh       && \
./scripts/build-tcc.sh       && \
./scripts/build-tinygl.sh       && \
./scripts/port-x11.sh        && \
```

---
Developed as an open-source project for fun and learning people will say its ai slop but i dont think so.
