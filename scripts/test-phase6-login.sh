#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

mkdir -p "$TMP_DIR/etc/skel" "$TMP_DIR/root"
printf 'root:x:0:0:root:/root:/bin/bash\n' > "$TMP_DIR/etc/passwd"
printf 'root:x:0:\n' > "$TMP_DIR/etc/group"

"$ROOT_DIR/scripts/configure-accounts.sh" "$TMP_DIR"

grep -qx 'root::0:0:root:/:/bin/bash' "$TMP_DIR/etc/passwd"
test ! -e "$TMP_DIR/root"
grep -qx 'TYPE=foreground' "$ROOT_DIR/initrd/ascentd/services/console.service"
grep -qx 'RESPAWN=yes' "$ROOT_DIR/initrd/ascentd/services/console.service"
grep -qx 'COMMAND=/bin/ascent-login' "$ROOT_DIR/initrd/ascentd/services/console.service"
grep -q 'write userland/ascent-login.elf bin/ascent-login' "$ROOT_DIR/GNUmakefile"
! grep -q 'opening emergency shell' "$ROOT_DIR/userland/ascentd.c"
! grep -q 'execl("/bin/bash"' "$ROOT_DIR/userland/ascentd.c"
cc -Wall -Wextra -Werror -fsyntax-only "$ROOT_DIR/userland/ascent-login.c"

echo "Phase 6 console login: PASS"
