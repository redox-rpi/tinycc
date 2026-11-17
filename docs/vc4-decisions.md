# TinyCC VC4: decisions and licence

Why this fork exists, what was decided and what we learned on the way.
The parts of the backend: [vc4-components.md](vc4-components.md). Build
commands and numbers: [`README-vc4.md`](../README-vc4.md). Background and
the alternatives that were considered (LLVM, a newer GCC): rpi-open-firmware
[vpu-compiler.md](https://github.com/redox-rpi/rpi-open-firmware/blob/redox-rpi/docs/vpu-compiler.md).
Open work is tracked in this repository's issues.

## Goal

One small compiler that builds the VPU part of the open Pi firmware
(rpi-open-firmware `bootcode/`), first on the build host and later on the
Pi under Redox, where TinyCC already runs (redox-rpi experiment `tcc`).
It is an investment in our own, independent toolchain: the firmware is
built with GCC 6.2.1, and vc4-tcc is tested next to it.

Targets and switching (draft, maintainer 2026-11-17):
- The firmware stays on GCC as long as vc4-tcc is not at least as good.
- The targets in [#13](https://github.com/redox-rpi/tinycc/issues/13)
  (REQ-TCCVC-3: B1–B4 against GCC `-Os`/`-O2`) are an intermediate goal.
  They set the order of the work; they are not a plan to switch.
- Once vc4-tcc is better than GCC, at least under some conditions,
  shipping the firmware built with vc4-tcc can be considered, possibly
  only for those conditions. That is the maintainer's decision, by RFC.
- What will run on the VPU is still open (the BCM2837 has two VPU cores),
  so the benchmarks follow the real VPU workloads as they appear. Optimisation work
  ([#16](https://github.com/redox-rpi/tinycc/issues/16)) keeps the use of
  both cores in mind.

The KPIs ([#8](https://github.com/redox-rpi/tinycc/issues/8)) track size
and speed against GCC; the targets are in #13.

## Decisions

| Topic | Decision |
|---|---|
| Where | This repository, a real fork of TinyCC `mob`, not a folder in the firmware repository. The firmware build points at a fixed commit (pinned in [redox-rpi/vc4-toolchain](https://github.com/redox-rpi/vc4-toolchain)). |
| Branches | `mob` is upstream, unchanged; `main` is our fork. Changes reach `main` by PR, reviewed by the coordinator or Quality and merged by the maintainer. Docs go straight to `main`. |
| Upstream | Upstream `mob` (repo.or.cz) is merged into `main` every week: the `upstream-sync` workflow (Monday 06:00 UTC, or by hand) test-merges it, runs the `vc4` workflow's tests on the result and offers it as a PR "upstream sync YYYY-Www" on branch `upstream-sync/YYYY-Www`; a conflict becomes an issue. It never merges; the maintainer merges with a merge commit (no squash), so upstream's commits keep their hashes. It also keeps this repository's `mob` equal to upstream. The aarch64 TCC-on-Redox track uses the same fork, so arm64 changes are flagged in the PR. |
| Redox patches | The four patches of the redox-rpi `tcc` experiment move into this fork later, as a separate step. |
| Generic code | Changes outside the VC4 files stay small and, where they change behaviour, limited to VC4 or behind a define (`TCC_LOOP_ROTATE`); `README-vc4.md` lists them. |
| Objects | ELF `EM_VIDEOCORE3` (137) with RELA relocations and the relocation numbers of the vc4 binutils, so TCC and GCC objects link together with either linker. |
| ABI | As GCC's VC4 port, so code from both compilers calls each other: arguments in r0–r5 and then the stack (also for variadic functions), result in r0 (r0:r1), structs returned through a hidden pointer in r0, r6–r23 saved by the callee, `sp` r25, `lr` r26. |
| Tests | Every instruction form against the GNU assembler and disassembler; programs in a simulator (resim) against host gcc; the firmware's own code (FAT, whole bootcode) in the simulator and on the Pi; weekly csmith programs. |

## Licence

LGPL-2.1-or-later, as TinyCC upstream, so the backend could go upstream
and `libtcc1` stays LGPL.

The VC4 instruction encoding is our own work. It is checked against the
GNU assembler and disassembler (`vc4-elf-as`, `vc4-elf-objdump`, GPLv3+),
used only as black boxes: `tests/vc4/enc-test.py` assembles and
disassembles every form the backend emits and compares the bytes. No
tables or code were taken from binutils. While the field layouts were
worked out, parts of the binutils CGEN description (`cpu/vc4.cpu`) were
also read; nothing was copied, and the black-box test verifies every
encoding.

The simulator, [redox-rpi/resim](https://github.com/redox-rpi/resim), is a
separate MIT project used only for testing.

## What we learned

About the VPU and its toolchain:
- `vc4-elf-as` encodes `ld rX,(rY-n)` (negative offset) as a PC-relative
  load, a binutils bug. GCC avoids negative offsets; vc4-tcc's encoder
  writes the correct 32-bit form.
- Variadic calls use the normal convention in GCC's port (r0–r5, then the
  stack), so calls between TCC and GCC `printf` work.
- `vc4-elf-ld` rejects TinyCC's `.eh_frame`; it is off for VC4, as for ARM.
- `vc4-elf-ld` takes the RELA addend and ignores the data word, so data
  relocations must carry their offset in the addend ([#6](https://github.com/redox-rpi/tinycc/issues/6)).
- The firmware's SD code uses one vector instruction, `v16mov -,imm REPn`,
  as a delay. vc4-tcc's assembler knows only this form: `v16mov`/`v32mov`
  to `-` (discard) with an immediate 0..63 and an optional `REP2`..`REP64`
  in upper case, as `vc4-elf-as` accepts. Without REP it is 48 bits
  (`f400`/`f600`, `e038`, `0400|imm`), with REP 80 bits (`fc00|log2(n)`,
  same middle, then `03c0 0000`). `vc4-elf-as` puts immediates from 64 up
  in a wider field; vc4-tcc rejects them. No other vector instructions,
  and the simulator has none.
- TinyCC has no linker scripts. The firmware's chain trampoline is linked
  where it is stored and copied at run time, with `__start_X`/`__stop_X`
  symbols (rpi-open-firmware fat-boot a8bcbe7); `-Wl,-Map=FILE` lets the
  build check `linker.ld`'s limits.

About the simulator:
- resim's `asr` was wrong for negative values and for a shift by 0, and
  `switch`, `bitflip` and `abs` were missing; fixed in redox-rpi/resim.
- resim has no `ldsb`, so vc4-tcc loads signed bytes with `ldb` plus
  `shl`/`asr`. GCC uses `ldsb`, so it is safe on the Pi; going back to it
  saves 2 bytes per load once the simulator has it.
- `stm r6-r7,lr` puts lr at the top in resim (the order is not
  documented). Normal functions only pair `stm` with `ldm`, so the order
  does not matter; variadic functions save r6, r7 and lr one by one.

About speed: TinyCC emits code while it parses and has no optimiser. Most
of the gap to GCC came from every variable living in the frame and three
jumps per loop iteration. Loop rotation, register variables (a token scan
before each function) and a few peepholes brought the firmware's 3 MB
file load on the Pi from 2.56× to 1.14× GCC's time (pi-tests rows 371 and
383); the rest is mostly GCC folding two pointers into one index.
