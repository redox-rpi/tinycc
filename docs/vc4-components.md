# TinyCC VC4: the components

This page describes the parts of the VC4 backend, what each one does and
how they fit together. Decisions and licence: [vc4-decisions.md](vc4-decisions.md).
Build commands and size/speed numbers: [`README-vc4.md`](../README-vc4.md).

The firmware build stays on GCC 6.2.1. vc4-tcc is our own, independent
toolchain, tested every night next to it (redox-rpi WBS 2.15).

## The pipeline

```
 bootcode/*.c, *.S        vc4-tcc -c                 vc4-tcc (linker)
 (rpi-open-firmware) ───► tccpp → tccgen ─► objects ───► bootcode.bin ──► Pi
                          vc4-gen / vc4-asm   (ELF,      vc4-link
                          vc4-enc.h           EM 137)    -Wl,-Map
```

1. **Preprocessor and parser** (TinyCC's own `tccpp.c`, `tccgen.c`) turn C
   into calls to the backend, one expression at a time. TinyCC has no
   intermediate representation and no optimiser: it emits code while it
   parses.
2. **Backend** (`vc4-gen.c`) turns those calls into VC4 instructions;
   `vc4-asm.c` does the same for `.S` files and inline asm. Both write the
   bytes through `vc4-enc.h`.
3. **Objects** are ELF files of type `EM_VIDEOCORE3` (137) with RELA
   relocations. They link with GCC's objects and with `vc4-elf-ld`.
4. **Linker** (TinyCC's `tccelf.c` with `vc4-link.c`) puts the objects at
   0x80000000 and writes a flat binary. `-Wl,-Map=FILE` lists the final
   addresses, so the checks of the firmware's `linker.ld` can run on it.

## vc4-enc.h: the instruction encoder

All instruction encodings live in this one header, as small inline
functions such as `vc4_alu16`, `vc4_alu32i` or `vc4_ldst48`.
The VC4 has 16-, 32- and 48-bit instructions:

| Length | Used for |
|---|---|
| 16 bit | `mov`/`add`/… between r0–r15, small immediates (0–31), `ld/st rd,(rs+0..60)`, `b<cc>` within ±128 bytes, `stm`/`ldm` (push/pop) |
| 32 bit | three-operand ALU ops on r0–r31, `#simm16`, `ld/st` with larger offsets, indexed `ldb rd,(ra+rb)`, `b<cc>` within ±8 MB, `bl`, `addcmpb<cc>` |
| 48 bit | 32-bit immediates (`mov`, ALU ops), `ld/st rd,(pc+sym)` |

The encoding is our own work. `tests/vc4/enc-test.py` checks every form
the backend can emit (290 forms) against the GNU assembler and
disassembler, used only as black boxes (licence: [vc4-decisions.md](vc4-decisions.md)).

## vc4-gen.c: the code generator

TinyCC calls a fixed set of backend functions (`load`, `store`,
`gen_opi`, `gfunc_call`, `gfunc_prolog`, `gfunc_epilog`, the jump
functions). `vc4-gen.c` implements them for the VPU.

**ABI**, as in the vc4 GCC port, so TCC and GCC code can call each other:
- arguments in r0–r5, further ones on the stack; result in r0 (r0:r1);
  structs returned through a hidden pointer in r0
- r6–r23 are saved by the called function; sp is r25, lr r26
- variadic functions take their arguments the same way

**Registers in TCC's sense:** r0–r5 are the registers TinyCC allocates
for expressions; r6 is a scratch register; r7 is the frame pointer.

**Frame:** the prologue is `stm r6-r7,lr,(--sp)`, then `add r7,sp,-64`,
then `add sp,#-size`. r7 points 64 bytes below the top of the frame, so
the first 16 locals are in reach of the short 16-bit `ld/st rd,(r7+0..60)`.

**Optimisations** (all inside the backend unless noted):
- *Register variables.* Before a function is compiled, `tccgen.c` scans
  its tokens once and weights each name by use (×8 per loop level). The 8
  heaviest locals and parameters live in r8–r15 instead of the frame;
  names whose address is taken (`&`), names in inline asm and file-scope
  names are left out. To save r8–r15, the prologue's `stm r6-r7,lr` is
  widened in place to `stm r6-r(7+n),lr` (and the epilogue's `ldm` too),
  so functions without register variables cost nothing extra. Not used in
  variadic functions, functions with stack arguments or with inline asm.
- *Loop rotation* (`tccgen.c`): the loop condition is compiled after the
  body as "jump back if true", so one jump per iteration instead of three.
- *Fused loop branch:* `add rd,#i; cmp rd,rs; b<cc> top` at the end of a
  loop becomes one `addcmpb<cc> rd,#i,rs,top`, as GCC does.
- *Retargeting:* a value stored into a register variable is computed
  there directly (`add r9,r9,r1`, not `add r0,r9,r1; mov r9,r0`);
  `x++` on one is a single `add`.
- *Constants and globals:* every ALU op uses its immediate forms;
  one-bit masks become `bmask`/`bset`/`bclr`; globals are read with one
  `ld rd,(pc+sym)`.
- *Store-to-load forwarding:* `st rX,(r7+n)` directly followed by
  `ld rY,(r7+n)` becomes `mov rY,rX`, unless a jump target lies between.
- *Indexed bytes:* `p[i]` for bytes is one `ldb rd,(ra+rb)`.

Not supported yet: float, `long long` beyond what the firmware needs,
struct arguments, VLAs.

## vc4-asm.c: assembler and inline asm

Assembles `.S` files and `asm()` statements in the syntax of the vc4 GNU
assembler. It knows the common integer instructions (moves, ALU ops,
loads and stores, branches, `rts`, `di/ei`, `sleep`, `version`) and the
data directives TinyCC's generic assembler (`tccasm.c`) provides, plus
`.irp` and `.incbin`, which we added there. Inline asm supports the `r`,
`i` and `m` constraints and clobbers. Branches to labels already known
are resolved at once (short form when it fits), all others get a
relocation. It shares the encoder and helpers with `vc4-gen.c`.

## vc4-link.c: relocations

Resolves the relocations the backend and assembler emit, with the
relocation numbers of the vc4 toolchain, so objects from TCC and GCC mix:

| Relocation | Where |
|---|---|
| `R_VC4_32` | data words (pointers in tables) |
| `R_VC4_IMM32` | 48-bit `mov rd,#sym` |
| `R_VC4_PCREL27_MUL2` | `bl` (calls) |
| `R_VC4_PCREL23_MUL2` | `b<cc>` to another section or symbol |
| `R_VC4_PCREL27` | 48-bit `ld/st rd,(pc+sym)` (globals) |

The image starts at 0x80000000. For VC4, code sections come first in the
image, so the first object's code lands at `-Ttext` as with a linker
script. The firmware's chain trampoline (`.tramp`) is linked in place and
copied at run time; the firmware gained `__start_X`/`__stop_X` symbols
for that (fat-boot a8bcbe7).

## Changes in TinyCC's generic code

Kept small and, where they change behaviour, limited to VC4 or behind a
define (list in `README-vc4.md`):
- `tccgen.c`: loop rotation (VC4, or any target with
  `-DTCC_LOOP_ROTATE`); the register-variable scan and hooks (VC4 only);
  `__attribute__((section))` on a prototype also applies to the
  definition, as in GCC.
- `tccasm.c`: `.irp`, `.incbin`; `.align N` means 2^N bytes for VC4.
- `tccpp.c`: `#` starts an immediate in VC4 assembly; `tok_macro_top()`.
- `tccelf.c`: `-Wl,-Map=FILE` writes section and symbol addresses.

## The simulator: resim (vc4emul)

Most tests run VC4 code in resim's `vc4emul`, a VPU simulator by Mathias
Gottschlag (2012, MIT) that Julian Brown extended for the vc4 GCC port.
We use our fork [redox-rpi/resim](https://github.com/redox-rpi/resim)
(sources and their status: rpi-open-firmware [sources.md](https://github.com/redox-rpi/rpi-open-firmware/blob/redox-rpi/docs/sources.md)); versions and build
scripts are in [redox-rpi/vc4-toolchain](https://github.com/redox-rpi/vc4-toolchain).
Test programs print through a simulated mini UART. Our fork adds:
- `asr` fixed for negative values and for a shift by 0 (found by the
  csmith test)
- `switch` (GCC jump tables), `bitflip`, `abs`, `mulhd.uu`
- an instruction counter (`VC4EMUL_COUNT`) and a pc histogram
  (`VC4EMUL_PROF`) for speed work
- builds with current g++ and CMake 4

Known gaps: no `ldsb` (so vc4-tcc loads signed bytes with `ldb` plus
shifts) and no vector instructions.
resim runs about 20 M VC4 instructions per second per core.

## Tests (`tests/vc4`)

| Test | What it checks |
|---|---|
| `enc-test.py` (+ `enc-test.c`) | every instruction form against the GNU assembler and disassembler |
| `asm-test.sh` (+ `asm-test.S`) | about 90 instructions and the data directives give the same bytes as `vc4-elf-as` |
| `abi-sim.sh` (+ `abi-test.c`, `abi-main.c`) | the same program on the host and as TCC code in resim must print the same: variadic calls (also from GCC code), 8 arguments, narrow types, div/mod, shifts, switch, loops, recursion, struct return, function pointers, address-taken loop variables, `asr` by 0 |
| `fat-sim.sh` (+ `fat-sim-main.c`) | the firmware's `fat.c` from TCC reads FAT16 and FAT32 card images in resim; `BENCH=1` counts instructions, `PROF=dir` per function |
| `bootcode-build.sh` | the firmware's whole `bootcode/` from TCC, sizes against GCC; `LINK=tcc` also links it with TCC and checks `linker.ld`'s limits (12 KB stack, trampoline ≤ 2 KB) on the map |
| `tests2-diff.sh` | TinyCC's own test programs (tests/tests2) in resim, compiled by this build and by a reference build; outputs must match |
| `csmith-diff.sh` | random programs from csmith in the integer subset, host gcc against vc4-tcc in resim, checksums must match; one JSON line per run for the KPIs (tinycc#3) |
| `csmith-reduce.sh` | shrinks a mismatching csmith program with creduce to a small case for an issue |
| `csmith-weekly.sh` | the weekly run (systemd timer on homenodellm, Tuesday 12:00 UTC): 1,000 programs from the newest `main`, line to pi-tests `tcc.jsonl` |

Support files: `sim-crt.S` (start code), `sim-rt.c` (small printf on the
simulated UART), `sim-libc.c` (`memcpy` and friends for all-TCC builds).

On the real Pi, pi-tests rows chain a TCC-built `bootcode.bin` from the
running monitor and compare every reply with the GCC build (rows 347,
349, 358, 371, 383). From WBS 2.15 on, the nightly TCC track does the
same automatically.

## Where things are

| What | Where |
|---|---|
| Code | this repository, branch `main` |
| Decisions, licence | [vc4-decisions.md](vc4-decisions.md); background in rpi-open-firmware [vpu-compiler.md](https://github.com/redox-rpi/rpi-open-firmware/blob/redox-rpi/docs/vpu-compiler.md) |
| Build clone, test images | homenodellm `~/rbc-build/tcc-vc4`, `~/rbc-build/payloads` |
| resim | [redox-rpi/resim](https://github.com/redox-rpi/resim), branch `vc4-tests`; built on homenodellm in `~/rbc-build/tcc-vc4-tools/resim` |
| Toolchain versions, build scripts | [redox-rpi/vc4-toolchain](https://github.com/redox-rpi/vc4-toolchain) (PR #1: `toolchain.lock`, `build.sh`, `test.sh`) |
| GCC toolchain (reference) | homenodellm `~/rbc-build/toolchains/prefix` (vc4-elf-gcc 6.2.1) |
| Pi test reports | redox-rpi/pi-tests `reports/` |
| csmith results | redox-rpi/pi-tests `tcc.jsonl`; programs and logs on homenodellm `~/rbc-build/tcc-csmith-work` |
