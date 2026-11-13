#!/bin/bash
# Assembler test for vc4-tcc (SPDX-License-Identifier: LGPL-2.1-or-later):
# asm-test.S through vc4-elf-as (after cpp) and through vc4-tcc; both
# objects are linked at the same addresses, and the bytes must be the same.
#
#   tests/vc4/asm-test.sh <vc4-prefix>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
prefix=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT

printf 'incbin\001\002' > "$t/inc.bin"
"${prefix}gcc" -E -P -x assembler-with-cpp -DINCFILE="\"$t/inc.bin\"" \
	"$here/asm-test.S" > "$t/gas.s"
"${prefix}as" "$t/gas.s" -o "$t/gas.o"
"$tcc_dir/vc4-tcc" -B"$tcc_dir" -DINCFILE="\"$t/inc.bin\"" \
	-c "$here/asm-test.S" -o "$t/tcc.o"
link() {
	"${prefix}ld" -Ttext=0x80000000 -Tdata=0x80010000 -e start \
		--defsym far_away=0x80123456 "$1.o" -o "$1.elf"
	"${prefix}objdump" -s -j .text -j .data "$1.elf" | tail -n +4
}
link "$t/gas" > "$t/gas.txt"
link "$t/tcc" > "$t/tcc.txt"
if diff -u "$t/gas.txt" "$t/tcc.txt"; then
	echo "asm ok ($(grep -c . "$t/gas.txt") lines of dump)"
else
	"${prefix}objdump" -d "$t/tcc.elf" > "$t/tcc.dis"
	"${prefix}objdump" -d "$t/gas.elf" > "$t/gas.dis"
	diff "$t/gas.dis" "$t/tcc.dis" | head -40
	echo "asm FAIL"; exit 1
fi
