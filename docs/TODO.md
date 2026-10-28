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

## Shell starts in safe mode, always (user)
`a5shell` should start its session in SAFE (no takes recorded) every time, however it was started, and say so; `l` goes live.

## `a5export`: take the audio out (user)
A standalone command, `a5export DIR [what] [--to FOLDER]`, working whether or not the loop is running (it reads the store,
never the tank, and does not write to the session).
- What: just the loop (the tank, as it sounds now), just the takes that are mixed (in the loop), or everything.
- Where: a subfolder (default `DIR/export/`), brought up to date by every call: files that are no longer wanted go away,
  changed ones are rewritten, unchanged ones left alone.
- Takes, not the mix: each take is written whole, with no gain or pan applied. flac, one file per iteration.
- Each file is padded with silence to fit the loop exactly; a take longer than the loop is padded to an exact multiple of
  the loop length. (Several takes in one iteration: to settle, probably summed at their offsets within the iteration's file.)
- Exporting the loop while the space runs: the tank is live and automix moves are in flight, so it must read only what the
  moves have finished (a move trails the head; the export must trail it too, never overtake a move, or a take would be
  half changed and click). If that proves hard, refuse to export the loop of a running space (takes and mixed takes are fine,
  they come from the store). Run at low priority (`nice`/`ionice`, small reads) so the core's reads and writes are not choked.
- Uses the take flac straight from the blobs where it can, so a long session exports without decoding it twice.

## Undo in the shell (user)
Replace the undo dialog (log point, needs SAFE). `u` undoes the last take, `U` the whole last iteration. An absolute undo
is chosen from the history: up/down to a point, `.` to action it (at the next start), `!` now, as with mixing.
In keep mode `u`/`U` do nothing. In keep mode `m`/`M` toggle (mix or mute) the most recent take (`m` queued, `M` now).

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

## Stability on a cubieboard with an SD card (user: stability is everything)
- Soak test with injected storage stalls (a slow write of 200-500 ms) and see what the edge's buffers do.
- Take commit does fsync + a sqlite commit on the tick thread: measure on the board; options are `synchronous=NORMAL`,
  doing the commit on an idle tick, or a writer thread in the runner (the core stays single-threaded).
- The loop file on tmpfs (`--tank`) is in place; measure the rebuild time after a reboot on the board.
- Keep the default buffers; shrinking them is opt-in (`A5CARD_BUFFER_MS`) and only after the soak test.

## `.a5p`: a session file you can click on (user)
Take over the `.a5p` extension. A session is a directory, so the clickable thing is a small text file, `NAME.a5p`, that
points at it: the session directory (relative or absolute), the profile to use, and how to run it (the sound card
through `a5card`, or a simulated card). Opening it starts the session in a terminal with `a5shell`. Needs: a MIME type
(`application/x-a5p`) and a `.desktop` handler on Linux, a file association on macOS and Windows, `a5pace new` writing
the file beside the session, and `a5card`/`a5shell` taking a `.a5p` in place of a directory. Make it one file you can
copy to another machine along with the session directory.

## Next: tracks in/out, level and pan mixing (design conversation, not built)
- `input` (auto|left|right|stereo) is the two-channel case of a general rule: which recorded channels become which loop
  tracks and where they sit in the stereo loop. N-track cards (four-track and up) keep a stereo loop.
- Rules to design: channel -> track map; 1+3 hard left and 2+4 hard right; spread across the mix; per-track pan and level
  as envelopes (a perceived linear drop over a take); one gate per channel or a shared one; per-channel tape; how the
  calibration (`a5cal`/`a5tune`) detects which channels are in use and writes the rule to a profile.
- Mandolin case: passive pickup on one channel and room mic on the other = a stereo config, chosen by the profile.
- `a5cal` should detect the used channel from the G windows and write `input` (done by hand in `violin` for now).
- Test gap: loop identical pass after pass after a long fixed-mono take is checked only by the stored mono/centre/no-job state.
