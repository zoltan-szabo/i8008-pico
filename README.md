# i8008-pico

A Raspberry Pi Pico (RP2040) acting as the complete support system for a real
Intel 8008 CPU from 1972: clock generator, single-stepper, interrupt source,
bus analyzer and emulated memory. I want to see this chip think -- every
T-state of every machine cycle, decoded and printed on a serial console --
and ultimately to put it through a thorough test.

This is the third incarnation of the idea. The first rig
(`hardware/rp2040/i8008a`) proved I could clock the chip, single-step it via
READY and read its state lines. The second (`hardware/rp2040/8008`) attempted
the data bus and memory emulation but never quite worked. This project
started as a cleaned-up merge of the two, and then the chip itself took over
the curriculum: almost everything I believed about its bus timing turned out
to be wrong, and the story of finding that out is documented below, because
the findings are the real value of this repository.

## What works today

Verified on the real chip (August 2026):

- Boot: the 8008 has no reset pin, so I interrupt it and jam a RST 0
  instruction onto the bus; the PC lands at 0x0000 and my program runs.
- Program execution from emulated memory: the CPU fetches every byte from a
  16 KB array in the Pico's RAM at 504 kHz, full speed, and the test
  program's counter is observable in a RAM dump while it runs.
- Memory writes: PCW cycles are decoded and committed back into the array.
- Single-stepping: one machine cycle per keypress, with a gap-free trace.
- I/O: INP from 8 input ports set from the monitor, OUT to 24 output
  ports recorded for the monitor. On the chip, a PCC cycle shows A at T1
  and the instruction byte at T2; INP runs T1-T5, OUT stops after T3.
- Forced jumps: a JMP to any address can be jammed into the instruction
  stream, which is the only way to set this CPU's program counter from
  outside.

## The 8008's states, as I met them

The 8008 multiplexes everything over 8 data pins and announces what it is
doing on three state outputs S2 S1 S0. One machine cycle is a sequence of
T-states:

| State | S2 S1 S0 | Meaning |
|-------|----------|---------|
| T1    | 0 1 0    | low address byte on the bus |
| T1I   | 1 1 0    | same, but this cycle is an interrupt acknowledge |
| T2    | 1 0 0    | high 6 address bits plus the 2-bit cycle code |
| WAIT  | 0 0 0    | READY was low, CPU is paused between T2 and T3 |
| T3    | 0 0 1    | the data transfer: instruction in, data in, or data out |
| T4,T5 | 1 1 1 / 1 0 1 | internal execution states (register transfers visible on the bus) |
| STOPPED | 0 1 1  | HLT was executed; only an interrupt revives it |

The cycle code on D7:D6 during T2 tells what kind of transfer T3 will be:
00 PCI instruction fetch, 01 PCC I/O command, 10 PCR data read (including
the extra bytes of multi-byte instructions), 11 PCW memory write. Getting
PCC and PCR the right way around cost me an afternoon; my old project's
table had it right and I "corrected" it into being wrong.

Because a boot ROM is just whatever answers the fetches, memory emulation
means: latch the address from T1 and T2, and during T3 either drive a byte
from the RAM array onto the bus (reads) or sample the bus into the array
(writes). Simple in principle. The rest of this document is about why it
was not simple in practice.

## What the chip taught me

I had a logic-analyzer-grade capture running before the memory serving
worked, and that turned out to be the only reason this project succeeded:
every wrong theory died by trace evidence. These are the facts this
particular chip (and rig) established, in the order they were discovered:

**A floating bus is an instruction stream.** Before serving worked, the CPU
happily executed whatever the floating bus decayed to: 0x3F is RET, so it
ran in circles at the top of memory; 0x00 is HLT, so it "mysteriously"
stopped. Watching garbage execute is a surprisingly good way to learn the
instruction encoding.

**The outputs lag half a T-state.** When I finally sampled the bus twice per
T-state, the trace showed that during the first clock period of every state
the bus and even the S0-S2 lines still carry the previous state's values;
they settle in the second period. Every decode (address latch, cycle code,
write data) must use the settled second sample. My first engine decoded the
early sample and cheerfully wrote garbage into RAM from misread "PCW"
cycles.

**The read latch closes before T3 is even visible.** The CPU latches
incoming data early in the real T3 -- before the state lines (which lag,
see above) ever show the T3 code. Serving data when I saw T3 was therefore
always too late, and the byte had to be on the bus before the end of T2.
The freakiest symptom along the way: the CPU executed RST 1 when I jammed
RST 0 (0x05), because it latched the bus mid-transition (0x3F decaying to
0x05 passes through 0x0D). An instruction latched from the falling edges of
other instructions.

**The serve path is hard real time.** Between capturing the settled T2
sample and the byte being driven there is a budget of a couple of
microseconds. Two things silently blew it: the core-1 code executing from
flash (a single XIP cache miss while core 0 hammers USB is enough), and the
pico-sdk's queue functions, also in flash. The fix was pinning the entire
core-1 hot path into RAM with `__not_in_flash_func` and replacing the SDK
queue with a hand-rolled lock-free single-producer ring. Symptom before the
fix: serves worked "about one time in three", which felt like electronics
but was software.

**S2 is marginal on my rig.** T1I reads as T1 and T4 as STOP now and then --
all pairs that differ only in S2. Consequence: I cannot rely on detecting
the interrupt-acknowledge cycle, so the boot jam is armed as "force RST 0
on the next instruction fetch, whatever the state lines claim". This line
needs a scope session; until then the workaround is solid.

**The chip is dynamic and it dies cold.** PMOS dynamic logic loses its mind
when the clock stops, which happens for two seconds at every firmware
reflash. The first boot afterwards fetches nonsense and usually halts;
the second or third `b` finds a warmed-up, sane CPU. Also, the INT pulse
has to last until the chip enters T1I: a halted chip woken by a pulse of
two clock periods sometimes did a plain fetch and halted again. The rig
now holds INT for four periods, about 8 us.

**Registers are invisible.** The 8008 has no way to show its registers,
so `r` makes the chip show them. At the next instruction fetch the rig
jams in LMA to LML, and each memory write carries one register across the
bus, where the engine records it instead of committing it. JTC, JTZ, JTS
and JTP to address 0 follow: if the next fetch is not 3 bytes on, the jump
was taken, so the flag was set. Seven RETs walk down the stack, and each
following fetch comes from one entry. Walking back up, a JMP to three
below each entry and a CAL there bring the PC back to exactly that entry
before the CAL pushes it, so every level gets its own value back. A JMP
back to the instruction the jam cut in on ends it, so no register, flag,
memory byte or stack entry changes. The 8008 does not expose its stack
pointer, so all seven entries show, used or not.
While stepping, the rig steps through those cycles by itself. A chip
parked in WAIT on an opcode fetch has not latched the opcode yet -- that
happens at T3 -- so the rig swaps the byte it is driving for the first
jammed instruction and the read-out takes over that very fetch; the chip
ends up parked on it again.

**There is no reset.** The only way to control the PC from outside is
through the instruction stream itself. `b` jams RST 0 (a one-byte call to
0x0000, which costs a push onto the internal stack), and `j` jams a full
three-byte JMP -- opcode on the next fetch, the two address bytes on the
following PCR cycles -- which moves the PC anywhere without touching the
stack.

## The monitor

The monitor is deliberately primitive: single-character commands over USB
serial at 115200 baud, no line editor, no protocol -- just me, the chip,
and a trace. Start it with:

    ./monitor

(the script finds the rig's port by USB identity, VID:PID 2E8A:000A, so it
does not matter which socket or name the board gets; `pio device monitor -p
<port>` works too if you prefer doing it by hand).

### Commands

| Key | Action |
|-----|--------|
| b   | boot: READY high, INT pulse, RST 0 jammed on the next fetch; PC ends at 0x0000 |
| j   | jam JMP: prompts for a hex address (1-4 digits, Enter; Esc cancels), forces the PC there without a stack push |
| s   | single step: one machine cycle |
| g   | go: free run (READY held high) |
| w   | wait: halt (READY low; the CPU parks in WAIT mid-cycle) |
| i   | bare INT pulse |
| x   | dump emulated RAM 0x0000-0x00FF |
| n   | set an input port: prompts for the port (0-7) and a hex byte that INP will read |
| p   | show the input ports and the last value OUT wrote to each output port, with write counts |
| r   | PC, registers A-L, flags C Z S P and the 7 stack entries, newest first, without changing anything; works while stepping, running or halted. Parked on an opcode fetch it reads right there, so pressing it again shows the same; parked mid-instruction it lets that instruction finish first |
| t   | chip test: functional suite, timing check and clock sweep, about 7 s ([docs/chip-testing.md](docs/chip-testing.md)) |
| T   | chip test with every ALU operation over all operand pairs added, about 4 minutes |
| z   | force-release the data bus and reset the bus-drive state machine |
| d   | toggle raw 14-bit sample dump |
| h   | this list |

### Reading the trace

Every captured sample prints as one line:

    26829  T2    00000000 0x00  ...  PCI 0x000C  serve 0x44 (jam)
    26831  T3    01000100 0x44  ..D  fetch JMP

The columns: sequence number, T-state, the data bus in binary and hex,
three flags (R = READY high, I = INT high, D = the Pico is driving the
bus), and an annotation: address latching at T1/T1I, cycle type and full
address at T2 plus the byte being served, and at T3 the decoded mnemonic
of a fetched instruction, the data byte of a read, or the committed
memory write. PCC cycles are annotated at T2 with the port and either the
byte served to INP or the accumulator value OUT sent.

Each T-state appears twice, once per clock period. This is intentional --
it is how the half-state output lag stays visible, and during T4/T5 the
second sample often shows internal register values passing over the bus
(the accumulator, for instance, is readable in the counter program's T4
states).

In free run the CPU produces around 500k samples per second and USB serial
prints a small fraction of them; the monitor reports how many trace lines
were dropped. Nothing is lost inside: the engine sees every sample and the
memory emulation never misses a cycle. For gap-free reading, halt with `w`
and step with `s`.

### A session, start to finish

    ./monitor
    b            (cold chip: repeat until the trace starts flowing)
    w            halt
    x            dump RAM: 0x0040 holds the counter
    g            run a moment
    w            halt again
    x            0x0040 has advanced
    s s s ...    watch LMA write the counter, JMP loop back, ADI increment
    r            PC, registers, flags: A holds the counter, HL = 0040
    j 0007       jump to the LAI, resetting the counter, without a reset pin

## The test program

`src/memory.cpp` preloads the 16 KB emulated RAM. Everything outside the
program is filled with 0xC0 (LAA, the canonical 8008 NOP), which makes the
whole address space a safe landing pad:

    0x0000  C0 C0 C0  LAA x3       landing pad after RST 0
    0x0003  2E 00     LHI 0x00
    0x0005  36 40     LLI 0x40     HL = 0x0040
    0x0007  06 00     LAI 0x00
    0x0009  04 01     ADI 0x01     <- loop
    0x000B  F8        LMA          ram[HL] = A
    0x000C  44 09 00  JMP 0x0009

It exercises instruction fetch (PCI), immediate reads (PCR) and memory
writes (PCW). A second program at 0x0100 exercises the I/O cycles (PCC)
by echoing input port 0 to output port 8:

    0x0100  41        INP 0        A = port 0
    0x0101  51        OUT 8        port 8 = A
    0x0102  44 00 01  JMP 0x0100

Reach it with `j 0100`, set the input with `n`, and watch it arrive with
`p`. To run something else, edit `boot_program[]` and reflash.

A PCC cycle carries the accumulator at T1 and the instruction byte at T2,
whose bits 5..1 are the port number (`01 RRM MM1`: 0-7 are INP, 8-31
OUT). INP is served exactly like a memory read, over the same deadline;
OUT is recorded at T2, since the data has already been on the bus.

## Building and flashing

PlatformIO with the Arduino-Pico (earlephilhower) core. No external
libraries; everything beyond the core is pico-sdk, and the PIO programs are
assembled by `pre_build.py` with pioasm.

    pio run              build
    pio run -t upload    flash; the rig is auto-detected by USB identity
    ./monitor            console

## Firmware architecture

Five PIO state machines do the hard real-time work:

- `clk` (PIO0): non-overlapping two-phase clock, 504 kHz (133 MHz / 66 / 4).
- `single_step` (PIO0): a READY pulse synchronized to SYNC and the clock,
  advancing exactly one machine cycle.
- `int_request` (PIO0): the INT pulse, raised on a CLK1 edge.
- `capture` (PIO1): a debounced 14-bit snapshot (S2 S1 S0, D0-D7,
  BUS_WRITE, READY, INT) at every phi1 rising edge -- two per T-state. A
  free-running DMA channel drains it into a 1024-word ring buffer.
- `bus_write` (PIO1): drives a served byte onto D0-D7 the moment core 1
  pushes it (mid-T2, per the latch timing above), holds through WAIT states
  until T3 becomes visible plus a microsecond, then releases. GPIO13
  mirrors the output enable for a scope.

Core 1 is the bus engine (`bus_engine.cpp`): it consumes the DMA ring,
tracks machine cycles from the settled samples, serves RAM on PCI/PCR,
handles the jam sequence, commits PCW writes, serves INP and records OUT
on PCC, and hands events to core 0
through the RAM-resident SPSC ring. The whole path is pinned out of flash.
Core 0 (`main.cpp`) owns the CLI and the trace printer, draining events in
bounded bursts so commands stay responsive under full trace load. The chip
test (`chip_test.cpp`) also runs on core 0; for it, core 1 logs every OUT
into a second ring and measures each instruction's length in T-states.

## Pico to 8008 connections

To be documented properly once the KiCad schematic of the rig exists. Until
then the pin map lives at the top of `include/i8008.pio` (state lines on
GPIO 2-4 with S2 lowest, data bus on GPIO 5-12, BUS_WRITE debug on 13,
READY 14, INT 15, clocks on 26/27, SYNC on 28). Level shifting between the
Pico's 3.3 V and the PMOS 8008 (+5 V / -9 V) is on the hardware side and
predates this repository.

## The thorough test

The actual goal of all this: a systematic exercise of the real silicon.
It exists now as the `t` and `T` commands, which turn the rig into a chip
tester: every instruction in every form, flags, all ports, the address
lines, the circular stack, HLT and interrupt resume, the T-state length of
every opcode, and a clock sweep. [docs/chip-testing.md](docs/chip-testing.md)
describes how to test a chip, how to read the result, how the test works,
and what my own chip showed.

## Open points

- Scope the S2 level shifter; T1I/T1 and T4/STOP confusions trace back to it.
- A way to load programs over serial instead of reflashing.
- A clock with the datasheet's phase widths instead of four equal steps, so
  the chip test's clock sweep becomes a real speed grade.
- The KiCad schematic, then the connections chapter above.
