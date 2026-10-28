# TODO

Ordered roughly by what unblocks what. Items from the user are marked (user).

## Gate: tunable to any setup, by an iterative process driven from the shell (user)
First version done: `a5cal` / `a5tune`, see docs/CALIBRATION.md (guided capture of Q/G/B windows, a search over lowcut, strict, margin and ring, floor and hum report, saved as a profile). Still to do from the list below: hum notch, dynamics chains, a real-card trial, labelling in the middle of free playing.
The gate has to be adjusted to a given setup: a mic on a wooden floor; a coil pickup instrument such as a guitar with
massive hum; at times the output of a chain of effects including compression, gain and distortion. What exists now:
margin (`GATE`), ring (`RING`), `LOWCUT` (the instrument's lowest note) and `STRICT`, a bump is discarded when most
of its power is below the lowcut, and `gateprobe` replays stored takes through any setting. What is needed:
- **A raw capture ("tape") mode**: record the input as played, with no gate, so a setting can be replayed against it.
- **A calibration run** (`aspcal`, from the shell): "be quiet 10 s" (learns floor, hum and its harmonics, noise colour),
  then prompts for what should open the gate (normal, soft, plucked, a knock) and what should not (a bump, a cough,
  the stand), with a label per event given by key. It replays the capture through candidate settings, shows false
  positives and misses for each, proposes settings, and repeats until the user is happy.
- **Hum**: mains hum and its harmonics (50/60 Hz) from coil pickups: an adaptive notch before the rules, or learnt
  per-band floors that include the hum's own variation.
- **Dynamics chains**: compressed / distorted sources have a different floor behaviour (the floor rises with the
  signal): a hold-off after loud material, a floor that follows slowly upward.
- **Store the result as a profile** (see defaults) so a setup is one name: `violin-ur22c`, `guitar-hum`, `mic-floor`.

## Defaults and profiles
Session-stored settings, new sessions inheriting a defaults set: stored as events in the session log (so a revert
restores them), the defaults file copied in by `a5pace new`. Profiles for gate and sync per setup.

## Create tool
Done: `a5create` (seconds, bars, or a file) and `a5click` (docs/CREATE.md). Click for a running space is done (`CLICK ADD` / `CLICK CLEAR`). Still open: resampling other rates on import; mp3 and other formats; swing and odd groupings (7/8 as 2+2+3); a count-in; a click that follows a tempo map.

## `a5export`: done and tested by the user
Done: `a5export DIR [loop|mixed|all] [--to FOLDER]`, see docs/EXPORT.md. Still open: a shell key for it, a take that
overlaps itself in the iteration sum (summed with saturation for now), a `--since` to export only newer iterations.

## Generated tracks are numbered in the 90000 series: done
a5click, a5create and a5import store them as 90001, 90002 ... . Old negative iterations still read (a5export shows them as
90000 - n); no migration, the old sessions are not being carried forward (user).

## Undo in the shell: done
`u`, `U`, history with `.` / `!`, keep-mode `m` / `M`, safe-start and `x` export are in (docs/SHELL.md). Open: an undo of an undo (redo) and a "what would change" preview in the history.

## Envelopes (to map out with the user)
A take's gain and pan are one constant value across the whole take for now (`lev`). Envelopes replace it: points over
a take, evaluated with integer arithmetic so that removing a take subtracts exactly what was added. Fading is an
envelope (a perceived linear drop over the whole take); the fade automix waits for this.

## Latency
Smaller arecord / aplay buffers to shrink the 0.63 s round trip (the loop stays aligned either way).

## Soon after
- Take length and a "close this take" control in the status line; input-level warning near clipping.
- Stage 2: example graphic control; a lighter backend for Claude (`a5control` exists).
- Windows shim (plat.h), JACK and PipeWire shells.
- GitHub: copyright holder's name in LICENSE, install notes per platform, `a5doctor` in the issue template.
- Publisher and hub (later, as planned).

## Stability on a cubieboard (user: stability is everything)
Done 2026-10-29, see docs/CUBIE.md and docs/STORE-DESIGN.md: nothing in the loop waits for the disk (sqlite never syncs in the tick, a keeper
thread places and syncs takes and makes the next take's file ahead of time), the gate runs 2.7 times faster on the A8, takes encode at
flac level 2, the loop lives in `/dev/shm`. Measured on echo (a cubieboard1 on an SD card): the average tick is about 110 ms of 200.
Still to do, on the dedicated board with its real card and drive:
- A soak test: hours, with a real instrument, a long take, and the buffer sizes of the rewritten driver.
- The rebuild time after a reboot (a loop of many takes).
- Occasional multi-second stalls were seen on echo in phases that touch no disk (gate, take write); echo is a busy single-core server, so
  look again on a board that does nothing else.
- Keep the default buffers; shrinking them is opt-in (`A5CARD_BUFFER_MS`) and only after the soak test.

## The take store: what is built, what follows
Built: docs/STORE-DESIGN.md (takes as header + codec stream, streaming jobs, keeper), docs/RECOVER.md (crash noticing, `a5recover`).
Not built:
- **astream on the same codec module** (`include/codec.h`), on its own branch of the astream repository, last of all.
- **Several artists in a session, and p2p**: the artist id, the per-artist folders and the key `(artist, iter, off)` are in; the rest is not.
- **More than two channels** in the core (tick buffers, gate, tank, sync). The take file and the codec already allow up to eight.
- **A job budget by time** instead of by frames (the moves cost about 33 ms a tick on an A8 while one runs).
- **The offline tools** `a5tune` and `gateprobe` still load whole files (they analyse captures, not takes).
- **A verification of the newest takes at open** after a hard power cut (`a5recover` finds what a dead run left, but nothing checks every file).
- **Other platforms**: see docs/PORTABILITY.md.
