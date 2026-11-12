#!/bin/bash
# Functional test of the VC4 backend (SPDX-License-Identifier: LGPL-2.1-or-later):
# the firmware's bootcode/fat.c, compiled by vc4-tcc, reads FAT16 and FAT32
# card images in the resim simulator (vc4emul); every loaded file must match
# the original. The expected sizes and checksums come from the source files.
# A third run builds everything (driver, runtime, start file, fat.c) with
# vc4-tcc alone and links it with TCC's own linker ("all").
# The same run with GCC's fat.c is for information only: resim lacks some
# instructions GCC uses (switch, bitflip with an immediate), so a GCC run
# that stops on one of those is reported as n/a, not as a failure.
#
#   tests/vc4/fat-sim.sh <rpi-open-firmware checkout> <vc4-prefix> <vc4emul>
#
# Needs mtools and sfdisk (as bootcode/tests/fat-test.sh). The memory image
# for vc4emul is: test program at 0, card image at 16 MiB (CARD in
# fat-sim-main.c).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
fw=$(cd "$1" && pwd)
prefix=$2
emul=$3
newlib=$(dirname "$(dirname "$(command -v "${prefix}gcc")")")/vc4-elf/include
# KEEP=dir keeps the programs, images and simulator output there
# BENCH=1 also prints how many instructions fat.c from GCC and from TCC
#   needs to load the files from the FAT32 image (no checksums; needs a
#   vc4emul that prints "insns: N" when VC4EMUL_COUNT is set)
if [ -n "${KEEP:-}" ]; then t=$KEEP; mkdir -p "$t"; rm -rf "${t:?}"/*
else t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fi
inc=(-I"$fw/bootcode" -I"$fw" -I"$fw/vc4_include" -D__VIDEOCORE4__ -DMONITOR=1)

"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" -I"$newlib" \
	"${inc[@]}" -c "$fw/bootcode/fat.c" -o "$t/fat-tcc.o"
"${prefix}gcc" -std=c11 -Os -ffreestanding -fno-builtin "${inc[@]}" \
	-fno-jump-tables -c "$fw/bootcode/fat.c" -o "$t/fat-gcc.o"
"${prefix}gcc" -O1 -fno-jump-tables "${inc[@]}" -c "$here/fat-sim-main.c" -o "$t/main.o"
"${prefix}gcc" -O1 -fno-jump-tables -c "$here/sim-rt.c" -o "$t/rt.o"
for c in tcc gcc; do
	"${prefix}gcc" -T vc4-sim.ld "$t/main.o" "$t/rt.o" "$t/fat-$c.o" -o "$t/sim-$c.elf"
	"${prefix}objcopy" -O binary "$t/sim-$c.elf" "$t/sim-$c.bin"
	[ "$(stat -c %s "$t/sim-$c.bin")" -lt $((16 << 20)) ]
done
# everything through vc4-tcc, linked by TCC
tcc_cc() { "$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" -I"$newlib" "$@"; }
tcc_cc -c "$here/sim-crt.S" -o "$t/crt-all.o"
tcc_cc "${inc[@]}" -c "$here/fat-sim-main.c" -o "$t/main-all.o"
tcc_cc -c "$here/sim-rt.c" -o "$t/rt-all.o"
tcc_cc -c "$here/sim-libc.c" -o "$t/libc-all.o"
"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdlib -static -Wl,-Ttext=0 \
	-Wl,--section-alignment=4 -Wl,--oformat=binary -o "$t/sim-all.bin" \
	"$t/crt-all.o" "$t/main-all.o" "$t/rt-all.o" "$t/libc-all.o" "$t/fat-tcc.o"

mkdir "$t/src"
head -c 3000000 /dev/urandom > "$t/src/Big-File.Data"
head -c 256 /dev/urandom > "$t/src/armstub8-redox.bin"
head -c 70000 /dev/urandom > "$t/src/u-boot.bin"
echo "arm_64bit=1" > "$t/src/config.txt"
want() { # name-on-card source-file
	printf 'RESULT %s %d %s\n' "$1" "$(stat -c %s "$2")" \
		"$(python3 -I -c 'import sys
h=2166136261
for b in open(sys.argv[1],"rb").read(): h=((h^b)*16777619)&0xffffffff
print("%08x"%h)' "$2")"
}
{
	want big-file.data "$t/src/Big-File.Data"
	want armstub8-redox.bin "$t/src/armstub8-redox.bin"
	want U-BOOT.BIN "$t/src/u-boot.bin"
	want CONFIG.TXT "$t/src/config.txt"
	echo "RESULT no-such-file.txt missing"
} > "$t/want"

fail=0
for fat in 16 32; do
	secs=$([ $fat = 16 ] && echo 32768 || echo 70000)
	rm -f "$t/p.img"; truncate -s $((secs * 512)) "$t/p.img"
	if [ $fat = 16 ]; then mformat -i "$t/p.img" -T $secs -h 64 -s 32 -c 4 ::
	else mformat -i "$t/p.img" -T $secs -h 64 -s 32 -F -c 1 ::; fi
	for f in "$t"/src/*; do mcopy -i "$t/p.img" "$f" "::$(basename "$f")"; done
	rm -f "$t/d.img"; truncate -s 1M "$t/d.img"; cat "$t/p.img" >> "$t/d.img"
	printf 'label: dos\nstart=2048, type=%s\n' "$([ $fat = 16 ] && echo 6 || echo c)" | sfdisk -q "$t/d.img"
	for c in gcc tcc all; do
		cp "$t/sim-$c.bin" "$t/mem.bin"
		truncate -s $((16 << 20)) "$t/mem.bin"
		cat "$t/d.img" >> "$t/mem.bin"
		if timeout 600 "$emul" "$t/mem.bin" 0 0 > "$t/out-$c" 2> "$t/err-$c" \
			&& grep '^RESULT' "$t/out-$c" | diff -u "$t/want" - > "$t/diff"; then
			echo "FAT$fat $c ok"
		elif [ $c = gcc ] && grep -q 'Exception: Un' "$t/err-$c"; then
			echo "FAT$fat $c n/a ($(grep -m1 Exception "$t/err-$c"))"
		else
			echo "FAT$fat $c FAIL"; fail=1
			tail -20 "$t/out-$c" "$t/err-$c" "$t/diff" 2>/dev/null || true
		fi
	done
done
if [ -n "${BENCH:-}" ]; then
	"${prefix}gcc" -O1 -fno-jump-tables -DFAT_SIM_BENCH "${inc[@]}" \
		-c "$here/fat-sim-main.c" -o "$t/main-bench.o"
	for c in gcc tcc; do
		"${prefix}gcc" -T vc4-sim.ld "$t/main-bench.o" "$t/rt.o" "$t/fat-$c.o" -o "$t/bench-$c.elf"
		"${prefix}objcopy" -O binary "$t/bench-$c.elf" "$t/mem.bin"
		truncate -s $((16 << 20)) "$t/mem.bin"
		cat "$t/d.img" >> "$t/mem.bin"
		VC4EMUL_COUNT=1 timeout 600 "$emul" "$t/mem.bin" 0 0 > /dev/null 2> "$t/err-bench-$c" || true
		echo "bench FAT32 fat.c $c: $(grep -o 'insns: [0-9]*' "$t/err-bench-$c")"
	done
fi
exit $fail
