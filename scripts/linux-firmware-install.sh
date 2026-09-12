#!/usr/bin/env bash
# linux-firmware-install.sh -- stage the firmware images AvoryOS ships.
#
# Fetches (or reuses) a linux-firmware checkout and copies the files/globs in
# scripts/linux/firmware-manifest.txt into build/firmware/lib/firmware/.  The
# top-level disk image build installs that directory at /lib/firmware, which is
# where the LinuxKPI request_firmware() loader looks (Phase 6 uses it for the
# amdgpu PSP/SMU/GSP/DMCUB blobs).
#
# Usage:
#   scripts/linux-firmware-install.sh            # stage into build/firmware/
#   LINUX_FIRMWARE_REF=<sha> scripts/linux-firmware-install.sh
#
# Environment:
#   LINUX_FIRMWARE_REPO  git remote (default: kernel.org linux-firmware)
#   LINUX_FIRMWARE_REF   commit/tag to check out (default: current master)
#   LINUX_FIRMWARE_SRC   checkout cache (default: build/linux-firmware)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${LINUX_FIRMWARE_REPO:-https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git}"
REF="${LINUX_FIRMWARE_REF:-}"
SRC="${LINUX_FIRMWARE_SRC:-$ROOT/build/linux-firmware}"
DEST="$ROOT/build/firmware/lib/firmware"
MANIFEST="$ROOT/scripts/linux/firmware-manifest.txt"

log() { printf '[linux-firmware] %s\n' "$*"; }
die() { printf '[linux-firmware] error: %s\n' "$*" >&2; exit 1; }

[ -f "$MANIFEST" ] || die "missing $MANIFEST"

# Collect active manifest entries.
mapfile -t entries < <(grep -v '^[[:space:]]*\(#\|$\)' "$MANIFEST")
if [ "${#entries[@]}" -eq 0 ]; then
    log "manifest has no active entries; nothing to install"
    log "add files to scripts/linux/firmware-manifest.txt when a driver needs them"
    exit 0
fi

if [ ! -d "$SRC/.git" ]; then
    log "cloning $REPO (this is several hundred MB)"
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 ${REF:+--branch "$REF"} "$REPO" "$SRC"
elif [ -n "$REF" ]; then
    log "fetching $REF"
    git -C "$SRC" fetch --depth 1 origin "$REF"
    git -C "$SRC" checkout --detach FETCH_HEAD
fi

mkdir -p "$DEST"
copied=0
for entry in "${entries[@]}"; do
    # Expand the entry as a glob relative to the firmware tree.
    matches=()
    while IFS= read -r m; do
        [ -n "$m" ] && matches+=("$m")
    done < <(cd "$SRC" && compgen -G "$entry" || true)

    if [ "${#matches[@]}" -eq 0 ]; then
        die "manifest entry matches nothing: $entry"
    fi

    for m in "${matches[@]}"; do
        mkdir -p "$DEST/$(dirname "$m")"
        cp -f "$SRC/$m" "$DEST/$m"
        copied=$((copied + 1))
    done
done

log "staged $copied file(s) into build/firmware/lib/firmware/"
