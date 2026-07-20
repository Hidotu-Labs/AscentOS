#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Install the firmware selected by the built-in Raphael AMDGPU driver.
set -eu

IMAGE=${1:?usage: install-amdgpu.sh <ext-filesystem-image>}
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

[ -f "$IMAGE" ] || {
    echo "AMDGPU install: image does not exist: $IMAGE" >&2
    exit 1
}
[ -d "$ROOT_DIR/firmware/amdgpu" ] || {
    echo "AMDGPU install: firmware/amdgpu is missing" >&2
    exit 1
}

echo "Installing Raphael AMDGPU firmware into $IMAGE..."
exec "$ROOT_DIR/scripts/populate-ext2-dir.sh" "$IMAGE" \
    "$ROOT_DIR/firmware/amdgpu" lib/firmware/amdgpu
