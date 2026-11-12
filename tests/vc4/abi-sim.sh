#!/bin/bash
# ABI and code generation test (SPDX-License-Identifier: LGPL-2.1-or-later):
# abi-test.c built by the host compiler and run natively must print the same
# as abi-test.c built by vc4-tcc and run in resim with abi-main.c.
#
#   tests/vc4/abi-sim.sh <vc4-prefix> <vc4emul>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
prefix=$1
emul=$2
if [ -n "${KEEP:-}" ]; then t=$KEEP; mkdir -p "$t"; rm -rf "${t:?}"/*
else t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fi

cat > "$t/host-main.c" <<'C'
#include <stdio.h>
void run_tests(void);
int sum(int n, ...);
int main(void) { run_tests(); printf("gcc-calls-sum %d\n", sum(7, 7, 6, 5, 4, 3, 2, 1)); return 0; }
C
cc -O1 -funsigned-char -w -o "$t/host" "$here/abi-test.c" "$t/host-main.c"
"$t/host" > "$t/want"

"$tcc_dir/vc4-tcc" -B"$tcc_dir" -I"$tcc_dir/include" -c "$here/abi-test.c" -o "$t/abi-tcc.o"
"${prefix}gcc" -O1 -fno-jump-tables -c "$here/abi-main.c" -o "$t/main.o"
"${prefix}gcc" -O1 -fno-jump-tables -c "$here/sim-rt.c" -o "$t/rt.o"
"${prefix}gcc" -T vc4-sim.ld "$t/main.o" "$t/rt.o" "$t/abi-tcc.o" -o "$t/abi.elf"
"${prefix}objcopy" -O binary "$t/abi.elf" "$t/abi.bin"
timeout 120 "$emul" "$t/abi.bin" 0 0 > "$t/got" 2> "$t/err" || { cat "$t/err"; }
if diff -u "$t/want" "$t/got"; then
	echo "abi ok ($(wc -l < "$t/want") lines)"
else
	echo "abi FAIL"; exit 1
fi

# the same with everything through vc4-tcc, linked by TCC's own linker
newlib=$(dirname "$(dirname "$(command -v "${prefix}gcc")")")/vc4-elf/include
tcc_cc() { "$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" -I"$newlib" "$@"; }
for f in sim-crt.S abi-main.c sim-rt.c sim-libc.c; do
	tcc_cc -c "$here/$f" -o "$t/$f-all.o"
done
"$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdlib -static -Wl,-Ttext=0 \
	-Wl,--section-alignment=4 -Wl,--oformat=binary -o "$t/abi-all.bin" \
	"$t/sim-crt.S-all.o" "$t/abi-main.c-all.o" "$t/abi-tcc.o" \
	"$t/sim-rt.c-all.o" "$t/sim-libc.c-all.o"
timeout 120 "$emul" "$t/abi-all.bin" 0 0 > "$t/got-all" 2> "$t/err-all" || { cat "$t/err-all"; }
if diff -u "$t/want" "$t/got-all"; then
	echo "abi all-tcc ok (compiled, assembled and linked by vc4-tcc)"
else
	echo "abi all-tcc FAIL"; exit 1
fi
