# TinyCC with a VideoCore IV (VPU) backend

Fork of TinyCC `mob` (base 70adbb12) for the redox-rpi open Pi firmware.
Plan, decisions and milestones: `docs/tcc-vc4.md` in
redox-rpi/rpi-open-firmware (branch `redox-rpi`).

**State: spike.** Integer subset only (load/store, arithmetic, compares,
branches, calls, frames). No float, `long long`, struct arguments,
variadic function definitions, VLAs, inline asm or assembler yet.
`vc4-tcc -c` writes ELF objects (`EM_VIDEOCORE3`, RELA) that link with
`vc4-elf-ld` from the vc4-toolchain; TCC's own linker is not wired up.

## Files

| File | What |
|---|---|
| `vc4-enc.h` | instruction encoder (16/32/48-bit forms) |
| `vc4-gen.c` | code generator; ABI as the vc4 GCC port |
| `vc4-link.c` | relocations (`R_VC4_32`, `IMM32`, `PCREL27_MUL2`, `PCREL23_MUL2`, `PCREL27`) |
| `tests/vc4/enc-test.py` | every encoder form against `vc4-elf-objdump` and, where it picks the same form, `vc4-elf-as` |
| `tests/vc4/fat-build.sh` | compiles the firmware's `bootcode/fat.c`, compares size with GCC, links it into the bootcode build |

## Build and test

```bash
./configure --config-bcheck=no --config-backtrace=no
make vc4-tcc
python3 tests/vc4/enc-test.py /path/to/prefix/bin/vc4-elf-
tests/vc4/fat-build.sh /path/to/rpi-open-firmware /path/to/prefix/bin/vc4-elf-
```

## Licence

LGPL-2.1-or-later, as TinyCC. The VC4 encoding is our own: worked out
from the output of the VC4 GNU assembler and disassembler, used as black
boxes, and checked against them by `tests/vc4/enc-test.py`. No tables or
code are taken from GNU binutils (GPLv3).
