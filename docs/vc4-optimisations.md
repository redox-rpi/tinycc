# TinyCC VC4: optimisations and what each would gain

**Draft** for [#16](https://github.com/redox-rpi/tinycc/issues/16) (survey),
part of [#13](https://github.com/redox-rpi/tinycc/issues/13) (REQ-TCCVC-3).
The targets in #13 set the order of the work; they are not a plan to switch
compilers ([vc4-decisions.md](vc4-decisions.md#goal)).

This page lists what GCC 6.2.1 does at `-Os`/`-O2` on the VPU that vc4-tcc
does not do, estimates what each would gain on the benchmarks, and ranks
them by gain and effort. Newer ideas (after GCC 6) are in a section of
their own. The optimisations vc4-tcc already has are in
[vc4-components.md](vc4-components.md#vc4-genc-the-code-generator).

## How it was measured (2026-11-18)

- **Inputs:**
  - rpi-open-firmware release-0.1 (22bbd72), MONITOR=1 configuration.
  - tinycc main dab343f9.
  - vc4-elf-gcc 6.2.1 (vc4-toolchain 2f57f09).
  - resim at redox-rpi/resim 4b542e7.
- **Benchmarks:**
  - **B1:** text bytes of the 19 C files of `bootcode/`. `start.S` is left out because it is the same for both compilers.
  - **B4:** resim instructions of `fat-sim.sh BENCH=1`. This is `fat.c` loading the FAT32 test card; only `fat.c` changes between runs.
  - **B3s:** a simulator stand-in for B3. It is the work of the monitor's `fb` command without the HDMI set-up: `fb_pattern` and `crc32` from the firmware, for 780×438 at 32 bit and 400×300 at 16 bit. On the Pi, `fb` is 1.71× and 1.91× GCC (Test 493); B3s is 1.83×, so the stand-in follows the Pi.
  - **B2:** `fl START.ELF` on the Pi. It depends on the SD card as much as on the code, so it is estimated from B4 below.
- **Ablation:** each GCC optimisation was turned off on its own (`-fno-…`, on top of `-Os` and of `-O2`) and the three benchmarks were measured again. How much worse GCC gets is what that pass is worth on the VPU. Instruction profiles (`VC4EMUL_PROF`) and the disassembly then show where vc4-tcc loses.

Today:

| | GCC `-Os` | GCC `-O2` | vc4-tcc | TCC/`-Os` | TCC/`-O2` |
|---|---|---|---|---|---|
| B1 text, bytes | 30,972 | 35,510 | 43,781 | 1.41 | 1.23 |
| B4 instructions | 12.07 M | 12.02 M | 18.69 M | 1.55 | 1.56 |
| B3s instructions | 88.7 M | 82.8 M | 162.5 M | 1.83 | 1.96 |
| B2 `fl START.ELF`, Pi (Test 493) | 4.08 s | 4.13 s | 4.64 s | 1.14 | 1.12 |
| B3 `fb 780 438 20 1`, Pi (Test 493) | 3.07 s | 3.13 s | 5.34 s | 1.74 | 1.71 |

GCC `-O2` buys almost nothing over `-Os` on this firmware: 0.4 % in B4, 7 % in B3s, and nothing on the Pi. So the GCC to match is in practice `-Os`.

## Where vc4-tcc loses

**B4: one loop.**
- 78 % of GCC's and 83 % of TCC's instructions are the byte copy in `fat_load` (`*dst++ = src[k]`, 3.07 M bytes).
- GCC does it in 3 instructions per byte: `ldb r3,(r2+r0)`, `stb r3,(r7+r0)`, `addcmpbne`. It indexes both pointers with the counter; that is GCC's induction variable optimisation (`ivopts`).
- vc4-tcc needs 5: `mov r0,r9`, `add r9,1`, `ldb`, `stb r1,(r0)`, `addcmpbcs`. The `dst++` costs two extra instructions.
- 2 × 3.07 M = 6.1 M of the 6.6 M gap. The rest is `le16`, `le32` and `clus_sec`, which GCC inlines and vc4-tcc calls (0.3 M).
- Without `ivopts`, GCC itself needs 18.2 M, as much as vc4-tcc.

**B3: the CRC.**
- `crc32` (bit by bit, 8 steps per byte) is 87 % of GCC's and 82 % of TCC's B3s instructions.
- GCC needs 6.0 instructions per bit: `lsr`, `signext r2,0` (that is `-(crc & 1)`), `and`, `eor`, and `addcmpbne` (a counter running down to 0).
- vc4-tcc needs 10.4:
  - `-(crc & 1)` takes three instructions: `and`, `mov 0`, `sub`.
  - The `k < 8` loop takes three: `add`, `cmp`, `blt`.
  - The fused loop branch exists only for a register bound, not for a constant.
- The pixel loop is the other 13–18 %. vc4-tcc makes two calls per pixel (`pattern_rgb`, `fb_pixel`), each with a full prologue and frame. Parameters used only once go to the frame, because a register variable must be used at least twice. GCC inlines `pattern_rgb`. vc4-tcc needs 63 instructions per pixel, GCC 25.
- The division `x * 8 / w` per pixel is in both: neither compiler hoists it.

**B1: spread over everything.** Counted in the disassembly of the 19 files:

| | vc4-tcc | GCC `-Os` | what it costs vc4-tcc |
|---|---|---|---|
| functions | 180 | 109 | GCC inlines 71 small or once-called static functions |
| `cmp` | 762 | 169 | 734 are `cmp` + `b<cc>`; GCC uses the fused compare-and-branch `b<cc> ra,rb/imm,target` (one 32-bit instruction) |
| frame loads/stores | 813 | 132 | locals and parameters that do not get a register |
| frame set-up (`add r7,sp,…`, `add sp,…`, `add sp,r7,…`, 16 bytes) | 180 | 0 | 79 of the 180 functions never use their frame |
| `mov` | 2,620 | 1,807 | register shuffling, argument set-up |
| `bl` | 706 | 512 | inlining |
| zero-extension as `shl`+`lsr` | 113 | 0 | GCC uses `bmask rd,ra,16` (one instruction) |

## GCC 6.2.1 optimisations and what they are worth

Ablation: the change when GCC runs **without** the pass. Positive means GCC
gets bigger or slower. A pass that changes nothing is left out.

| GCC pass turned off | What it does on the VPU | `-Os` B1 | `-Os` B4 | `-Os` B3s | `-O2` B1 | `-O2` B4 | `-O2` B3s |
|---|---|---|---|---|---|---|---|
| `-fno-ivopts` | induction variables: both pointers of the copy loop become one index | +58 | **+50.9 %** | +3.6 % | +114 | **+51.2 %** | +3.9 % |
| `-fno-tree-loop-optimize` (all tree loop passes) | as above, plus the other loop passes | −20 | **+50.9 %** | +3.6 % | +436 | **+76.7 %** | +3.9 % |
| `-fno-tree-ccp -fno-tree-vrp` | constant and value-range propagation (here it lets the copy loop's count be known non-zero) | +16 | **+51.5 %** | 0 | +918 | +0.5 % | +1.7 % |
| `-fno-tree-loop-im -fno-move-loop-invariants` | loop-invariant code motion | +78 | **+25.4 %** | +1.0 % | +192 | 0 | +1.7 % |
| `-fno-peephole2` | late patterns; on VC4 this is where the fused `addcmpb` and compare-and-branch forms come from | +44 | **+25.4 %** | **+14.5 %** | +96 | 0 | **+16.1 %** |
| `-fno-auto-inc-dec` | post-increment addressing (`(r9++)`); at `-Os` ivopts already removed the need | −2 | 0 | 0 | +14 | **+25.5 %** | 0 |
| `-fno-inline` | all inlining | +612 | +0.9 % | +3.6 % | −2,568 | +1.0 % | **+8.6 %** |
| `-fno-inline-small-functions` | inlining of small functions | −48 | 0 | 0 | −3,146 | +0.1 % | +4.7 % |
| `-fno-inline-functions-called-once` | inlining of static functions with one caller | +176 | 0 | 0 | +100 | 0 | 0 |
| `-fno-reorder-blocks -fno-crossjumping` | block order and merging of equal code tails | **+1,044** | 0 | −1.0 % | +948 | +0.1 % | +3.2 % |
| `-fno-omit-frame-pointer` | no frame pointer where none is needed | **+906** | +0.3 % | +2.1 % ¹ | +876 | +0.1 % | 0 |
| `-fno-split-wide-types -fno-tree-forwprop` | forward propagation of expressions into their uses | +332 | +0.8 % | +0.5 % | +1,150 | +0.5 % | +0.6 % |
| `-fno-if-conversion -fno-if-conversion2` | conditional execution instead of short branches | +136 | 0 | 0 | +124 | 0 | 0 |
| `-fno-gcse -fno-tree-pre` | common subexpressions, partial redundancy | +126 | 0 | +0.5 % ¹ | +382 | 0 | 0 |
| `-fno-tree-ter` | expression trees rebuilt before instruction choice | +108 | 0 | +1.0 % | +72 | 0 | +0.1 % |
| `-fno-tree-dce -fno-dce -fno-tree-dse` | dead code and dead stores | +98 | 0 | 0 | +488 | 0 | 0 |
| `-fno-thread-jumps -fno-tree-dominator-opts` | jump threading | −24 | +0.1 % | 0 | +598 | +0.2 % | +2.2 % |
| `-fno-ipa-cp -fno-ipa-sra -fno-ipa-pure-const` | interprocedural constants and arguments | +68 | +0.3 % | 0 | +32 | 0 | +0.5 % |
| `-fno-expensive-optimizations` | extra CSE and combine work | +46 | +0.3 % | −0.5 % | +184 | +0.3 % | 0 |
| **All of** `-fno-inline -fno-ivopts -fno-tree-loop-im -fno-move-loop-invariants` | | +700 | **+77.2 %** | **+34.4 %** | | | |
| `-funroll-loops` (turned **on**) | unrolling | +216 | 0 | 0 | +7,416 | −9.1 % | −18.2 % |

¹ The B3s self-check failed with this flag: the program computed a wrong result. This is probably a bug of GCC's VC4 port with these options; it was not followed up.

No measurable effect: `-fno-optimize-sibling-calls`, scheduling (`-fno-schedule-insns`, `-fno-schedule-insns2`), `-fno-jump-tables`, `-fno-tree-switch-conversion`, `-fno-strict-aliasing`, `-fno-tree-coalesce-vars`, `-fno-caller-saves`, `-fno-tree-sink`, `-fno-tree-fre`, `-fno-tree-ch` (at `-Os`), `-fno-cprop-registers`, `-fno-ira-share-spill-slots`, `-fira-region=one`, `-fno-tree-slsr`, `-fno-tree-loop-ivcanon`.

Levels for comparison:

| Level | B1 | B4 | B3s |
|---|---|---|---|
| `-O0` | 41,256 | 49.6 M | 249.3 M |
| `-O1` | 33,358 | 12.1 M | 103.3 M |
| `-Os` | 30,972 | 12.07 M | 88.7 M |
| `-O2` | 35,510 | 12.02 M | 82.8 M |
| `-O3` | 47,684 | **5.25 M** | **63.8 M** |
| vc4-tcc | 43,781 | 18.69 M | 162.5 M |

What the ablation shows:
- **The speed comes from a few passes.** On B4 they are the loop passes (ivopts, constant and range propagation, loop-invariant motion) and peephole2. Without inlining, ivopts and loop-invariant motion together, GCC `-Os` needs 21.4 M on B4, more than vc4-tcc's 18.7 M, and 119 M on B3s.
- **Size comes from many small things.** The largest single passes are merging equal code tails (about 1 KB) and leaving out the frame pointer (about 0.9 KB). Inlining makes `-Os` code bigger by 0.6 KB in sum, but smaller per call.
- **`-O3` is much faster on both benchmarks:**
  - B4 is 2.3× faster, from the copy loop. How `-O3` restructures that loop has not been analysed in detail.
  - B3s is 1.4× faster, mainly because the 8-step CRC loop is unrolled completely: 4 instructions per bit, no loop branch.
  - The price is 54 % more code. Neither `-O3` nor unrolling is in the firmware's build.
- Scheduling does nothing measurable. resim counts instructions, not cycles, so pipeline effects on the real VPU are not visible here.

## Candidates for vc4-tcc, ranked

Status: points 1–4 are in [PR #21](https://github.com/redox-rpi/tinycc/pull/21)
(waiting for review; measured there: B4 1.55× → 1.03×, B3s 1.83× → 1.14×,
B1 1.41× → 1.33× of GCC `-Os` in resim; Pi Test 502: `fl` 1.01×,
`fb` 1.26–1.45× of GCC). It needs
[resim#2](https://github.com/redox-rpi/resim/pull/2) (`signext`).
Point 5 and the leaf part of point 6 (parameters in r0–r3, no saves)
are in [PR #22](https://github.com/redox-rpi/tinycc/pull/22), stacked on
#21: B3s 1.14× → 1.04×, B4 1.03× → 1.02×, B1 1.33× → 1.26×; on the Pi
as pi-tests Test 503. Draft.

The estimated gain is for vc4-tcc today, from the counts above. Effort
assumes TinyCC's one-pass design: patterns on the instructions just
emitted are small; anything that needs to see a whole loop or function
first is large.

| # | Optimisation | GCC equivalent | B1 | B4 | B3s / Pi `fb` | Effort | Notes |
|---|---|---|---|---|---|---|---|
| 1 | **Post-increment load/store** for `*p++` (`stb r1,(r9++)`): with the indexed source the copy loop is 3 instructions, as GCC's | `auto-inc-dec` + `ivopts` | −0.1 KB | **−6.1 M (−33 %) → 12.6 M, 1.04×** | – | small (pattern on `*p++` in the store) | B2: about −0.1 s (estimate) |
| 2 | **Constant bound in the fused loop branch** (`addcmpb<cc> rd,#i,#imm,top`) and counting down to 0 | loop `doloop`/`ivopts` | small | small | **−25.7 M (−16 %)** | small (extend the existing fusion) | every `for (k = 0; k < N; k++)` |
| 3 | **Idioms:** `-(x & 1)` → `signext rd,ra,0`; `(uint16_t)x`/`(uint8_t)x` → `bmask rd,ra,16/8` | combine | −0.2 KB | – | **−25.7 M (−16 %)** | small (patterns) | more idioms from the GCC disassembly |
| 4 | **Fused compare-and-branch everywhere** (`cmp`+`b<cc>` → `b<cc> ra,rb/#imm,target` when it reaches) | combine | **−1.5 to −2.2 KB (−4 %)** | small | −3 to −5 M | small (as the loop fusion, for all branches) | 734 places |
| 5 | **Frame elision:** no `r7`/`sp` set-up in functions that do not use their frame | `-fomit-frame-pointer`, shrink-wrapping | **−1.3 KB (−3 %)** | small | −2 M | small to medium (the prologue is emitted before the body; patch or move it, as `stm` is widened today) | 79 functions; fewer instructions per call |
| 6 | **More registers for variables:** parameters used once stay in their argument register; leaf functions also use r0–r5; more than 8 (r16–r23) | IRA/LRA | **−1.5 KB** | −0.2 M | −5 M | medium | 813 frame accesses against GCC's 132 |
| 7 | **Inlining** of small and once-called static functions (`le16`, `pattern_rgb`, …), by replaying their tokens at the call (the register-variable scan already records a function body's tokens) | `-finline-small-functions`, `-finline-functions-called-once` | ±0 (calls go, bodies are copied) | −0.3 M | −10 to −15 M (−8 %) | large | GCC inlines 71 functions here |
| 8 | **Induction variables beyond #1:** `while (n--)` against an end pointer; pointers and index merged; `y * pitch` as an addition | `ivopts`, strength reduction | small | small | −3 M | large in general; the common shapes as patterns are medium | #1 covers the big case |
| 9 | **Loop-invariant code motion** (e.g. `v.spc`, `x * 8 / w` parts) | `tree-loop-im`, `move-loop-invariants` | small | small once #1 is in (GCC needs it to reach its 3-instruction copy loop; vc4-tcc gets there by the post-increment) | −1 % | large (needs the whole loop) | needs an IR |
| 10 | **Constant propagation, CSE, dead code** across statements | `tree-ccp`, `gcse`, `tree-dce` | −0.2 KB | small once #1 is in | small | large (needs an IR) | TinyCC folds constants inside expressions only |
| 11 | **Tail calls** (`b f` instead of `bl f; ldm …pc`) | `optimize-sibling-calls` | 0 in GCC here | – | – | small to medium | low priority |
| 12 | **Conditional execution** instead of short branches (VC4 32-bit ALU forms take a condition) | `if-conversion` | −0.1 KB | – | – | medium | worth little to GCC here |
| 13 | **Merging equal code tails** (the same epilogue or error path in several branches becomes one) | `crossjumping` | **−1 KB** | – | – | medium (on the emitted code, before the function's end is written) | the largest single size pass in GCC |
| 14 | **Complete unrolling of short constant loops** (`for (k = 0; k < 8; k++)` with a small body), by replaying the body's tokens (loop rotation already replays tokens) | `-O3` complete peeling (`cunroll`) | +0.1 KB | – | **−25 M more after #2 and #3: B3s below GCC `-Os`** | medium | goes beyond GCC `-Os`: GCC does this only at `-O3` |

Taken together:
- **Items 1 to 5** are all small. They would bring vc4-tcc to about:
  - **B4 ≈ 12.6 M (1.04×)**;
  - **B3s ≈ 108 M (1.22×)**, and on the Pi about 3.6 s for `fb 780 438 20 1` (estimate);
  - **B1 ≈ 40 KB (1.30×)**.
- **Items 6 and 7** bring B3s to about 1.05× and B1 to about 1.25×.
- **Item 14** (unrolling the CRC's bit loop) then takes B3s below GCC `-Os`, about 0.8×. That is a first place where vc4-tcc could be faster than the GCC in use.
- **Item 13** is the next step for size, after items 4–6.
- **The rest of the size gap** needs what TinyCC has no place for today:
  - code motion over a whole function;
  - sharing of common code;
  - better choice of instruction forms (GCC's 16-bit forms; see the newer ideas).

B2 is estimated from B4. B4 is 1.55× where B2 is 1.12×, so about a fifth of `fl`'s time behaves like code; the rest is waiting for the card. Item 1 should bring `fl START.ELF` to about 4.2 s. `sd.c`'s word loop is not in B4; a Pi run will show.

## Newer ideas (after GCC 6)

| Idea | Where from | What it could gain here | Effort |
|---|---|---|---|
| **The VPU's vector unit** for copies, fills and checksums: 16 lanes, loads and stores of 16×16 bytes. GCC's VC4 port never uses it. | hand-written VPU code; auto-vectorisation in GCC/LLVM, here as library routines | B4's copy loop and B3's fill: several times faster; B2's copy from the sector buffer | medium as hand-written `memcpy`/`memset` in the firmware's libc; large in the compiler |
| **Recognising CRC loops** and using a table (or the vector unit) | GCC 15 (`-foptimize-crc`) and recent LLVM recognise bitwise CRC loops | `crc32` is 82–87 % of B3s. A 256-entry table (1 KB of B1) makes it about 5 instructions per byte instead of 48 (GCC) or 83 (vc4-tcc): **B3 several times faster than GCC** | medium (pattern on the loop shape) or a firmware change |
| **Two VPU cores** | – | The fill and the CRC split in two: up to 2× for B3, if the second core is free. What runs on the VPU is still open, so this follows the real workload. | a firmware decision first; then a runtime (start the second core, split loops) |
| **A small per-function IR** (SSA, linear-scan register allocation) after parsing, before emitting | LLVM, newer GCC | Items 6, 8–10 in one place; the size gap to GCC | large; changes TinyCC's design |
| **Separate shrink-wrapping** (save registers only on the paths that use them) | GCC 7 | Early returns in `sd.c` and `fat.c` | medium with item 5 |
| **Whole firmware as one unit** (all files in one TinyCC run): inlining across files, dropping unused functions | LTO | B1: functions nobody calls (`--gc-sections` equivalent); inlining across files | medium (TinyCC compiles several files in one run already) |
| **Profile-guided choices** from resim's pc histogram (`VC4EMUL_PROF`) | PGO | Inline and unroll only the hot loops (the copy loop, the CRC); keep the rest small | medium |
| **Superoptimised peepholes**: search short instruction sequences against resim | Souper, GNU superopt | More idioms like item 3, found automatically | medium |
| **16-bit instruction forms** where registers allow (r0–r15) | GCC's own choice | B1: GCC is smaller partly because more of its instructions are the 16-bit forms | medium |

## Two cores, and benchmarks that change

The BCM2837 has two VPU cores, and the firmware today uses one. What the
VPU will do (boot, display, perhaps more) is still open, so B1–B4 follow
the real workloads as they appear. Optimisations that pay off for any
workload come first: items 1–7 and the vector-unit library routines.

## Broadcom's binaries

Broadcom's closed `bootcode.bin` and `start.elf` were not used for this
page. Any study of them stays internal: notes on behaviour only, no code,
nothing published (same rule as rpi-open-firmware's README).

## Next steps

1. Items 1–4 as one PR, with B1, B4 and B3s before and after.
2. Add B3s to `tests/vc4` (as `fat-sim.sh BENCH=1`), so the CI and the nightly can report it.
3. Item 5, then 6.
4. A vector `memcpy`/`memset` for the firmware: this needs the firmware session's agreement.
5. Decide on inlining (7) and on an IR, from the numbers after 1–6.
