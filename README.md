# i8008-pico

A Raspberry Pi Pico (RP2040) acting as the complete support system for a real
Intel 8008 CPU (1972): clock generator, single-stepper, interrupt source,
bus/state analyzer and emulated memory. The goal is to run programs on the
real chip while seeing every T-state of every machine cycle, as detailed as
possible, on a serial console.

Successor to the earlier `hardware/rp2040/i8008a` (clock/step/state bring-up
rig, proven on hardware) and `hardware/rp2040/8008` (first data bus + memory
attempt) experiments, with their known bugs fixed.

## Architecture

Five PIO state machines do all the hard real-time work; the two CPU cores
split the rest:

- `clk` (PIO0): non-overlapping two-phase clock, 473 kHz (125 MHz / 66 / 4).
- `single_step` (PIO0): pulses READY for one machine cycle, synchronized to
  SYNC and the clock phases.
- `int_request` (PIO0): pulses INT, raised on a CLK1 edge.
- `capture` (PIO1): samples S2..S0, D0..D7, BUS_WRITE, READY and INT (14 bits)
  once per T-state at CLK2-high inside SYNC-high, and pushes only changes.
  A free-running DMA channel drains it into a 1024-word ring buffer, so no
  transition is ever lost, even in free-run.
- `bus_write` (PIO1): drives a served byte onto D0..D7 for exactly the T3
  window: takes the bus at a T-state boundary, holds it through WAIT states
  while single stepping, releases at the boundary after core 1 confirms the
  T3 sample (PIO IRQ flag 4). PIN_BUS_WRITE mirrors the output enable for a
  scope.

Core 1 (`bus_engine.cpp`) consumes the ring buffer: latches the address at
T1/T1I, reads the cycle type at T2 (PCI/PCR/PCC/PCW), serves bytes from the
16 KB `i8008_ram[]` for read cycles, jams RST 0 on interrupt acknowledge,
commits PCW writes back to RAM, and forwards every transition to core 0
through a multicore-safe queue.

Core 0 (`main.cpp`) runs the serial CLI and prints the decoded trace,
including 8008 mnemonics (`i8008_decode.cpp` covers the full instruction set).

## Pin map

| GPIO  | Signal    | Direction (Pico view) |
|-------|-----------|-----------------------|
| 2     | S2        | in                    |
| 3     | S1        | in                    |
| 4     | S0        | in                    |
| 5-12  | D0-D7     | in, out during T3     |
| 13    | BUS_WRITE | out (debug: bus driven)|
| 14    | READY     | out                   |
| 15    | INT       | out                   |
| 26    | CLK1      | out                   |
| 27    | CLK2      | out                   |
| 28    | SYNC      | in                    |

Note: S2 is wired to the lowest GPIO, so the raw captured 3-bit state code is
bit-reversed relative to the datasheet's S2 S1 S0 ordering. The decode table
in `main.h` (`ST_*`) is the single source of truth for this.

The pin map lives in `include/i8008.pio` as public defines and is shared by
the PIO programs and the C++ code.

## Serial commands (115200 baud)

| Key | Action |
|-----|--------|
| b   | boot: READY high + INT pulse (RST 0 jammed on the ack cycle) |
| s   | single step, one machine cycle |
| g   | go / free run (READY held high) |
| w   | wait / halt (READY low) |
| i   | INT pulse |
| x   | dump RAM 0x0000-0x007F |
| z   | reset the bus_write SM, release the bus |
| d   | toggle raw 14-bit sample dump |
| h   | help |

Trace line format:

    seq    state  D7......D0 hex   flags  annotation
       42  T2     00000000 0x00    R..    PCI 0x0006
       43  T3     00000100 0x04    R.D    fetch ADI

Flags: R = READY, I = INT, D = Pico driving the bus.

## Default test program

Loaded into emulated RAM at boot (rest of memory filled with 0xC0 = LAA, the
canonical 8008 NOP):

    0x0000  2E 00     LHI 0x00
    0x0002  36 40     LLI 0x40
    0x0004  06 00     LAI 0x00
    0x0006  04 01     ADI 0x01     <- loop
    0x0008  F8        LMA          ; ram[HL] = A, exercises a PCW write cycle
    0x0009  44 06 00  JMP 0x0006

Watch it run with `b` then `g` (or step with `s`), and verify the write with
`x`: address 0x0040 should count up.

## Build

PlatformIO with the Arduino-Pico (earlephilhower) core:

    pio run              # build
    pio run -t upload    # flash
    pio device monitor   # trace console

`pre_build.py` assembles `include/*.pio` with pioasm (from PATH or
/usr/local/bin) into the generated `include/*.pio.h` headers, which are not
committed.

## Status and open points

- The whole bus-serving path (capture ring, T3-window bus drive, PCW writes,
  RST 0 jam) is new and needs verification on the real chip; the earlier rigs
  only proved clock/step/state capture.
- The exact SYNC phase relative to T-state boundaries should be confirmed on
  a scope; the bus_write timing assumes SYNC rises once per T-state.
  PIN_BUS_WRITE exists precisely to make this visible.
- I/O cycles (PCC, INP/OUT) are decoded and traced but not implemented.
- Interfacing levels: the 8008 is PMOS (+5 V / -9 V); the RP2040 is a 3.3 V
  device and not 5 V tolerant. Level shifting is the hardware side's problem,
  same as with the previous rigs.
