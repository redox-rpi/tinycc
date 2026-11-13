#!/bin/bash
# Differential test with random programs (WBS 2.15 point 4, tinycc#3):
# csmith writes programs in the integer subset vc4-tcc supports; each is
# built by host gcc and by vc4-tcc, the vc4-tcc build runs in resim, and
# the checksums csmith's code prints must be the same.
# SPDX-License-Identifier: LGPL-2.1-or-later
#
#   tests/vc4/csmith-diff.sh <vc4-prefix> <vc4emul> [count]
#
#   CSMITH=path      csmith binary (default: csmith in PATH)
#   CSMITH_INC=dir   csmith's runtime headers (csmith.h; default: next to
#                    the binary, ../include or ../include/csmith-*)
#   SEED=n           first seed (default: from the date, so each run differs)
#   JOBS=n           parallel programs (default: nproc)
#   TIMEOUT=s        per run, host and resim (default 10)
#   KEEP=dir         keep mismatching and crashing programs there (default
#                    ./csmith-keep)
#   OUT=file         append the JSON record line there (it is always
#                    printed as the last line too)
#
# Counted: same, mismatch (different checksum, or the TCC build crashed in
# resim), unsupported (vc4-tcc rejects the program), timeout (host or resim
# over TIMEOUT), host (gcc fails or crashes: not our problem), gap (resim
# stopped on an instruction it does not know: a resim gap, not a TCC bug).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tcc_dir=$(cd "$here/../.." && pwd)
prefix=$1
emul=$2
count=${3:-100}
csmith=${CSMITH:-$(command -v csmith || true)}
[ -x "$csmith" ] || { echo "csmith not found (set CSMITH)" >&2; exit 2; }
if [ -z "${CSMITH_INC:-}" ]; then
	d=$(dirname "$(readlink -f "$csmith")")
	for c in "$d" "$d/../include" "$d"/../include/csmith-*; do
		[ -f "$c/csmith.h" ] && { CSMITH_INC=$(cd "$c" && pwd); break; }
	done
fi
[ -f "${CSMITH_INC:-}/csmith.h" ] || { echo "csmith.h not found (set CSMITH_INC)" >&2; exit 2; }
seed0=${SEED:-$(( $(date +%s) / 7 % 1000000000 ))}
jobs=${JOBS:-$(nproc)}
timeout=${TIMEOUT:-10}
keep=$(mkdir -p "${KEEP:-csmith-keep}" && cd "${KEEP:-csmith-keep}" && pwd)
newlib=$(dirname "$(dirname "$(command -v "${prefix}gcc")")")/vc4-elf/include
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT

# the integer subset: no float, no 64-bit types, no bit-fields or packed
# structs; struct arguments are rare, programs with them count as unsupported
opts="--no-float --no-longlong --no-math64 --no-bitfields --no-packed-struct"

"${prefix}gcc" -O1 -fno-jump-tables -c "$here/sim-rt.c" -o "$t/rt.o"

one() { # seed -> prints "<class> <seed>"
	local s=$1 d="$t/$1" r
	mkdir "$d"
	"$csmith" $opts --seed "$s" -o "$d/p.c" > /dev/null 2>&1 || { echo "host $s"; return; }
	if ! gcc -O0 -w -I"$CSMITH_INC" "$d/p.c" -o "$d/host" 2> /dev/null; then
		echo "host $s"; return
	fi
	r=0; timeout "$timeout" "$d/host" > "$d/host.out" 2> /dev/null || r=$?
	[ $r = 124 ] && { echo "timeout $s"; return; }
	grep -q '^checksum = ' "$d/host.out" || { echo "host $s"; return; }
	if ! "$tcc_dir/vc4-tcc" -B"$tcc_dir" -nostdinc -I"$tcc_dir/include" -I"$newlib" \
		-I"$CSMITH_INC" -w -c "$d/p.c" -o "$d/p.o" 2> "$d/tcc.err"; then
		if grep -q 'not supported' "$d/tcc.err" && ! grep -q 'internal' "$d/tcc.err"; then
			echo "unsupported $s"
		else # a compiler error on valid C is a bug
			cp "$d/p.c" "$keep/$s.c"; cp "$d/tcc.err" "$keep/$s.tcc-error"
			echo "mismatch $s"
		fi
		return
	fi
	"${prefix}gcc" -T vc4-sim.ld "$d/p.o" "$t/rt.o" -o "$d/p.elf" 2> /dev/null ||
		{ echo "unsupported $s"; return; }  # e.g. a missing libgcc helper
	"${prefix}objcopy" -O binary "$d/p.elf" "$d/mem.bin"
	truncate -s $((16 << 20)) "$d/mem.bin"
	r=0; timeout "$timeout" "$emul" "$d/mem.bin" 0 0 > "$d/vc4.out" 2> "$d/vc4.err" || r=$?
	[ $r = 124 ] && { echo "timeout $s"; return; }
	if ! grep -q '^checksum = ' "$d/vc4.out"; then
		if grep -qi 'unknown\|unimplemented\|not implemented' "$d/vc4.err"; then
			echo "gap $s"
		else
			cp "$d/p.c" "$keep/$s.c"; tail -20 "$d/vc4.err" > "$keep/$s.resim-error"
			echo "mismatch $s"
		fi
		return
	fi
	if [ "$(grep '^checksum = ' "$d/host.out")" = "$(grep '^checksum = ' "$d/vc4.out" | tr -d '\r')" ]; then
		echo "same $s"
	else
		cp "$d/p.c" "$keep/$s.c"
		{ grep '^checksum' "$d/host.out"; grep '^checksum' "$d/vc4.out"; } > "$keep/$s.checksums"
		echo "mismatch $s"
	fi
	rm -rf "$d"
}
export -f one
export t timeout keep prefix emul tcc_dir newlib csmith opts CSMITH_INC

seq "$seed0" $((seed0 + count - 1)) | xargs -P "$jobs" -I{} bash -c 'one {}' > "$t/results"

n() { grep -c "^$1 " "$t/results" || true; }
same=$(n same) mism=$(n mismatch) unsup=$(n unsupported) tmo=$(n timeout) host=$(n host) gap=$(n gap)
grep '^mismatch ' "$t/results" | sed 's/^/  /' >&2 || true
head=$(git -C "$tcc_dir" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
line=$(printf '{"kind":"csmith","date":"%s","week":"%s","tinycc":"%s","csmith":"%s","seed":%d,"programs":%d,"same":%d,"mismatches":%d,"unsupported":%d,"timeouts":%d,"host_failures":%d,"resim_gaps":%d,"kept":"%s"}' \
	"$(date -u +%Y-%m-%d)" "$(date -u +%G-W%V)" "$head" \
	"$("$csmith" --version 2>/dev/null | head -1 | tr -d '"\\')" \
	"$seed0" "$count" "$same" "$mism" "$unsup" "$tmo" "$host" "$gap" "$keep")
[ -z "${OUT:-}" ] || echo "$line" >> "$OUT"
echo "$line"
[ "$mism" = 0 ]
