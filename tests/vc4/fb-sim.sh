#!/bin/bash
# Speed and code generation test (B3s of tinycc#13; SPDX-License-Identifier:
# LGPL-2.1-or-later): the firmware's frame buffer pattern and CRC
# (fb_pixel, pattern_rgb, fb_pattern from bootcode/hvs.c, crc32 from
# bootcode/boot64.c), built by vc4-tcc and by vc4-elf-gcc -Os, run in the
# resim simulator with fb-sim-main.c. Both must print what a host build
# prints ("fb ok" and the CRCs of both frames).
#
#   tests/vc4/fb-sim.sh <rpi-open-firmware checkout> <vc4-prefix> <vc4emul>
#
# The checkout must have bootcode/hvs.c with fb_pattern (release-0.1 and
# later). BENCH=1 also prints the instructions each build needs (needs a
# vc4emul that prints "insns: N" when VC4EMUL_COUNT is set); vc4-tcc
# needed 1.83x GCC -Os before the peepholes of tinycc#16, about as on
# the Pi. KEEP=dir keeps the files there.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
fw=$(cd "$1" && pwd)
prefix=$2
emul=$3
newlib=$(dirname "$(dirname "$(command -v "${prefix}gcc")")")/vc4-elf/include
if [ -n "${KEEP:-}" ]; then t=$KEEP; mkdir -p "$t"; rm -rf "${t:?}"/*
else t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fi

# a top-level definition from its first line up to the "}" in column 0
take() { # file regex
	awk -v re="$2" '!on && $0 ~ re { on = 1 } on { print } on && /^}/ { exit }' "$1"
}
{
	echo '#include <stdint.h>'
	grep -m1 '^static const uint32_t g_bars' "$fw/bootcode/hvs.c"
	take "$fw/bootcode/hvs.c" '^uint32_t fb_pixel[(]'
	take "$fw/bootcode/hvs.c" '^static uint32_t pattern_rgb[(]'
	take "$fw/bootcode/hvs.c" '^uint32_t fb_pattern[(]'
	take "$fw/bootcode/boot64.c" '^uint32_t crc32[(]'
} > "$t/fb.c"
for f in g_bars fb_pixel pattern_rgb fb_pattern crc32; do
	grep -q "$f" "$t/fb.c" || { echo "fb-sim: $f not found in $fw/bootcode"; exit 1; }
done

cc -O1 -w -DFB_HOST -o "$t/host" "$t/fb.c" "$here/fb-sim-main.c"
"$t/host" > "$t/want"
grep -q '^fb ok ' "$t/want"

"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" -I"$newlib" \
	-c "$t/fb.c" -o "$t/fb-tcc.o"
"${prefix}gcc" -std=c11 -Os -ffreestanding -fno-builtin -fno-jump-tables \
	-c "$t/fb.c" -o "$t/fb-gcc.o"
"${prefix}gcc" -O1 -fno-jump-tables -c "$here/fb-sim-main.c" -o "$t/main.o"
"${prefix}gcc" -O1 -fno-jump-tables -c "$here/sim-rt.c" -o "$t/rt.o"
fail=0
for c in tcc gcc; do
	"${prefix}gcc" -T vc4-sim.ld "$t/main.o" "$t/rt.o" "$t/fb-$c.o" -o "$t/sim-$c.elf"
	"${prefix}objcopy" -O binary "$t/sim-$c.elf" "$t/mem.bin"
	truncate -s $((16 << 20)) "$t/mem.bin"
	if VC4EMUL_COUNT=1 timeout 600 "$emul" "$t/mem.bin" 0 0 > "$t/out-$c" 2> "$t/err-$c" \
		&& grep '^fb ' "$t/out-$c" | diff -u "$t/want" - > "$t/diff"; then
		echo "fb $c ok"
	else
		echo "fb $c FAIL"; fail=1
		tail -20 "$t/out-$c" "$t/err-$c" "$t/diff" 2>/dev/null || true
	fi
	[ -n "${BENCH:-}" ] && echo "bench fb $c: $(grep -o 'insns: [0-9]*' "$t/err-$c" || echo 'insns: n/a')"
done
exit $fail
