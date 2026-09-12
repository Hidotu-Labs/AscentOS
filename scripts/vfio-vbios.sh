#!/usr/bin/env bash
# vfio-vbios.sh -- extract a GPU's VBIOS image on the host for QEMU passthrough.
#
# A passthrough guest does not receive the host's ACPI VFCT table, so the
# guest kernel (amdgpu, later) reads the VBIOS from the device's ROM BAR.  QEMU
# can load an image into that BAR with romfile=; this script produces it from
# the host sysfs ROM node.
#
# The device must not be actively using the ROM when read.  If it is bound to
# a host driver (amdgpu/nvidia/...), unbind it or bind vfio-pci first, or the
# read may return all-0xff.  Reading the ROM node requires root.
#
# Usage: scripts/vfio-vbios.sh [BDF] [OUTPUT]
#   BDF      PCI address of the passed-through GPU (default: 0000:0e:00.0)
#   OUTPUT   destination image (default: build/vfio/vbios.rom)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BDF="${1:-0000:0e:00.0}"
OUT="${2:-$ROOT/build/vfio/vbios.rom}"
ROM="/sys/bus/pci/devices/$BDF/rom"

if [ ! -e "$ROM" ]; then
    echo "vfio-vbios: no such device or ROM node: $ROM" >&2
    echo "vfio-vbios: check the BDF (lspci -D | grep -i vga)" >&2
    exit 1
fi

driver_path="$(readlink -f "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null || true)"
if [ -n "$driver_path" ]; then
    echo "vfio-vbios: warning: device is bound to $(basename "$driver_path");"
    echo "vfio-vbios: ROM reads may fail unless it is bound to vfio-pci."
fi

if [ "$(id -u)" != 0 ]; then
    echo "vfio-vbios: must run as root to read $ROM" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

# Sysfs exposes the ROM only after the enable write; restore it afterwards.
echo 1 > "$ROM"
if ! cat "$ROM" > "$OUT"; then
    echo 0 > "$ROM" || true
    echo "vfio-vbios: failed to read ROM image" >&2
    exit 1
fi
echo 0 > "$ROM"

size="$(stat -c%s "$OUT")"
echo "vfio-vbios: wrote $OUT ($size bytes)"
if [ "$size" -lt 1024 ]; then
    echo "vfio-vbios: image looks too small; ROM read probably failed" >&2
    exit 1
fi

# A VBIOS image starts with the 0x55AA signature.
sig="$(od -An -tx1 -N2 "$OUT" | tr -d ' \n')"
if [ "$sig" != "55aa" ]; then
    echo "vfio-vbios: warning: missing 0x55AA ROM signature (got $sig)" >&2
fi
