#!/bin/bash
# Differential test: every tests/tests2 program, compiled by this vc4-tcc
# and by a reference vc4-tcc (an older build), runs in resim; the outputs
# must be the same. Programs that the reference cannot compile or run
# (floats, long long division, ...) are skipped.
# SPDX-License-Identifier: LGPL-2.1-or-later
#
#   tests/vc4/tests2-diff.sh <vc4-prefix> <vc4emul> <reference tcc dir>
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
prefix=$1
emul=$2
ref=$(cd "$3" && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT

"${prefix}gcc" -O1 -fno-jump-tables -c "$here/sim-rt.c" -o "$t/rt.o"
run() { # tcc-dir src out
	"$1/vc4-tcc" -B"$1" -I"$1/include" -w -c "$2" -o "$t/p.o" 2> /dev/null || return 1
	"${prefix}gcc" -T vc4-sim.ld "$t/p.o" "$t/rt.o" -o "$t/p.elf" 2> /dev/null || return 1
	"${prefix}objcopy" -O binary "$t/p.elf" "$t/mem.bin"
	truncate -s $((16 << 20)) "$t/mem.bin"
	timeout 30 "$emul" "$t/mem.bin" 0 0 > "$3" 2> /dev/null || true
}
pass=0 fail=0 skip=0 exact=0
for f in "$tcc_dir"/tests/tests2/*.c; do
	n=$(basename "$f" .c)
	if ! run "$ref" "$f" "$t/ref.out" || [ ! -s "$t/ref.out" ]; then
		skip=$((skip + 1)); continue
	fi
	if run "$tcc_dir" "$f" "$t/new.out" && cmp -s "$t/ref.out" "$t/new.out"; then
		pass=$((pass + 1))
		# also the same as on the host (sim-rt's printf is small)
		if [ -f "${f%.c}.expect" ] && cmp -s "${f%.c}.expect" "$t/new.out"; then
			exact=$((exact + 1))
		fi
	else
		fail=$((fail + 1)); echo "DIFF $n"
		diff "$t/ref.out" "$t/new.out" 2>/dev/null | head -6 || true
	fi
done
echo "tests2 diff: $pass same ($exact also as .expect), $fail different, $skip skipped"
[ $fail = 0 ]
