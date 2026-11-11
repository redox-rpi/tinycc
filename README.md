# tinycc: redox-rpi fork

This is the [redox-rpi](https://github.com/redox-rpi/redox-rpi) fork of
[TinyCC](https://repo.or.cz/tinycc.git) (TCC), the small C compiler,
assembler and linker. redox-rpi ports Redox OS to the Raspberry Pi.

We use TCC for two things:

- **TCC on Redox on the Pi.** TCC runs on Redox on the Pi 3B+ and is in
  the release images as a beta since v0.5.0 (topic page
  [tcc.md](https://github.com/redox-rpi/redox-rpi/blob/main/docs/topics/tcc.md)).
  Its small Redox patches still live as patch files in the main project
  and will move here later.
- **A VideoCore IV (VPU) backend**, so that our own open Pi firmware
  ([rpi-open-firmware](https://github.com/redox-rpi/rpi-open-firmware)) can
  be built with TCC instead of the old VC4 GCC 6.2.1, in the end on the Pi
  itself. This starts as a time-boxed spike. Background and alternatives:
  [vpu-compiler.md](https://github.com/redox-rpi/rpi-open-firmware/blob/redox-rpi/docs/vpu-compiler.md).

## Licence

TinyCC's licence: the GNU Lesser General Public License, version 2.1
([COPYING](COPYING)). Our changes are under the same licence, so they could
go upstream. The VC4 instruction encoding is written by us from public
documentation and checked against the output of the VC4 GNU assembler; no
tables or code are taken from GNU binutils (GPLv3).

## Branches

| Branch | What |
|---|---|
| `mob` | TinyCC upstream `mob`, unchanged |
| `main` | our fork: upstream plus this README; work branches merge here |
| `vc4` | the VPU backend (spike) |

Upstream's own README is [README](README).
