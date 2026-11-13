# Pinned inputs of the vc4 CI (SPDX-License-Identifier: LGPL-2.1-or-later).
# Sourced by the scripts in tests/vc4/ci/ and by .github/workflows/vc4*.yml.

# vc4-elf binutils, GCC 6.2.1 and newlib (Julian Brown's port, as in
# rpi-open-firmware redox-rpi/build-toolchain.sh).
VC4_TOOLCHAIN_REPO=https://github.com/itszor/vc4-toolchain.git
VC4_TOOLCHAIN_REV=2f57f093ef6bc77470152f00e98f38843e8fbbc3
# Release of this repository that holds the built toolchain and its source.
# A new toolchain build gets a new tag (bump the last number).
VC4_TOOLCHAIN_TAG=vc4-toolchain-6.2.1-2f57f09-1
VC4_TOOLCHAIN_ASSET=vc4-elf-gcc-6.2.1-2f57f09-ubuntu-24.04-x86_64.tar.zst
VC4_TOOLCHAIN_SHA256=ac4564e681e0fcc93fc45a08c9f5320654d97d3753f1efb7e4f2f1aad7d25854

# rpi-open-firmware branch fat-boot, for fat-sim.sh and bootcode-build.sh.
FAT_BOOT_REPO=redox-rpi/rpi-open-firmware
FAT_BOOT_REV=a8bcbe7a634661fea5075027ed6edb5c3a4c635c
