# Running aspace: the runner, the shell, and the backend

```
a5pace new DIR NAME SECONDS          make a session (a later create tool will build one from bpm, time
                                     signature and bars, with a metronome track made from samples)
a5pace run [options] DIR             run it
a5shell DIR                            terminal window on a running aspace: status line, events, a few keys
a5control DIR [COMMAND...]              one command to a running aspace; no command prints the status line
```

`a5pace run` has two edges:
- **pipe** (default): raw little-endian s32 stereo 48 kHz on stdin, ticks of 9600 frames, the loop on stdout.
  `arecord ... | a5pace run DIR | aplay ...` or aspface. The edge does the device and the loopback.
- **simulated card** (`-S LAT`): no devices at all; the loop's output comes back in only during sync, with LAT
  frames of round trip. Real time by default, `--fast` as fast as it will go, `--step` only when told.
  This is the backend for testing and for Claude: a whole session can be started, played into and inspected with
  no sound card and almost no cost (`a5control` is a few hundred bytes of text each way).

The control port is TCP on 127.0.0.1 (TCP_NODELAY), its number in `DIR/ctl`. Every command gets lines then `OK`
or `ERR text`. `WATCH` turns a connection into a stream of `@tick type json`, the log as it is written.

| command | |
|---|---|
| `STATE` / `LINE` | the state as json / as one readable line |
| `SAFE` / `LIVE` | stop / resume recording takes (the loop keeps playing) |
| `SYNC` | hunt the round trip again |
| `GATE n`, `GATE +n`, `GATE -n`, `RING n`, `RELEARN` | gate margin dB, ring rule, learn the floor again |
| `AUTOMIX LOOP`, `KEEP`, `OVERLAP n`, `SWAP [each]` | the mode, one at a time, see below |
| `TAKES` | every take with its state |
| `WIPE [ALL]` | take the recorded takes out of the loop (`ALL`: the generated ones too, clicks and an imported file); they stay in the store |
| `HISTORY [n]` | the moments the loop changed, newest first: each a place to return to, with how many takes were in the loop after it |
| `REVERT TAKE iter off` | go back to how the loop was just after the take with that iteration and offset (needs SAFE) |
| `REVERT seq` | go back to a log point (only while SAFE); the loop is rebuilt from the store |
| `EDGE drop\|starve N` | the edge reports lost samples: the take in progress is marked damaged and the hunt restarts |
| `EVENT edge_x {json}` | an edge or shell tells the log something (a latency report) |
| `SIM BURST ch amp [ticks]`, `SIM DUP 0\|1`, `STEP n` | simulated card only: play a tone burst in (ch 0 left, 1 right, 2 both), identical noise on both channels, step n ticks and reply when done |
| `HELP` | this list of commands |
| `QUIT` | close cleanly |

A loop that was not closed cleanly (dirty flag in its header), is the wrong length, or is an asp4 file is
rebuilt from the store on the next start.

## Starting a space, and the tape
`a5start DIR` makes sure the space is running and starts it if not; `a5shell DIR` and `a5control --start DIR ...` call it
(and so will the gui and the backend), so how a space is started is decided in one place: the file `DIR/start` (lines
`edge card|sim`, `args ...` extra options for a5pace, `release 1` to let go of pipewire for the card), else an `edge` line
in `~/.config/a5pace/defaults`, else the sound card if there is one, else a simulated card. `A5START_EDGE=sim|card`
overrides. It runs detached; its words go to `DIR/run.log`; `a5shell DIR --no-start` does not start it.

**Tape.** `a5pace run --tape` keeps the input exactly as the card gave it, as stereo FLAC in `DIR/tape/input-DATE.flac`
(zero bits the converter leaves out compress away, so it is small). Sample n of the tape is tick n/9600 of the run, so the
log's ticks say what happened where. `--tape-loop` also keeps the loop as it went out (`loop-DATE.flac`): useful for
diagnosis, but heavy, and a sum of takes does not compress. `a5control DIR TAPE ON [path] | OFF` and `TAPE LOOP ON [path] | OFF`
start and stop either while running; both are off unless asked for. On a board with an SD card, keep them off for playing.

## Modes (one at a time)
- **loop**: the usual. Every take goes into the loop as it is recorded. This is the default.
- **keep**: takes are recorded and stored, but not put into the loop (their state is `off`, the history marks them "kept").
- **overlap n**: a take goes out of the loop when n newer takes have overlapped it (it stays in the store, and a revert
  can bring it back).
- **swap [each]**: successive takes swap channels. Mono takes go hard left, hard right, hard left; stereo takes alternate
  flipped and not. By default the swap happens between iterations; `each` also swaps between takes of the same iteration.
Set from the control port with `AUTOMIX LOOP | KEEP | OVERLAP n | SWAP [EACH]`, from `a5shell` with `a` (a short list, then
the one question that mode asks), or from a profile (`automix loop`, `automix keep`, `automix overlap 4`, `automix swap each`).
Fading is not a mode yet. A take's gain and pan are, for now, one constant setting across the whole take
(`lev`: a right shift per output channel, 6 dB a step, or off); fades will be envelopes on a take (a perceived
linear drop over the whole take) once envelopes are mapped out.
A take that changes place (a mono conversion, a swap, an overlap removal) is moved by a job a slice at a time
(a fifth of a second of work a tick), started when the playhead is not in the take and will not reach it before
the move is done. A take that fills the loop waits one pass and goes anyway (that one may click).

## The output
The loop is the sum of the takes in 64 bits. On the way out it is brought down by a right shift so the peak fits 32
bits. When that shift changes, the output fades from the old scale to the new one across the next output tick, so
the change is a ramp and not a step.

### The shell's keys (`a5shell DIR`; press `?` in it)
The top line names the space. `space` safe/live, `h` history (places to return to; pick a row or type an iteration and
offset such as `7.3`: it goes safe first and asks you to press space when you want to be live again), `w` wipe (asks),
`a` the mode (a short list; overlap asks how many, swap asks about each), `r` sync again, `l` relearn the gate floor,
`g`/`G` gate more/less sensitive, `p` choose a profile, `t` list takes, `?` help, `q` leave (aspace carries on), `Q` stop
aspace. Under it, in words, what has just happened; the status line shows the mode as `loop`, `keep`, `overlap 4` or `swap each`.

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

## On a real sound card: `tools/a5card DIR [aspace options]`
`arecord | a5pace run | aplay` on ALSA (the UR22C here; add a case for another card), with the shell's jobs done
for aspace: the loopback goes on for the sync hunt and off for playing (it follows `sync_start` / `sync_found` /
`resync` in the log), and the card's `overrun` / `underrun` reports become `EDGE drop` / `EDGE starve`. The sync
tone is capped (`A5CARD_SYNCMAX`, default -36 dB re asp4's) because the monitor may be in headphones; it starts at
-48 and only rises if it is not heard. It never stops anything it did not start. If only pipewire holds the card,
`A5CARD_RELEASE=1` sets the card's pipewire profile to "off" for the run and puts it back on exit. `A5CARD_DRY=1`
checks everything and starts nothing. First real run (2026-10-27, UR22C, loopback): sync found on the first
attempt, 30123 frames (0.63 s: the pipes and card buffers), spread 1 sample, no drops or starves, loopback off and
pipewire restored at the end.

### The driver's idea of the latency, against the measured one
`a5card` samples ALSA's own status files (`/proc/asound/cardN/pcm0p|pcm0c/sub0/status`, "delay") for four seconds after
each sync and logs `edge_latency` (measured, driver's playback + capture, buffers, periods, the gap), also appended
to `DIR/cache/latency.tsv`; after three runs it says whether the gap is steady. On the UR22C, six runs
(2026-10-27): the measured round trip was 30118-30137 frames (19 frames, one USB microframe is 8), the driver's
figure moved by about 600 frames and the gap by about 900, so the driver's number is less steady than the sync and
not a substitute for it here. Reusing the last measured value instead (`a5pace run --latency FRAMES`, no hunt) is
good to about 20 frames (0.4 ms); it is there as an option, and the sync stays the default.

### Options, and what the shell does about pipewire
The card is taken only if it is free; if anything holds it, `a5card` names it and stops (what you want for
exclusive use). `A5CARD_RELEASE=1` is the opt-in: when only pipewire / wireplumber hold the card, switch its
pipewire profile to "off" for the run and restore it after. Running through pipewire, or jack, would be other
shells; this one does ALSA direct only.
