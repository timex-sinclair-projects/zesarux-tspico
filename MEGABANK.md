# The TS-2068 Megabank in ZEsarUX

A model of the [TS-2068 Megabank](https://github.com/timex-sinclair-projects/TS-2068-Megabank),
a battery-backed 4 MB RAM card for the TS-2068 expansion bus, so its ROM and
firmware support can be written and tested with no hardware. The board's
behaviour is in the Megabank's [specification](https://github.com/timex-sinclair-projects/TS-2068-Megabank/blob/main/docs/spec.md):
this model follows its sections "The board" and "Registers".

TS-2068 only. The code is in `src/machines/timex.c` (search for `megabank`),
with hooks in `src/operaciones.c` (ports and writes), `src/cpu.c` (reset),
`src/start.c` (exit) and `src/debug.c` (the memory-zone name).

## Turning it on

The board is off unless one of these is set when ZEsarUX starts:

| Variable | Meaning |
|---|---|
| `MEGABANK_FILE=path` | Fit the board and keep its RAM in this file: the battery. Loaded at start, saved on exit. |
| `MEGABANK=1` | Fit the board with no file: its RAM starts at zero and is lost on exit. |
| `MEGABANK_BASE` | The first bank number: `1` (default), `65`, `129` or `193`. |
| `MEGABANK_SIZE` | Banks fitted: `8`, `16`, `32` or `64` (default, 4 MB). |
| `MEGABANK_WP=1` | The write-protect switch is on. |

For example, with the TS-Pico ROM:

    MEGABANK_FILE=~/megabank.bin zesarux --machine TS2068 --romfile TSPICO-23.ROM

## What it does

- **Ports FDh and FCh**, decoded from A7–A0: `OUT (FDh)` selects a register,
  `IN`/`OUT (FCh)` reads or writes it, and `IN (FDh)` reads back the selected
  register number. The registers are as the spec's table: `OWN0`–`OWN7`,
  `SUB0`–`SUB7`, `WPROT`, `STATUS`, `ID` (A5h), `IDCOMP` (5Ah), `BASE`, `SIZE`,
  `VERSION` (1). Unused registers read FFh and ignore writes.
- **Memory.** A chunk whose owner is one of the board's banks reads and writes
  page `(owner − BASE) × 8 + SUB` of the board's RAM, whatever ports F4h and
  FFh say, in any chunk, including 0–1 over the ROM. Writes are dropped in
  chunks set in `WPROT`, or everywhere while the write-protect switch is on.
  A chunk owned by a bank that isn't on the board, or by 0, FEh or FFh, is
  left to the 2068.
- **The screen** still comes from HOME RAM, as on the real machine: mapping a
  bank into chunk 2 hides the screen from the Z80, not from the display.
- **Reset** clears `OWN0`–`OWN7` and `WPROT` and sets `SUBn` = *n*; the RAM is
  kept. (ZEsarUX's reset; its "hard reset" does nothing on a TS-2068.)
- **The debugger** names an owned chunk `MB`, "Megabank bank *n*".

Not modelled: timing (board RAM is as fast as HOME), the battery-low bit
(`STATUS` bit 1 always reads 0), and snapshots (a `.zsf` doesn't hold the
board's registers or RAM).

## Test

    python3 tspico/megabank_test.py src/zesarux

Stock ROM, no other files: it drives a headless ZEsarUX over ZRCP, and runs in
CI after the bridge test.
