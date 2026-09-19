# History

Release notes. Commit messages stay short; the detail lives here.

## 2026-09-19 -- I/O cycles

PCC cycles are handled: INP 0-7 is served from monitor-set input values
over the same path and deadline as a memory read, OUT 8-31 is recorded per
port with a write count. New monitor commands `n` (set an input port) and
`p` (show ports); the trace annotates PCC cycles with port and data. An
echo program at 0x0100 (INP 0, OUT 8, JMP) is the test, verified on the
chip: T1 of a PCC cycle carries the accumulator, T2 the instruction byte
with the port in bits 5..1; INP runs T1-T5, OUT stops after T3.

## 2026-08-02 -- Working rig

First working state, verified on real silicon:

- Boot via INT plus a jammed RST 0 (the 8008 has no reset pin); the PC
  lands at 0x0000 reliably even when the marginal S2 line hides the
  interrupt-acknowledge state.
- Program execution from the Pico's emulated 16 KB RAM at 473 kHz,
  including PCW memory writes back into the array. The built-in test
  program counts into 0x0040, observable with the `x` dump.
- Single-stepping one machine cycle per keypress with a gap-free trace.
- `j` command: jam a three-byte JMP to force the PC anywhere without a
  stack push.
- Capture pipeline: debounced 14-bit PIO snapshot every clock period (two
  per T-state), DMA ring buffer, RAM-resident core-1 bus engine, lock-free
  event ring to the core-0 trace printer.
- The `monitor` wrapper script finds the rig's serial port by USB identity,
  so replugging the board never breaks the workflow.

The README tells the full bring-up story: the half-T-state output lag, the
read latch that closes before T3 is visible, the flash/XIP real-time trap,
and the rest of what the chip taught me on the way here.
