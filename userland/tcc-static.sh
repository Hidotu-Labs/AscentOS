#!/bin/sh
# tcc-static: wrapper around tcc for producing musl-linked static binaries.
# Usage: tcc-static source.c -o output [extra tcc flags]
#
# TCC's internal linker cannot handle glibc's libc.a (R_X86_64_PC32/TPOFF32
# from -fPIC). This wrapper links against musl libc + libgcc instead.

MUSL_DIR=/opt/tcc/lib/musl
TCC_INC=/opt/tcc/lib/tcc/musl-include

exec tcc \
    -static \
    -nostdlib \
    -nostdinc \
    -I"$TCC_INC" \
    -I/opt/tcc/lib/tcc/include \
    "$MUSL_DIR/crt1.o" \
    "$MUSL_DIR/crti.o" \
    "$@" \
    "$MUSL_DIR/crtn.o" \
    "$MUSL_DIR/libc.a" \
    "$MUSL_DIR/libgcc.a" \
    "$MUSL_DIR/libgcc_eh.a" \
    /opt/tcc/lib/tcc/libtcc1.a
