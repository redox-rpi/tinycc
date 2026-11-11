#!/bin/bash
# Spike check for the VC4 backend (SPDX-License-Identifier: LGPL-2.1-or-later):
# compile the firmware's bootcode/fat.c with vc4-tcc and with vc4-elf-gcc -Os,
# compare the sizes, and link the TCC object into the real bootcode build
# (MONITOR=1) in place of GCC's.
#
#   tests/vc4/fat-build.sh <rpi-open-firmware checkout> [vc4-prefix]
#
# The checkout needs bootcode/ (branch fat-boot or later) and the upstream
# headers at the top (hardware.h, vc4_include/); bootcode's Makefile looks
# for them in ../firmware, so the script adds a firmware -> . link if needed.
# Uses newlib's stdint.h from the vc4 toolchain (TCC ships none).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
fw=$(cd "$1" && pwd)
prefix=${2:-vc4-elf-}
newlib=$(dirname "$(dirname "$(command -v "${prefix}gcc")")")/vc4-elf/include
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

[ -e "$fw/firmware" ] || ln -s . "$fw/firmware"
cd "$fw/bootcode"
make -s MONITOR=1 CROSS_COMPILE="$prefix" >/dev/null
"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" -I"$newlib" \
	-I. -I.. -I../vc4_include -D__VIDEOCORE4__ -DMONITOR=1 \
	-c fat.c -o "$out/fat-tcc.o"
echo "fat.c, text/data/bss (GCC -Os, then TCC):"
"${prefix}size" build/fat.c.o "$out/fat-tcc.o"

objs=$(cd build && ls *.o | grep -v '^fat\.c\.o$')
(cd build && "${prefix}gcc" -nostdlib -nostartfiles -Wl,--build-id=none \
	-T ../linker.ld $objs "$out/fat-tcc.o" -lgcc -o "$out/bootcode-tcc.elf")
"${prefix}objcopy" -O binary "$out/bootcode-tcc.elf" "$out/bootcode-tcc.bin"
printf 'bootcode.bin (MONITOR=1): GCC %d bytes, with TCC fat.c %d bytes\n' \
	"$(stat -c %s build/bootcode.bin)" "$(stat -c %s "$out/bootcode-tcc.bin")"
if [ -n "${KEEP:-}" ]; then
	cp "$out/bootcode-tcc.elf" "$out/bootcode-tcc.bin" "$out/fat-tcc.o" "$KEEP/"
	echo "kept in $KEEP"
fi
