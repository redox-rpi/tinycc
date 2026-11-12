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
section symbols instead of linker-script ones (rpi-open-firmware `fat-boot`
a8bcbe7 or later; Pi test: pi-tests row 358, passed).
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

## Code size and speed (branch `vc4-opt`, experiment)

| | before (`vc4` dbee1ac0) | step 1 (3e88adad) | step 2: register variables | GCC 6.2.1 `-Os` |
|---|---|---|---|---|
| bootcode text | 24,856 (1.71×) | 22,362 (1.54×) | 21,524 (1.48×) | 14,526 |
| `bootcode.bin` (TCC link) | 24,972 | 22,472 | 21,636 | 14,584 |
| resim, `fat.c` loading the FAT32 test card (`fat-sim.sh BENCH=1`) | 65.0 M insns (5.4×) | 49.6 M (4.1×) | 18.7 M (1.55×) | 12.1 M |
| Pi, `fl START.ELF` (3 MB) | 9.43 s (2.56×, row 371) | 8.18 s (2.22×, row 371) | (Pi row pending) | 3.69 s |

What changed:
- loops: the `for` increment and the loop condition are compiled after the
  body (tokens saved), the condition as "jump back if true"; a short
  condition (up to 8 tokens) also at the top as the entry test, a longer
  one is reached by a jump on entry. One jump per iteration instead of
  three. A lone `x++` increment becomes `++x`. Expressions with `enum`,
  `struct`, `union` or `{` keep TCC's usual order (scope rules).
- r7 points 64 bytes below the frame top, so the first 16 local words use
  the 16-bit `ld/st rd,(r7+off)` form.
- constants: every ALU op uses its `#simm16` or `#imm32` form; `and`/`or`
  with one-bit masks become 16-bit `bmask`/`bset`/`bclr`.
- globals: one 48-bit `ld/st rd,(pc+sym)` (R_VC4_PCREL27), as GCC does.
- `st rX,(r7+n)` directly followed by `ld rY,(r7+n)` (no label between)
  becomes `mov rY,rX` or nothing.

Step 2:
- register variables: before a function is compiled, its body's tokens
  are scanned (then replayed); names are weighted by use, ×8 per loop
  level, and the 8 heaviest that are not file-scope names, never follow a
  unary `&` and do not appear in inline asm become candidates. Their
  4-byte scalar locals and parameters live in r8-r15 (callee-saved, 16-bit
  forms): the prologue's `stm r6-r7,lr` is widened in place to
  `stm r6-r(7+n),lr`, the epilogue's `ldm` likewise, so a function without
  them costs nothing. None in variadic functions, functions with stack
  arguments or with inline asm.
- `x++`/`--x` on a register variable is one `add`; a store into one
  retargets the instruction that computed the value (`add r9,r9,r1`, not
  `add r0,r9,r1; mov r9,r0`); a cast lvalue such as `(uint8_t)v` reads the
  low bits.
- `p[i]` for bytes: no `i*1` (whose rvalue conversion copied i), and
  `add rB,ra,rb; ldb rd,(rB)` becomes `ldb rd,(ra+rb)`.
- the loop bottom `add rd,#i; cmp rd,rs; b<cc>` becomes GCC's
  `addcmpb<cc> rd,#i,rs,top` (or `b<cc> rd,rs,top`).
- the frame's `add sp,#-size` is the 4-byte `#simm16` form (frames up to
  32 KB).

GCC's copy loop in `fat_load` is 3 instructions per byte, TCC's was 14 and
is 6 now (a post-increment copy and the index add remain: GCC strength-
reduces both pointers to one index).

`tests/vc4/tests2-diff.sh` runs TinyCC's tests/tests2 programs in resim,
compiled by this vc4-tcc and by an older reference build, and compares
the outputs (70 run, 59 of them also match the host's `.expect`; the rest
use floats, `long long` division or more of printf than sim-rt has).
`fat-sim.sh PROF=dir` prints instructions per function (needs a vc4emul
that writes a pc histogram to `$VC4EMUL_PROF`; local patch).

`fat-sim.sh BENCH=1` needs a vc4emul that prints its instruction count
(`insns: N` on stderr when `VC4EMUL_COUNT` is set; local patch of
`vc4emul/main.cpp`), and GCC's `fat.c` needs `bitflip` (`OP_BCHG`, one line
added to `VideoCoreIVExecute.hpp`, locally).

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
- `tccgen.c` (branch `vc4-opt`): loops with the condition at the end, for
  VC4 or with `-DTCC_LOOP_ROTATE` (native `make test` passes with it, and
  such a TCC builds itself reproducibly); `gind()` marks jump targets for
  vc4-gen.c's store-to-load forwarding (`vc4_label_ind`, declared in
  `tcc.h`). For VC4 only: the register-variable scan before
  `gen_function` (`regvar_ids`), hooks for `++`/`--`
  (`vc4_inc_regvar`) and the loop branch (`vc4_loop_branch`), no `i*1`
  for byte pointers. `tccpp.c` gets `tok_macro_top()`.
- `tccelf.c`: `-Wl,-Map=FILE` (parsed but unused before) writes the
  output's sections and global symbols with their final addresses; for
  VC4, code sections come first in the image, so the first object's code
  lands at `-Ttext` as with a linker script.

## Licence

LGPL-2.1-or-later, as TinyCC. The VC4 encoding is our own: worked out
from the output of the VC4 GNU assembler and disassembler, used as black
boxes, and checked against them by `tests/vc4/enc-test.py`. No tables or
code are taken from GNU binutils (GPLv3).
