# Running aspace: the runner, the shell, and the backend

```
aspace new DIR NAME SECONDS          make a session (a later create tool will build one from bpm, time
                                     signature and bars, with a metronome track made from samples)
aspace run [options] DIR             run it
aspsh DIR                            terminal window on a running aspace: status line, events, a few keys
aspctl DIR [COMMAND...]              one command to a running aspace; no command prints the status line
```

`aspace run` has two edges:
- **pipe** (default): raw little-endian s32 stereo 48 kHz on stdin, ticks of 9600 frames, the loop on stdout.
  `arecord ... | aspace run DIR | aplay ...` or aspface. The edge does the device and the loopback.
- **simulated card** (`-S LAT`): no devices at all; the loop's output comes back in only during sync, with LAT
  frames of round trip. Real time by default, `--fast` as fast as it will go, `--step` only when told.
  This is the backend for testing and for Claude: a whole session can be started, played into and inspected with
  no sound card and almost no cost (`aspctl` is a few hundred bytes of text each way).

The control port is TCP on 127.0.0.1 (TCP_NODELAY), its number in `DIR/ctl`. Every command gets lines then `OK`
or `ERR text`. `WATCH` turns a connection into a stream of `@tick type json`, the log as it is written.

| command | |
|---|---|
| `STATE` / `LINE` | the state as json / as one readable line |
| `SAFE` / `LIVE` | stop / resume recording takes (the loop keeps playing) |
| `SYNC` | hunt the round trip again |
| `GATE n`, `GATE +n`, `GATE -n`, `RING n`, `RELEARN` | gate margin dB, ring rule, learn the floor again |
| `AUTOMIX OFF`, `OVERLAP n`, `SWAP [each]` | one automix at a time, see below |
| `TAKES` | every take with its state |
| `REVERT seq` | go back to a log point (only while SAFE); the loop is rebuilt from the store |
| `EDGE drop\|starve N` | the edge reports lost samples: the take in progress is marked damaged and the hunt restarts |
| `EVENT edge_x {json}` | an edge or shell tells the log something (a latency report) |
| `SIM BURST ch amp [ticks]`, `SIM DUP 0\|1`, `STEP n` | simulated card only: play a tone burst in (ch 0 left, 1 right, 2 both), identical noise on both channels, step n ticks and reply when done |
| `QUIT` | close cleanly |

A loop that was not closed cleanly (dirty flag in its header), is the wrong length, or is an asp4 file is
rebuilt from the store on the next start.

## Automix (one at a time)
- **OVERLAP n**: a take goes out of the loop when n newer takes have overlapped it (it stays in the store; its
  state is `off`, and a revert can bring it back).
- **SWAP [each]**: successive takes swap channels. Mono takes go hard left, hard right, hard left; stereo
  takes alternate flipped and not. By default the swap happens between iterations; `each` also swaps between
  takes of the same iteration.
Fading is not an automix yet. A take's gain and pan are, for now, one constant setting across the whole take
(`lev`: a right shift per output channel, 6 dB a step, or off); fades will be envelopes on a take (a perceived
linear drop over the whole take) once envelopes are mapped out.
A take that changes place (a mono conversion, a swap, an overlap removal) is moved by a job a slice at a time
(a fifth of a second of work a tick), started when the playhead is not in the take and will not reach it before
the move is done. A take that fills the loop waits one pass and goes anyway (that one may click).

## The output
The loop is the sum of the takes in 64 bits. On the way out it is brought down by a right shift so the peak fits 32
bits. When that shift changes, the output fades from the old scale to the new one across the next output tick, so
the change is a ramp and not a step.

## Mono
The gate says which channels rang during a take. If one channel never did (or the two are identical) the take is
stored as one channel and placed in the middle (a level shift of 1, 6 dB down, on both outputs); with SWAP
it goes hard left / right instead. While it is being recorded it is heard as stereo, and is replaced by the mono
placement when it ends. `mono_detect` and `mono_lev` are in the core's config.

## What was not carried over from aspace.sh
It stays untouched in `~/bin`; asp5 takes the ideas, not the code. Left behind on purpose:
- state kept in file extensions (`.trk/.mix/.off`) and renamed by the shell: the store is the books now
- **wind mode** (mixed nothing: its argument count made aspace print usage) and the pan arguments that were
  ignored; pan and gain are placements in the store
- the safe-mode drop that tested the wrong file name and never deleted
- layers and fade done as removals by shell scripts queued per wrap: replaced by automix inside the core
- signals (SIGUSR1/2, SIGURG) as the control path: replaced by the control port
- the tick clicks (`CLICK`): dropped entirely; the create tool will make a metronome track from samples
- `aspace.rc`-style variables read by several scripts: options are the runner's

## On a real sound card: `tools/aspcard DIR [aspace options]`
`arecord | aspace run | aplay` on ALSA (the UR22C here; add a case for another card), with the shell's jobs done
for aspace: the loopback goes on for the sync hunt and off for playing (it follows `sync_start` / `sync_found` /
`resync` in the log), and the card's `overrun` / `underrun` reports become `EDGE drop` / `EDGE starve`. The sync
tone is capped (`ASPCARD_SYNCMAX`, default -36 dB re asp4's) because the monitor may be in headphones; it starts at
-48 and only rises if it is not heard. It never stops anything it did not start. If only pipewire holds the card,
`ASPCARD_RELEASE=1` sets the card's pipewire profile to "off" for the run and puts it back on exit. `ASPCARD_DRY=1`
checks everything and starts nothing. First real run (2026-10-27, UR22C, loopback): sync found on the first
attempt, 30123 frames (0.63 s: the pipes and card buffers), spread 1 sample, no drops or starves, loopback off and
pipewire restored at the end.

### The driver's idea of the latency, against the measured one
`aspcard` samples ALSA's own status files (`/proc/asound/cardN/pcm0p|pcm0c/sub0/status`, "delay") for four seconds after
each sync and logs `edge_latency` (measured, driver's playback + capture, buffers, periods, the gap), also appended
to `DIR/cache/latency.tsv`; after three runs it says whether the gap is steady. On the UR22C, six runs
(2026-10-27): the measured round trip was 30118-30137 frames (19 frames, one USB microframe is 8), the driver's
figure moved by about 600 frames and the gap by about 900, so the driver's number is less steady than the sync and
not a substitute for it here. Reusing the last measured value instead (`aspace run --latency FRAMES`, no hunt) is
good to about 20 frames (0.4 ms); it is there as an option, and the sync stays the default.

### Options, and what the shell does about pipewire
The card is taken only if it is free; if anything holds it, `aspcard` names it and stops (what you want for
exclusive use). `ASPCARD_RELEASE=1` is the opt-in: when only pipewire / wireplumber hold the card, switch its
pipewire profile to "off" for the run and restore it after. Running through pipewire, or jack, would be other
shells; this one does ALSA direct only.
