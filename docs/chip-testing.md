# Testing 8008 chips

The rig wires every pin of the 8008 to the Pico, which supplies the clock,
serves every memory and I/O cycle and sees every bus transfer. That makes it
a chip tester: the Pico runs test programs on the chip at full speed,
collects the results from the bus, and compares them with a reference model
of the 8008. One chip takes 7 seconds for the quick test and about four
minutes for the full one.

## Testing a chip

1. Unplug the Pico's USB cable. The 8008's -9 V supply comes from USB power,
   so this powers everything down. Never swap a chip with power on.
2. Put the chip in the socket (pin 1 marked) and plug the USB cable in.
3. Start the console: `./monitor`
4. Press `t` for the quick test or `T` for the full test. It asks for a
   label; type the chip's marking, date code or your own number and press
   Enter (Enter alone leaves it unlabeled, Esc cancels).
5. Read the result line at the end.

To keep a log of every chip, start the console with the log filter, which
writes everything to a file in the current directory:

    ./monitor -f log2file

Afterwards `grep RESULT platformio-device-monitor-*.log` gives one line per
chip tested.

The test boots the chip itself (no `b` needed) and leaves it halted when it
is done, with the normal counter program back in RAM; `b` boots it.

## The two tests

`t`, the quick test, runs the functional suite and the timing check at the
rig's normal clock of 504 kHz, and if both pass, the clock sweep.

`T`, the full test, runs the same, plus every ALU operation over all 65536
operand pairs (both carry inputs for ADC and SBB) between the timing check
and the sweep: about 655,000 results checked in about four minutes. Esc
aborts it.

## Reading the result

A passing chip looks like this:

    chip test (quick) -- ref chip, 504 kHz
      boot    ok after 1 try
      bus     data bus walking bits      pass  20 checks
      regs    register moves             pass  343 checks
      mem     memory, address lines      pass  71 checks
      incdec  increment, decrement       pass  192 checks
      add     ADD all forms              pass  589 checks
      ...
      hlt     HLT and interrupt resume   pass  5 checks
      timing  T-states per opcode        pass  247 checks
    clock sweep (quick suite at each rate):
       520 kHz  pass
       536 kHz  FAIL  (regs)
    done in 7 s; chip halted (t tests again, b runs the counter demo)
    RESULT label="ref chip" mode=quick result=PASS failed=- fmax_khz=520

A failing test prints up to three details under its line, each naming the
instruction under test, its operands and what came back. For illustration
(not from a real chip):

      add     ADD all forms              FAIL  589 checks, 1 failed
                ADB (81) a=7F b=01 c=0: expected A=80 C0Z0S1P0, got A=80 C0Z0S0P0

Here the chip computed 0x7F + 0x01 correctly but lost the sign flag. `a` is
the accumulator operand, `b` the second operand, `c` the carry going in.
Results carrying flags print as `A=.. CxZxSxPx`; plain reports as
`OUT port=value`. "only N of M results" means the program went astray and
stopped reporting; the instruction named after it is the one it was about
to test.

The RESULT line is the one to collect:

| Field | Meaning |
|-------|---------|
| label | what you typed |
| mode | quick or full |
| result | PASS when every functional test and the timing check passed; the sweep does not affect it |
| failed | comma-separated ids of the failed tests, or `-` |
| fmax_khz | highest clock the quick suite passed at in the sweep; `-` if the sweep did not run |

## What is tested

| Id | Covers |
|----|--------|
| bus | walking ones and zeros, 0x55 and 0xAA through A: every data line, into the chip (immediate reads) and out of it (OUT) |
| regs | all 49 register-to-register moves; after each one every register is reported, so a move that disturbs a third register fails too. Also every LrI |
| mem | writes and reads at walking-bit addresses over all 14 address bits (0x0001 to 0x2000 and their complements), every LMr including LMH and LML, every LrM including LHM and LLM, and LMI |
| incdec | INr and DCr on B, C, D, E, H, L across the wrap and sign edges, with both carry values: carry must come through unchanged |
| add ... cmp | each ALU operation in all nine forms (registers A to L, M, immediate) with edge operands (00, 01, 7F, 80, FF, 5A) and both carry inputs: result and all four flags |
| rot | RLC, RRC, RAL, RAR with both carry inputs; Z, S and P must stay untouched |
| jump | all 8 encodings of JMP; JFc and JTc on each flag, taken and not taken |
| call | all 8 encodings of CAL and of RET; CFc/CTc and RFc/RTc on each flag, both ways |
| rst | RST 0 to 7, into vectors that report and return |
| stack | 7 nested calls return cleanly; an 8th call wraps the stack (see below) |
| io | INP from all 8 input ports, OUT to all 24 output ports |
| hlt | HLT in all three encodings (00, 01, FF); an interrupt resumes after it, once with a jammed RST 1 whose return must come back to the same place |
| timing | the length in T-states of every documented opcode, both outcomes for conditionals (table below) |

Every test also checks the whole 16 KB of RAM afterwards: it must hold
exactly what was loaded plus the writes the test expects, so a stray write
anywhere fails the test.

Not covered: the undefined opcodes (0x22, 0x2A, 0x32, 0x3A and the INr/DCr
forms 0x38, 0x39), interrupts while a program is running, READY and WAIT
states, and anything electrical -- supply current, input thresholds, output
levels. The timing check counts T-states, not nanoseconds.

## How it works

Each test is an 8008 program the Pico generates into the emulated RAM at
0x0C00, followed by an end marker (OUT 31 with 0xDD) and a HLT. The Pico
halts whatever the chip was doing by filling RAM with HLT, loads the
program, and starts it by jamming a JMP 0x0C00 into the chip's next
instruction fetch, with an interrupt to wake it from HLT. The chip then runs
alone at full speed.

Results come out through OUT. The bus engine logs every OUT (port and the
accumulator from T1 of the I/O cycle) into a ring buffer that the test
reads. Plain values go to port 24. Flags have no direct way out of an 8008,
so the tests call FLAGOUT, a subroutine at 0x0B00 that tests C, Z, S and P
with a tree of conditional jumps and ends in one of 16 OUT instructions:
the port (8 to 23) encodes the flags, the data is A. That also exercises
the conditional jumps millions of times over the full test.

The reference model computes what each result must be: 8-bit results with
the carry as carry out of bit 7 for additions and as borrow for
subtractions and compares, cleared by AND, XOR and OR; Z, S and P from the
result, where P is set for even parity. The model was checked against the
reference chip, which agrees with it everywhere.

Timing is measured by the bus engine, not by the test programs: it counts
capture samples (one per clock period, two per T-state) from each
instruction fetch to the next and keeps the shortest and longest length
seen per opcode. After the suite, every documented opcode must have run and
must show exactly the datasheet lengths. Conditionals show two lengths, the
not-taken one as the shortest. HLT is not timed (its length is however long
it stays halted), nor are the instructions the rig jams in, which run as
interrupt acknowledges.

| Instructions | T-states |
|--------------|----------|
| Lrr, INr, DCr, rotates, ALU with register, RET, RST | 5 |
| LMr | 7 |
| LrM, LrI, ALU with M, ALU immediate, INP | 8 |
| LMI | 9 |
| OUT | 6 |
| JMP, CAL | 11 |
| JFc, JTc, CFc, CTc | 9 not taken, 11 taken |
| RFc, RTc | 3 not taken, 5 taken |

## The clock sweep

After a passing suite the sweep reruns the quick suite at rising clock
rates (520, 536, 554, 573, 605, 639, 679, 723, 773, 811, 853 and 875 kHz)
until it fails, and reports the highest rate that passed. It stops at 875
kHz because above that the rig itself would fail: bus_write holds a served
byte for a fixed microsecond after T3 becomes visible, which would reach
into the next state.

Read the number as a comparison between chips on this rig, not as a
datasheet speed grade. The Intel rating is 500 kHz for the 8008 and 800 kHz
for the 8008-1, but with minimum widths for each clock phase that the rig's
clock does not follow: it divides the period into four equal steps (phase
1, gap, phase 2, gap), so at 504 kHz each phase is about 0.5 us wide, and
narrower as the rate rises. A chip that fails early in the sweep may be
limited by the phase widths, not by the rate.

This runs the chip above its rating for the length of the sweep, which only
risks wrong results, not the chip.

## The reference chip

The test was developed on the rig's own 8008 (September 2026). It passes
the full functional suite and the timing check at 504 kHz, and the sweep
tops out at 520 kHz: at 536 kHz, straight-line code, memory and
unconditional jumps still work, but conditional jumps and calls go wrong,
so evaluating a condition is this chip's slowest path.

Two corners of the chip the suite relies on, both confirmed on it:

- The stack is 8 registers, one of which is the program counter, so 7
  nested calls return cleanly. An 8th call overwrites the oldest return
  address with the called routine's own program counter; when the
  outermost RET is reached, execution continues right after the innermost
  routine's RET instruction.
- HLT leaves the program counter after the HLT: an interrupt that supplies
  a NOP continues with the next instruction, and one that supplies RST 1
  returns there.

## Rig fixes that came out of this

Building the test showed two problems in the rig, both fixed in the same
change:

- A halted chip woken by a 2-clock-period INT pulse sometimes did a plain
  fetch and halted again instead of entering T1I. The pulse is now 4
  periods (about 8 us at 504 kHz, scaled with the clock in the sweep).
- The bus engine could lose the low address byte of a cycle whose T1 read
  as T1I on its first sample (the marginal S2 line). It now treats T1 and
  T1I as one state when finding the settled sample. Before the fix, the
  first fetch after a `j` could come from the wrong address.
