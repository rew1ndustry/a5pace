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
| `GAIN n`, `DOWN [n]`, `UP [n]` | the loop gain, in steps of 6 dB down |
| `AUTOMIX OFF`, `FADE n [restore]`, `OVERLAP n`, `SWAP [each]` | one automix at a time, see below |
| `TAKES` | every take with its state |
| `REVERT seq` | go back to a log point (only while SAFE); the loop is rebuilt from the store |
| `EDGE drop\|starve N` | the edge reports lost samples: the take in progress is marked damaged and the hunt restarts |
| `SIM BURST ch amp [ticks]`, `SIM DUP 0\|1`, `STEP n` | simulated card only: play a tone burst in (ch 0 left, 1 right, 2 both), identical noise on both channels, step n ticks and reply when done |
| `QUIT` | close cleanly |

A loop that was not closed cleanly (dirty flag in its header), is the wrong length, or is an asp4 file is
rebuilt from the store on the next start.

## Loop gain, and the automixes (one at a time)
There are no per-take levels. A take is in the loop or it is not, and the loop as a whole has one gain, in
steps of 6 dB down. `GAIN n` sets it, `DOWN [n]` knocks it down n x 6 dB, `UP [n]` brings it back. **Whenever the
gain changes, the next output tick fades from the old gain to the new one**, so it never clicks. The tank does not
change (the gain is applied on the way out); the gain is kept in the store as the state of take 0, so a revert
restores it.
- **FADE n [restore]**: at the top of each pass the loop gain goes down n x 6 dB, until what the loop plays is at
  the learnt noise floor, and stays there. A take recorded while the loop has faded plays at the faded gain;
  `restore` goes back to full gain when a take starts.
- **OVERLAP n**: a take goes out of the loop when n newer takes have overlapped it (it stays in the store; its
  state is `off`, and a revert can bring it back).
- **SWAP [each]**: successive takes swap channels. Mono takes go hard left, hard right, hard left; stereo
  takes alternate flipped and not. By default the swap happens between iterations; `each` also swaps between
  takes of the same iteration.
A take that changes place (a mono conversion, a swap, an overlap removal) is moved by a job a slice at a time
(a fifth of a second of work a tick), started when the playhead is not in the take and will not reach it before
the move is done. A take that fills the loop waits one pass and goes anyway (that one may click).

## Mono
The gate says which channels rang during a take. If one channel never did (or the two are identical) the take is
stored as one channel and placed in the middle (fed to both outputs); with SWAP
it goes hard left / right instead. While it is being recorded it is heard as stereo, and is replaced by the mono
placement when it ends. `mono_detect` is in the core's config.

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
