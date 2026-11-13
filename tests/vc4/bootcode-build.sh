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
#   LINK=tcc          link with vc4-tcc itself instead of vc4-elf-ld (needs
#                     every file through TCC and fat-boot a8bcbe7 or later,
#                     whose code uses __start_X/__stop_X symbols); the checks
#                     of linker.ld (ASSERT) are done on the -Wl,-Map output
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

# start.S first: its code is the image's first bytes
tccobjs=$(for f in $files; do echo "$out/$f-tcc.o"; done | sort -t/ -k99 |
	awk '/\/start\.S-tcc\.o$/ { print; next } { r = r $0 "\n" } END { printf "%s", r }')
if [ "${LINK:-ld}" = tcc ]; then
	[ -z "$objs" ] || { echo "LINK=tcc needs every file through TCC, left: $objs" >&2; exit 1; }
	"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdlib -static -Wl,-Ttext=0x80000000 \
		-Wl,--section-alignment=4 -Wl,--oformat=binary \
		-Wl,-Map="$out/bootcode-tcc.map" -o "$out/bootcode-tcc.bin" $tccobjs
	# linker.ld's checks, on the map
	sym() { awk -v n="$1" '$3 == n && $1 != "S" { print $1 }' "$out/bootcode-tcc.map"; }
	sec() { awk -v n="$1" '$1 == "S" && $4 == n { print $2 }' "$out/bootcode-tcc.map"; }
	for n in __start_bss __stop_bss __start_tramp __stop_tramp; do
		[ -n "$(sym $n)" ] || { echo "map: $n missing" >&2; exit 1; }
	done
	[ "$(sec .text.start)" = 80000000 ] ||
		{ echo "map: .text.start not at 0x80000000" >&2; exit 1; }
	(( 16#$(sym __stop_bss) <= 0x80018000 )) ||
		{ echo "bootcode leaves less than 12 KB of stack" >&2; exit 1; }
	(( 16#$(sym __stop_tramp) - 16#$(sym __start_tramp) <= 0x800 )) ||
		{ echo "chain trampoline over 2 KB" >&2; exit 1; }
	echo "linked by vc4-tcc: .text.start at 0x80000000, bss ends 0x$(sym __stop_bss), trampoline $(( 16#$(sym __stop_tramp) - 16#$(sym __start_tramp) )) bytes"
else
	(cd build && "${prefix}gcc" -nostdlib -nostartfiles -Wl,--build-id=none \
		-T ../linker.ld $objs $tccobjs -lgcc -o "$out/bootcode-tcc.elf")
	"${prefix}objcopy" -O binary "$out/bootcode-tcc.elf" "$out/bootcode-tcc.bin"
fi
printf 'bootcode.bin (MONITOR=1): GCC %d bytes, with TCC objects %d bytes\n' \
	"$(stat -c %s build/bootcode.bin)" "$(stat -c %s "$out/bootcode-tcc.bin")"
if [ -n "${KEEP:-}" ]; then
	mkdir -p "$KEEP"
	cp "$out"/* "$KEEP/"
	echo "kept in $KEEP"
fi
