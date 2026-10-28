# Running on a cubieboard

The cubieboard (Allwinner A10, one Cortex-A8 at 1 GHz, 1 GB) is the baseline: the core and the basic functions have to run on it, and they
do. This is what is known (measured on `echo`, a cubieboard1 with an SD card, kernel 6.18, 2026-10-28/29; no sound card was attached, so the
card path was tested on a PC with a UR22C).

## Build
Build on the board if you like (about 25 minutes, mostly SQLite and libFLAC), or cross-compile. libFLAC must be linked with `-lpthread`
(the Makefile does). On armv7 the Makefile builds `gate.c` with `-O3 -mfpu=neon -funsafe-math-optimizations -fno-math-errno`: the A8's
floating point unit is not pipelined, and this makes the gate 2.7 times faster (its decisions are checked against asp4's own gate by
`tests/gate_diff.sh`: 873 ticks, no differences).

## Run
```
a5pace new /mnt/disk/session NAME 20
a5pace run --live --tank /dev/shm/loop.d64 /mnt/disk/session   # (a5card does this with the card; --tank puts the loop in ram)
```
Keep the session on the fastest storage there is (a SATA drive is best); keep the loop file (`--tank`) in RAM. The loop is a cache and is rebuilt
from the takes when it is not there.

## What a tick costs (200 ms of audio), on the A8
| part | cost |
|---|---|
| the gate (filters and the transform of both channels) | about 45 ms |
| recording a stereo take (flac level 2, verify on) | about 14 ms |
| a level change running (a job) | about 33 ms (at most one slice of 48000 frames a tick) |
| the tank, the output mix | about 3 ms |
| the whole tick, averaged over a simulated run with takes coming every 2.7 s | 110-125 ms |

`tools/tickbench.c` (`make bin/tickbench`) times the parts on any machine. The runner logs `slow_phase` events (a phase of a tick over 20 ms: gate,
take, take_write, take_open, take_end, moves, ...) and counts `slow_ticks` (over 100 ms) in `a5control DIR STATE`, with `tick_ms_avg` and `tick_ms_max`.

## The disk
On an SD card `fdatasync` can take seconds (worst seen: 4.3 s), a rename or a new file can wait behind it, and a WAL checkpoint takes about
a second. Nothing in the loop waits for any of it: sqlite's log never syncs or checkpoints in a tick; a servant thread (the keeper) places
each finished take, syncs it, makes the next take's file ahead of time, and checkpoints the log. See docs/STORE-DESIGN.md for the numbers and
docs/RECOVER.md for what a power cut can cost.

## Not yet known
The dedicated board with its real sound card, a SATA drive and the rewritten driver has not been run. docs/TODO.md lists what to measure.
