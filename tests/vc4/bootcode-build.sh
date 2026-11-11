#!/bin/bash
# VC4 backend check on real firmware code (SPDX-License-Identifier:
# LGPL-2.1-or-later): compile C files of the firmware's bootcode/ with
# vc4-tcc and with vc4-elf-gcc -Os, compare the sizes, and link the TCC
# objects into the real bootcode build (MONITOR=1) in place of GCC's.
#
#   tests/vc4/bootcode-build.sh <rpi-open-firmware checkout> [vc4-prefix]
#
#   TCC_FILES="fat"   which files TCC compiles (default: every C file and
#                     every .S file, so only the link is left to vc4-elf-ld;
#                     names with .S are assembled, e.g. "fat start.S")
#   KEEP=dir          keep bootcode-tcc.{elf,bin} and the TCC objects there
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
files=${TCC_FILES:-$(cd build && ls *.c.o *.S.o | sed 's/\.c\.o$//; s/\.o$//')}

text() { "${prefix}size" "$1" | awk 'NR==2 { print $1 }'; }
printf '%-10s %8s %8s %6s\n' file "GCC -Os" TCC factor
gt=0 tt=0 objs=$(cd build && ls *.c.o *.S.o)
for f in $files; do
	case $f in
	*.S)	src=$f obj=build/$f.o
		"$tcc_dir/vc4-tcc" -B"$tcc_dir" -I. -I.. -I../vc4_include \
			-D__VIDEOCORE4__ -c "$f" -o "$out/$f-tcc.o" ;;
	*)	src=$f.c obj=build/$f.c.o
		"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" \
			-I"$newlib" -I. -I.. -I../vc4_include -D__VIDEOCORE4__ \
			-DMONITOR=1 -DBUILD_ID='"tcc"' -c "$src" -o "$out/$f-tcc.o" ;;
	esac
	g=$(text "$obj") t=$(text "$out/$f-tcc.o")
	gt=$((gt + g)) tt=$((tt + t))
	awk -v f="$f" -v g="$g" -v t="$t" \
		'BEGIN { printf "%-10s %8d %8d %6.2f\n", f, g, t, t / g }'
	objs=$(echo "$objs" | grep -vx "${obj#build/}" || true)
done
awk -v g="$gt" -v t="$tt" 'BEGIN { printf "%-10s %8d %8d %6.2f\n", "total", g, t, t / g }'

tccobjs=$(for f in $files; do echo "$out/$f-tcc.o"; done)
(cd build && "${prefix}gcc" -nostdlib -nostartfiles -Wl,--build-id=none \
	-T ../linker.ld $objs $tccobjs -lgcc -o "$out/bootcode-tcc.elf")
"${prefix}objcopy" -O binary "$out/bootcode-tcc.elf" "$out/bootcode-tcc.bin"
printf 'bootcode.bin (MONITOR=1): GCC %d bytes, with TCC objects %d bytes\n' \
	"$(stat -c %s build/bootcode.bin)" "$(stat -c %s "$out/bootcode-tcc.bin")"
if [ -n "${KEEP:-}" ]; then
	mkdir -p "$KEEP"
	cp "$out"/* "$KEEP/"
	echo "kept in $KEEP"
fi
