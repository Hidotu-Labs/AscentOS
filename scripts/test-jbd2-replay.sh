#!/bin/sh
set -eu

iso=${1:-ascentos-x86_64.iso}
image=${JBD2_TEST_IMAGE:-/tmp/ascentos-jbd2-replay.img}
log=${JBD2_TEST_LOG:-/tmp/ascentos-jbd2-replay.log}
mkfs=${MKFS_EXT4:-/run/host/usr/bin/mkfs.ext4}
debugfs=${DEBUGFS:-/run/host/usr/bin/debugfs}
qemu=${QEMU:-/run/host/usr/bin/qemu-system-x86_64}

for tool in dd "$mkfs" "$debugfs" "$qemu" timeout xxd; do
  command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; exit 1; }
done
[ -f "$iso" ] || { echo "missing ISO: $iso" >&2; exit 1; }

rm -f "$image" "$log" /tmp/ascentos-jbd2-payload
dd if=/dev/zero of="$image" bs=1M count=64 status=none
"$mkfs" -F -b 1024 -I 128 -O extent,filetype,has_journal,^dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file "$image" >/dev/null
dd if=/dev/zero of=/tmp/ascentos-jbd2-payload bs=1024 count=1 status=none
printf JBD2-RECOVERY-PASS > /tmp/ascentos-jbd2-payload
"$debugfs" -w -R "write /tmp/ascentos-jbd2-payload /jbd2-target" "$image" >/dev/null 2>&1
target=$("$debugfs" -R "stat /jbd2-target" "$image" 2>/dev/null | sed -n "s/.*(0): \([0-9]*\).*/\1/p")
journal=$("$debugfs" -R "stat <8>" "$image" 2>/dev/null | sed -n "s/.*(0-12): \([0-9]*\).*/\1/p")
[ -n "$target" ] && [ -n "$journal" ] || { echo "cannot resolve test or journal blocks" >&2; exit 1; }
desc=$((journal + 1))
data=$((journal + 2))
commit=$((journal + 3))
dd if=/dev/zero of="$image" bs=1024 count=1 seek="$desc" conv=notrunc status=none
printf c03b39980000000100000001 | xxd -r -p | dd of="$image" bs=1 seek=$((desc * 1024)) conv=notrunc status=none
printf %08x00000008 "$target" | xxd -r -p | dd of="$image" bs=1 seek=$((desc * 1024 + 12)) conv=notrunc status=none
dd if=/tmp/ascentos-jbd2-payload of="$image" bs=1024 count=1 seek="$data" conv=notrunc status=none
dd if=/dev/zero of="$image" bs=1024 count=1 seek="$commit" conv=notrunc status=none
printf c03b39980000000200000001 | xxd -r -p | dd of="$image" bs=1 seek=$((commit * 1024)) conv=notrunc status=none
printf 00000001 | xxd -r -p | dd of="$image" bs=1 seek=$((journal * 1024 + 28)) conv=notrunc status=none
if [ "$qemu" = /run/host/usr/bin/qemu-system-x86_64 ]; then
  export LD_LIBRARY_PATH=/run/host/usr/lib64:/run/host/usr/lib
fi
set +e
timeout 20s "$qemu" -M q35 -m 2G -cdrom "$iso" -drive file="$image",format=raw,if=ide -serial file:"$log" -display none -no-reboot
status=$?
set -e
[ "$status" -eq 0 ] || [ "$status" -eq 124 ] || { cat "$log"; exit "$status"; }
grep -F "[EXT3] Journal recovery complete." "$log" >/dev/null
dd if="$image" bs=1024 skip="$target" count=1 status=none | cmp -s /tmp/ascentos-jbd2-payload -
echo "JBD2 replay test passed"
