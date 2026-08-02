# Contributing to i8008-pico

Thanks for your interest! This is a hobby project around one specific piece
of 1972 silicon, but bug reports, ideas and pull requests are welcome --
especially from anyone else putting a real Intel 8008 back to work.

## Building and flashing

Requires PlatformIO (the Arduino-Pico / earlephilhower core is fetched
automatically) and `pioasm` on the PATH or in /usr/local/bin for the PIO
assembly step.

```bash
pio run              # build
pio run -t upload    # flash; the rig is auto-detected by USB identity
./monitor            # serial console, 115200
```

Note that meaningful testing needs the hardware: a real 8008, level
shifting between the Pico's 3.3 V and the PMOS chip's +5 V / -9 V rails,
and the wiring described in the README. The firmware builds without any of
that, but all the interesting behaviour lives on the bus.

## Project layout

| Path | Contents |
|---|---|
| `include/i8008.pio` | Pin map and all five PIO programs (clock, step, INT, capture, bus drive) |
| `src/bus_engine.cpp` | Core 1: real-time bus engine -- cycle tracking, memory serving, jam sequence |
| `src/main.cpp` | Core 0: state machine setup, serial monitor, trace printer |
| `src/i8008_decode.cpp` | 8008 instruction mnemonics and state/cycle name tables |
| `src/memory.cpp` | The 16 KB emulated RAM and the built-in test program |
| `monitor` | Console wrapper that finds the rig's port by USB VID:PID |

## Reporting bugs

The single most useful thing is a **trace excerpt** from the monitor (the
numbered lines), together with what you did (`b`, `s`, `j`...) and what you
expected the CPU to do instead. Please include which 8008 variant you have
(8008 / 8008-1, date code if readable) and your clock divider if you
changed it -- timing findings in the README are measured on one specific
chip and may well differ on another.

## Pull requests

- Keep the firmware dependency-free: Arduino-Pico core and pico-sdk only.
- The core-1 serve path is hard real time: anything it calls must stay in
  RAM (`__not_in_flash_func`), and nothing on that path may block.
- The timing model (settled second sample per T-state, serve before the end
  of T2) is measured behaviour, documented in the README -- changes to it
  should come with trace evidence from real silicon.
- Cite the Intel 8008 datasheet or the MCS-8 User's Manual where behaviour
  is subtle; this chip has plenty of folklore and less documentation.
