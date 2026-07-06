#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

sh -n "$ROOT_DIR/initrd/startx.sh"
sh -n "$ROOT_DIR/initrd/startw.sh"
grep -q 'node->mask & 07777' "$ROOT_DIR/kernel/src/syscalls/sys_stat.c"
! grep -q 'export HOME=/' "$ROOT_DIR/initrd/startx.sh"
! grep -q 'seatd -u root' "$ROOT_DIR/initrd/startw.sh"
grep -q 'chmod 0700 "$XDG_RUNTIME_DIR"' "$ROOT_DIR/initrd/startw.sh"
grep -q 'SEATD_SOCK="/run/seatd.sock"' "$ROOT_DIR/initrd/startw.sh"
cc -Wall -Wextra -Werror -fsyntax-only "$ROOT_DIR/userland/test_graphics_session.c"

echo "Non-root graphical session launchers: PASS"
