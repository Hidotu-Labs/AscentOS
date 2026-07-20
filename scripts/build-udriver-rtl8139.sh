#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MUSL_ROOT="$ROOT_DIR/toolchain/musl-sysroot"
CROSS_CC="$ROOT_DIR/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"
OUTPUT="$ROOT_DIR/userland/8139too-udrv.elf"
TEMP_OUTPUT="$ROOT_DIR/userland/.8139too-udrv.elf.tmp"
COMMON_SOURCES="$ROOT_DIR/userland/linux_compat/src/ascent_compat.c $ROOT_DIR/userland/drivers/net/ethernet/realtek/8139too.c"
COMMON_FLAGS="-static -O2 -Wall -Wextra -fno-stack-protector -I$ROOT_DIR/include -I$ROOT_DIR/userland/linux_compat/include"

rm -f "$TEMP_OUTPUT"
if "$CROSS_CC" $COMMON_FLAGS -I"$MUSL_ROOT/include" -L"$MUSL_ROOT/lib" \
     $COMMON_SOURCES -o "$TEMP_OUTPUT"; then
  mv "$TEMP_OUTPUT" "$OUTPUT"
  exit 0
fi

echo "[UDRIVER BUILD] bundled cross compiler unavailable; using host compiler with the local musl sysroot"
HOST_COMPILER=${CC:-cc}
"$HOST_COMPILER" -nostdlib -static -O2 -Wall -Wextra -fno-stack-protector \
  -isystem "$MUSL_ROOT/include" \
  -I"$ROOT_DIR/include" -I"$ROOT_DIR/userland/linux_compat/include" \
  "$MUSL_ROOT/lib/crt1.o" "$MUSL_ROOT/lib/crti.o" \
  $COMMON_SOURCES -L"$MUSL_ROOT/lib" -lc -lgcc -lgcc_eh \
  "$MUSL_ROOT/lib/crtn.o" -o "$TEMP_OUTPUT"
mv "$TEMP_OUTPUT" "$OUTPUT"
