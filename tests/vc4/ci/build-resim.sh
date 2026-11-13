#!/bin/bash
# Build vc4emul from resim at the pinned commit with our patch
# (SPDX-License-Identifier: LGPL-2.1-or-later). resim.patch: the asr fix
# (`||` for `|`), OP_BCHG (bitflip, for GCC's fat.c), and the instruction
# counter (VC4EMUL_COUNT) and pc histogram (VC4EMUL_PROF) of vc4emul.
#
#   tests/vc4/ci/build-resim.sh <work dir>     prints the vc4emul path
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/pins.sh"
work="$(mkdir -p "$1" && cd "$1" && pwd)"
src="$work/resim"
[ -d "$src" ] || git clone -q "$RESIM_REPO" "$src"
git -C "$src" checkout -q "$RESIM_REV"
git -C "$src" apply "$here/resim.patch"
# resim predates <cstdint>/<cstring> being pulled in by other headers.
printf '#!/bin/sh\nexec g++ -include cstdint -include cstring -include cassert "$@"\n' > "$work/gxx"
chmod +x "$work/gxx"
cmake -S "$src" -B "$work/resim-build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_CXX_COMPILER="$work/gxx" -DCMAKE_BUILD_TYPE=Release > "$work/resim-cmake.log" 2>&1
cmake --build "$work/resim-build" -j"$(nproc)" --target vc4emul > "$work/resim-build.log" 2>&1
echo "$work/resim-build/vc4emul/vc4emul"
