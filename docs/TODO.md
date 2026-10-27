# TODO

Ordered roughly by what unblocks what. Items from the user are marked (user).

## Gate: tunable to any setup, by an iterative process driven from the shell (user)
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
restores them), the defaults file copied in by `aspace new`. Profiles for gate and sync per setup.

## Create tool
Build a space from bpm, time signature and bar count, with an automatic metronome track made from samples.

## Envelopes (to map out with the user)
A take's gain and pan are one constant value across the whole take for now (`lev`). Envelopes replace it: points over
a take, evaluated with integer arithmetic so that removing a take subtracts exactly what was added. Fading is an
envelope (a perceived linear drop over the whole take); the fade automix waits for this.

## Latency
Smaller arecord / aplay buffers to shrink the 0.63 s round trip (the loop stays aligned either way).

## Soon after
- Take length and a "close this take" control in the status line; input-level warning near clipping.
- Stage 2: example graphic control; a lighter backend for Claude (`aspctl` exists).
- Windows shim (plat.h), JACK and PipeWire shells.
- GitHub: copyright holder's name in LICENSE, install notes per platform, `aspdoctor` in the issue template.
- Publisher and hub (later, as planned).

## Stability on a cubieboard with an SD card (user: stability is everything)
- Soak test with injected storage stalls (a slow write of 200-500 ms) and see what the edge's buffers do.
- Take commit does fsync + a sqlite commit on the tick thread: measure on the board; options are `synchronous=NORMAL`,
  doing the commit on an idle tick, or a writer thread in the runner (the core stays single-threaded).
- The loop file on tmpfs (`--tank`) is in place; measure the rebuild time after a reboot on the board.
- Keep the default buffers; shrinking them is opt-in (`ASPCARD_BUFFER_MS`) and only after the soak test.
