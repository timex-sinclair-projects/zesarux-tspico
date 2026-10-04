# ZEsarUX with a TS-Pico

This fork of [ZEsarUX](https://github.com/chernandezba/zesarux) emulates a
TS-2068 with a [TS-Pico](https://github.com/timex-sinclair-projects/tspico-firmware-build)
plugged in. The TS-Pico itself isn't emulated here: every access to ports
0Eh/0Fh goes to `pico_host`, which runs the real TS-Pico firmware with its SD
card in a folder on your computer.

What this branch (`tspico-device`) adds to upstream:

- **16K EXROM.** The TS-Pico ROM is 32K: a 16K HOME and a 16K EXROM. A 24K
  stock ROM still loads, as before.
- **The TS-Pico bridge**, version 1 of
  [the bridge spec](https://github.com/timex-sinclair-projects/tspico-firmware-build/blob/main/docs/EMULATOR_BRIDGE.md)
  (`src/operaciones.c`), TS-2068 only.

## Running it

1. Start `pico_host`: the standalone binary from a tspico-firmware-build release, or
   `python3 tools/emu/pico_host.py` in a checkout of tspico-firmware-build.
2. Start this ZEsarUX on the TS-Pico ROM:

       zesarux --machine TS2068 --romfile TSPICO-21.ROM

   (`TSPICO-21.ROM` is in tspico-firmware-build's `src/rom/`.)

It finds `pico_host` at `tcp:127.0.0.1:2068`. To use another address, set
`TSPICO_BRIDGE` to `tcp:HOST:PORT` or `unix:PATH`. Without `pico_host` the
2068 runs as if no TS-Pico were plugged in.

## Building

As upstream: `cd src && ./configure && make`. On macOS, run the binary from
outside `src/`, or it takes `src/` for an app bundle and can't find its ROMs.
