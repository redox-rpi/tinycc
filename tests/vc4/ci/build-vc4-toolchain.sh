#!/bin/bash
# Build the vc4-elf toolchain (binutils, GCC 6.2.1 C only, newlib) at the
# pinned vc4-toolchain commit and pack it for a release of this repository
# (SPDX-License-Identifier: LGPL-2.1-or-later). Same configure options as
# rpi-open-firmware redox-rpi/build-toolchain.sh, but C only.
#
#   tests/vc4/ci/build-vc4-toolchain.sh <work dir>
#
# Writes <work dir>/$VC4_TOOLCHAIN_ASSET (prefix with bin/vc4-elf-*) and
# <work dir>/vc4-toolchain-src-$VC4_TOOLCHAIN_REV.tar.zst: the complete
# source it was built from (GPL: the source goes with the binary).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/pins.sh"
work="$(mkdir -p "$1" && cd "$1" && pwd)"
prefix="$work/prefix"
jobs="$(nproc)"
infra=https://gcc.gnu.org/pub/gcc/infrastructure
GMP=gmp-6.1.0 MPFR=mpfr-3.1.4 MPC=mpc-1.0.3
cflags="-O2 -std=gnu11" cxxflags="-O2 -std=gnu++98"
export PATH="$prefix/bin:$PATH"

src="$work/src"
mkdir -p "$src"
V="$src/vc4-toolchain"
[ -d "$V" ] || git clone -q "$VC4_TOOLCHAIN_REPO" "$V"
git -C "$V" checkout -q "$VC4_TOOLCHAIN_REV"
git -C "$V" submodule update --init --depth 1 binutils-vc4 gcc-vc4 newlib
for t in $GMP.tar.bz2 $MPFR.tar.bz2 $MPC.tar.gz; do
    [ -f "$src/$t" ] || curl -sSfL -o "$src/$t" "$infra/$t"
done
(cd "$src" && sha256sum -c - <<'SUMS'
498449a994efeba527885c10405993427995d3f86b8768d8cdf8d9dd7c6b73e8  gmp-6.1.0.tar.bz2
617decc6ea09889fb08ede330917a00b16809b8db88c29c31bfbb49cbf88ecc3  mpc-1.0.3.tar.gz
d3103a80cdad2407ed581f3618c4bed04e0c92d1cf771a65ead662cc397f7775  mpfr-3.1.4.tar.bz2
SUMS
)
for p in $GMP $MPFR $MPC; do
    [ -d "$V/gcc-vc4/${p%%-*}" ] && continue
    tar -C "$src" -xf "$src/$p".tar.*
    ln -sfn "$src/$p" "$V/gcc-vc4/${p%%-*}"
done

common="--target=vc4-elf --prefix=$prefix --disable-nls --disable-werror"
b() { rm -rf "$work/build-$1"; mkdir -p "$work/build-$1"; cd "$work/build-$1"; }
b binutils
CFLAGS="$cflags" CXXFLAGS="$cxxflags" "$V/binutils-vc4/configure" $common > ../binutils.log
make -j"$jobs" >> ../binutils.log 2>&1 && make install >> ../binutils.log 2>&1
b gcc1
CFLAGS="$cflags" CXXFLAGS="$cxxflags" "$V/gcc-vc4/configure" $common \
    --disable-ssp --enable-languages=c --without-headers --with-newlib \
    --disable-decimal-float --disable-threads --disable-libatomic \
    --disable-libitm --disable-libsanitizer --disable-libquadmath \
    --disable-lto --enable-sjlj-exceptions > ../gcc1.log
make -j"$jobs" >> ../gcc1.log 2>&1 && make install >> ../gcc1.log 2>&1
b newlib
"$V/newlib/configure" --target=vc4-elf --prefix="$prefix" > ../newlib.log
make -j"$jobs" >> ../newlib.log 2>&1 && make install >> ../newlib.log 2>&1
b gcc2
CFLAGS="$cflags" CXXFLAGS="$cxxflags" "$V/gcc-vc4/configure" $common \
    --disable-ssp --enable-languages=c --disable-decimal-float \
    --enable-threads --disable-libatomic --disable-libsanitizer \
    --disable-libquadmath --disable-lto --enable-sjlj-exceptions --with-newlib > ../gcc2.log
make -j"$jobs" >> ../gcc2.log 2>&1 && make install >> ../gcc2.log 2>&1

cd "$work"
"$prefix/bin/vc4-elf-gcc" --version | head -1
{
    echo "vc4-toolchain $VC4_TOOLCHAIN_REV ($VC4_TOOLCHAIN_REPO), built by tests/vc4/ci/build-vc4-toolchain.sh"
    git -C "$V" submodule status
} > "$prefix/BUILD-INFO.txt"
tar -C "$work" -cf - prefix | zstd -q -19 -T0 -o "$work/$VC4_TOOLCHAIN_ASSET"
rm -rf "$V"/*/.git "$V"/.git
tar -C "$src" -cf - vc4-toolchain $GMP.tar.bz2 $MPFR.tar.bz2 $MPC.tar.gz |
    zstd -q -19 -T0 -o "$work/vc4-toolchain-src-$VC4_TOOLCHAIN_REV.tar.zst"
ls -la "$work"/*.tar.zst
