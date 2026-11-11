# TinyCC with a VideoCore IV (VPU) backend

Fork of TinyCC `mob` (base 70adbb12) for the redox-rpi open Pi firmware.
Plan, decisions and milestones: `docs/tcc-vc4.md` in
redox-rpi/rpi-open-firmware (branch `redox-rpi`).

**State: spike.** Integer subset (load/store, arithmetic, compares,
branches, calls, frames, variadic functions, struct return). Every C file
of the firmware's `bootcode/` but `monitor.c` compiles and links. No float,
`long long`, struct arguments, VLAs, inline asm or assembler yet.
`vc4-tcc -c` writes ELF objects (`EM_VIDEOCORE3`, RELA) that link with
`vc4-elf-ld` from the vc4-toolchain; TCC's own linker is not wired up.

## Files

| File | What |
|---|---|
| `vc4-enc.h` | instruction encoder (16/32/48-bit forms) |
| `vc4-gen.c` | code generator; ABI as the vc4 GCC port |
| `vc4-link.c` | relocations (`R_VC4_32`, `IMM32`, `PCREL27_MUL2`, `PCREL23_MUL2`, `PCREL27`) |
| `tests/vc4/enc-test.py` | every encoder form against `vc4-elf-objdump` and, where it picks the same form, `vc4-elf-as` |
| `tests/vc4/bootcode-build.sh` | compiles the firmware's `bootcode/` C files (all but `monitor.c`, or `TCC_FILES=…`), compares sizes with GCC `-Os` per file, links them into the bootcode build |
| `tests/vc4/abi-sim.sh`, `abi-test.c`, `abi-main.c` | ABI and code generation test: the host build and the TCC build run in resim must print the same |
| `tests/vc4/sim-rt.c` | `printf` on the mini UART for test programs in resim |
| `tests/vc4/fat-sim.sh`, `fat-sim-main.c` | runs TCC's `fat.c` in the resim simulator (vc4emul) on FAT16 and FAT32 card images; every file must load with the right size and checksum |

## Build and test

```bash
./configure --config-bcheck=no --config-backtrace=no
make vc4-tcc
python3 tests/vc4/enc-test.py /path/to/prefix/bin/vc4-elf-
tests/vc4/bootcode-build.sh /path/to/rpi-open-firmware /path/to/prefix/bin/vc4-elf-
tests/vc4/abi-sim.sh /path/to/prefix/bin/vc4-elf- /path/to/vc4emul
tests/vc4/fat-sim.sh /path/to/rpi-open-firmware /path/to/prefix/bin/vc4-elf- /path/to/vc4emul
```

vc4emul comes from resim (github.com/puppeh/resim, the vc4-toolchain
submodule). With CMake 4 and a current g++ it needs
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` and a compiler wrapper that adds
`-include cstdint -include cstring -include cassert`. Two resim gaps
matter here: its `asr` returns 1 for every negative value (`||` for `|`
in `libresim/src/videocore/VideoCoreIVExecute.hpp`, case `OP_ASR`; fix
that line before building), and it has no `ldsb`, so vc4-tcc loads signed
bytes with `ldb` and a shift pair. resim also lacks `switch` and
`bitflip #imm`: newlib's `printf` and GCC's `fat.c` do not run there.
`stm r6-r7,lr` stores lr at the top in resim; vc4-tcc's variadic
prologue therefore saves r6, r7 and lr one by one.

## Licence

LGPL-2.1-or-later, as TinyCC. The VC4 encoding is our own: worked out
from the output of the VC4 GNU assembler and disassembler, used as black
boxes, and checked against them by `tests/vc4/enc-test.py`. No tables or
code are taken from GNU binutils (GPLv3).
