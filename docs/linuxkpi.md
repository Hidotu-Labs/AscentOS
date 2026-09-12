# LinuxKPI on AvoryOS

AvoryOS runs unmodified upstream Linux drivers through a LinuxKPI layer.  The
first target is the AMD `amdgpu` driver (Linux 6.6 LTS) on the Raphael iGPU
(gfx1036, DCN 3.1.x); the layer is generic and later phases reuse it for more
DRM drivers (nouveau, i915, ...) and other subsystems.

## Layout

| Path | Role |
|---|---|
| `kernel/linux/` | Pinned upstream Linux subset, vendored by `scripts/linux-import.sh`. Gitignored; never edited. |
| `scripts/linux/subset.txt` | Paths copied from the Linux tree (dirs/files). |
| `scripts/linux/files.txt` | Upstream `.c` files compiled into the kernel. |
| `scripts/linux/firmware-manifest.txt` | Firmware blobs staged for `/lib/firmware` by `scripts/linux-firmware-install.sh`. |
| `kernel/linuxkpi/` | AvoryOS-written glue and implementations (initcalls, FPU, later locks/workqueues/DRM glue). |
| `kernel/linuxkpi/include/generated/` | Hand-maintained kernel config (`autoconf.h`) and build identity for imported code. |
| `kernel/src/linuxkpi/` | Phase 0 glue today; grows into the LinuxKPI implementation tree. |
| `kernel/src/include/linux/` | Legacy stub headers. Imported code does **not** see them; they are being retired as real Linux headers take over. |

## Build integration

`kernel/GNUmakefile` includes `kernel/linux/Makefile.files` (generated) and
compiles listed sources into `obj-$(ARCH)/linux/...` with their own include
order: `linuxkpi/include`, then the Linux tree, and deliberately not the legacy
stubs in `src/include`.  If the Linux tree has not been imported, `LINUX_OBJ`
is empty and the kernel still builds; the Phase 0 self-test logs that the
import is missing.

Commands:

```sh
scripts/linux-import.sh          # fetch/refresh the pinned v6.6.* subset
make -C kernel                   # build (import optional)
scripts/linux-firmware-install.sh
scripts/vfio-vbios.sh 0000:0e:00.0   # host-side VBIOS for passthrough
make run-vfio                    # boot with the GPU passed through (serial console)
```

## Initcalls

Upstream `module_init()` places pointers in `.initcall6.init`; the linker
script keeps all levels and `kernel/src/linuxkpi/init.c` walks them once, late
in `kmain_high_half()`.  With `CONFIG_MODULES=n` this is exactly how built-in
Linux drivers get started.

## Kernel FPU

The kernel is compiled `-mno-sse`; float-heavy imported files (AMD display DML)
are compiled with SSE per-directory and bracket their use with
`kernel_fpu_begin()/end`.  The implementation saves per-CPU XSAVE state with
interrupts masked; calls nest.  See `kernel/src/linuxkpi/fpu.c`.

## Rules

1. Never modify `kernel/linux/**`; changes belong in overlays or glue.
2. Prefer importing pure algorithms from upstream over reimplementing them.
3. Every phase ends with a bootable kernel, a green self-test, and an entry in
   `docs/linuxkpi-progress.md`.
