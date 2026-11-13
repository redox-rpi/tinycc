#!/bin/bash
# Reduce a program that csmith-diff.sh kept (different checksum) with
# creduce, to a small case for an issue and a test in tests/vc4.
# SPDX-License-Identifier: LGPL-2.1-or-later
#
#   tests/vc4/csmith-reduce.sh <vc4-prefix> <vc4emul> <kept.c> [out.c]
#
# The test creduce runs keeps a candidate only if host gcc builds it with
# -Wall's uninitialized/return warnings as errors, it runs clean under
# UBSan, vc4-tcc builds it, it prints "checksum =" in resim, and the two
# checksums differ. Needs creduce and CSMITH_INC as for csmith-diff.sh.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
prefix=$1 emul=$2 src=$(readlink -f "$3") out=${4:-${3%.c}.min.c}
inc=${CSMITH_INC:-/usr/include/csmith}
newlib=$(dirname "$(dirname "$(command -v "${prefix}gcc")")")/vc4-elf/include
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
"${prefix}gcc" -O1 -fno-jump-tables -c "$here/sim-rt.c" -o "$w/rt.o"
cat > "$w/test.sh" <<EOF
#!/bin/bash
defs="-DNO_LONGLONG -DUSE_MATH_MACROS"
gcc -O0 -w -Werror=uninitialized -Werror=return-type -Werror=implicit-int \\
	-Werror=implicit-function-declaration -fsanitize=undefined \\
	-fno-sanitize-recover=all \$defs -I$inc p.c -o host 2>/dev/null || exit 1
timeout 10 ./host > host.out 2>/dev/null || exit 1
grep -q '^checksum = ' host.out || exit 1
$tcc_dir/vc4-tcc -B$tcc_dir -nostdinc -I$tcc_dir/include -I$newlib -I$inc \$defs \\
	-w -c p.c -o p.o 2>/dev/null || exit 1
${prefix}gcc -T vc4-sim.ld p.o $w/rt.o -o p.elf 2>/dev/null || exit 1
${prefix}objcopy -O binary p.elf mem.bin && truncate -s 16M mem.bin
timeout 10 $emul mem.bin 0 0 > vc4.out 2>/dev/null || exit 1
grep -q '^checksum = ' vc4.out || exit 1
[ "\$(grep '^checksum' host.out)" != "\$(grep '^checksum' vc4.out | tr -d '\\r')" ]
EOF
chmod +x "$w/test.sh"
cp "$src" "$w/p.c"
(cd "$w" && ./test.sh) || { echo "$3: not interesting as it is (UBSan, or no difference)" >&2; exit 1; }
(cd "$w" && creduce --n "$(nproc)" ./test.sh p.c > creduce.log 2>&1)
cp "$w/p.c" "$out"
echo "reduced: $out ($(wc -l < "$out") lines)"
