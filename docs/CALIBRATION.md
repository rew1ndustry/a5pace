# Calibration: preparing a gate profile for a named setup

Engineering a setup is kept apart from recording. `a5cal` is run **outside any loop session**, once per setup (an
instrument and interface, a room, a signal chain), and leaves a finished **profile** that the loop shell (`a5shell`, key
`p`) picks from its list. Nothing in `a5cal` touches a session.

```
a5cal violin                      the guided way: lowest note, capture, search, look, save
a5cal capture NAME [--script FILE]   record the guided capture from the card
a5cal tune NAME [--lowest HZ]        search for the best settings
a5cal show NAME                      the floor and hum, the profile in use, and the proposal, window by window
a5cal save NAME [PROFILE] [--force]  write the proposal as a profile (the old one is kept as PROFILE.old)
a5cal list
```

## How it works
1. **A script** says what to do and what each sound is. Default (`share/calibration.script`, copied to
   `~/.config/a5pace/calibration/default.script` by `make install`, and yours to edit):
   `Q` quiet (the instrument on but still), `G` good (a note, a soft note, a pluck: must open the gate), `B` bad (a bump,
   a knock, touching the cable: must not), `X` free. A rest of 3 s is added between prompts. The prompts are shown and
   spoken (`say`), so you do not have to watch the screen.
2. **Capture**: the raw input is recorded as you work through the script (the card must be free: `A5CARD_RELEASE=1` lets go
   of pipewire for the capture). No gate is involved; it is the input as it is.
3. **Search** (`a5tune`): the gate and the core's take logic are replayed over the capture with many settings. A setting is
   scored by: good sounds missed (worst), bad or quiet windows opened, stray events in the rests. It searches the lowcut
   (never above the instrument's lowest note, which is why `a5cal` asks), strict, then the gate margin and the ring rule,
   then the quietest take worth keeping (`minpeak`, placed between the loudest wrong event and the softest good one, when they are clearly apart), and takes the setting in the middle of the best ones, so a wide plateau is preferred to a lucky point. It says how many
   settings were equally good: few means the capture was thin, record more.
4. **Save**: the proposal becomes the profile. Re-run any time; capture again to change it.

## What it reports
The noise floor per channel, the DC offset, and any tonal lines in the quiet windows (mains hum and its harmonics are
named). The gate does not yet have a hum notch: for a coil pickup with strong hum the proposal can only raise the margin;
a notch, and a floor that follows compressed or distorted sources, are on the TODO.

## Testing it without a card
`a5tune synth SCRIPT OUT.raw OUT.windows [--seed N] [--hum HZ]` makes a capture of a script with known events (notes, soft
notes, plucks, bumps, hiss bursts, optional hum); `a5cal capture NAME --from OUT.raw --windows OUT.windows` imports it.
`tests/test_cal.sh` does exactly that: on the made-up capture the gate as it was (asp4's) opens for 5 of 6 quiet and
bad windows; the tuned profile opens for none and still catches all 7 good sounds.
