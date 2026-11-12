# TinyCC with a VideoCore IV (VPU) backend

Fork of TinyCC `mob` (base 70adbb12) for the redox-rpi open Pi firmware.
Plan, decisions and milestones: `docs/tcc-vc4.md` in
redox-rpi/rpi-open-firmware (branch `redox-rpi`).

**State: spike.** Integer subset (load/store, arithmetic, compares,
branches, calls, frames, variadic functions, struct return), a VC4
assembler for `.S` files and inline asm, and TCC's own linker. The
firmware's whole `bootcode.bin` (C and `.S`) is compiled, assembled and
linked by `vc4-tcc` alone (`-nostdlib -static -Wl,-Ttext=0x80000000
-Wl,--oformat=binary`), given a bootcode that uses `__start_X`/`__stop_X`
section symbols instead of linker-script ones (proposed to the firmware).
No float, `long long`, struct arguments or VLAs yet.
`vc4-tcc -c` writes ELF objects (`EM_VIDEOCORE3`, RELA) that also link with
`vc4-elf-ld` from the vc4-toolchain. TCC has no linker scripts: no VMA/LMA
split, no `ASSERT`; `-Wl,-Map=FILE` writes the final section and symbol
addresses so a build script can do such checks.

## Files

| File | What |
|---|---|
| `vc4-enc.h` | instruction encoder (16/32/48-bit forms) |
| `vc4-gen.c` | code generator; ABI as the vc4 GCC port |
| `vc4-asm.c` | assembler: the common integer instructions in vc4 gas syntax, inline asm (`r`, `i`, `m` constraints, clobbers) |
| `vc4-link.c` | relocations (`R_VC4_32`, `IMM32`, `PCREL27_MUL2`, `PCREL23_MUL2`, `PCREL27`) |
| `tests/vc4/enc-test.py` | every encoder form against `vc4-elf-objdump` and, where it picks the same form, `vc4-elf-as` |
| `tests/vc4/asm-test.sh`, `asm-test.S` | the assembler against `vc4-elf-as`: both objects linked at the same addresses must give the same bytes |
| `tests/vc4/bootcode-build.sh` | compiles the firmware's `bootcode/` C and `.S` files (all, or `TCC_FILES=…`), compares sizes with GCC `-Os` per file, links them into the bootcode build with vc4-elf-ld, or with `LINK=tcc` by vc4-tcc (linker.ld's checks done on the `-Wl,-Map` output) |
| `tests/vc4/abi-sim.sh`, `abi-test.c`, `abi-main.c` | ABI and code generation test: the host build and the TCC build run in resim must print the same |
| `tests/vc4/sim-rt.c` | `printf` on the mini UART for test programs in resim |
| `tests/vc4/sim-crt.S`, `sim-libc.c` | start file and `mem*`/`strlen` for test programs built and linked by vc4-tcc alone; `abi-sim.sh` and `fat-sim.sh` run such an "all TCC" build too |
| `tests/vc4/fat-sim.sh`, `fat-sim-main.c` | runs TCC's `fat.c` in the resim simulator (vc4emul) on FAT16 and FAT32 card images; every file must load with the right size and checksum |

## Build and test

```bash
./configure --config-bcheck=no --config-backtrace=no
make vc4-tcc
python3 tests/vc4/enc-test.py /path/to/prefix/bin/vc4-elf-
tests/vc4/bootcode-build.sh /path/to/rpi-open-firmware /path/to/prefix/bin/vc4-elf-
tests/vc4/abi-sim.sh /path/to/prefix/bin/vc4-elf- /path/to/vc4emul
tests/vc4/asm-test.sh /path/to/prefix/bin/vc4-elf-
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

## Changes outside the VC4 files

- `tccasm.c`: `.irp` (expanded on the source text, as gas does) and
  `.incbin`; for VC4, `.align N` means 2^N bytes, as in the vc4 GNU
  assembler.
- `tccpp.c`: `#` starts an immediate in VC4 assembly, not a comment (as
  for ARM).
- `tccgen.c`, `tcc.h`: `__attribute__((section))` on a prototype also
  applies to the function's later definition (GCC does this; TCC put the
  code in `.text`), and a section that gets function code is marked
  executable.
- `tccelf.c`: `-Wl,-Map=FILE` (parsed but unused before) writes the
  output's sections and global symbols with their final addresses; for
  VC4, code sections come first in the image, so the first object's code
  lands at `-Ttext` as with a linker script.

## Licence

LGPL-2.1-or-later, as TinyCC. The VC4 encoding is our own: worked out
from the output of the VC4 GNU assembler and disassembler, used as black
boxes, and checked against them by `tests/vc4/enc-test.py`. No tables or
code are taken from GNU binutils (GPLv3).
