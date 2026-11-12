# The graphical interface: what became of asp4's aspface

*Written 2026-11-12 while nut slept. This is the assessment asked for, and what was done about it.*

## What aspface (asp4) was
One SDL2 program, about 2100 lines, doing three jobs at once: (1) the **display** (a grid of ticks, one row an iteration,
coloured by peak, with a dial and zoom), (2) the **controls** (mouse wheel and clicks for gain, range, noise, feeding a track),
(3) the **audio edge** (SDL audio devices feeding `aspace` over pipes, plus the sync mixer hook). Its own header says "mouse clicks
are not allowed, could be recording".

## The options, honestly
| option | verdict |
|---|---|
| Port aspface to SDL | Rejected for the display. SDL gives pixels and no widgets, text needs SDL_ttf and a font path (aspface hard-codes a macOS font), and every control would be re-drawn by hand. It also ties the shell to a desktop session. |
| A browser page (`a5web`) | **Chosen, and done** (docs/A5WEB.md). No library, no build step, runs wherever a current browser does, can sit on another machine through `ssh -L`, scales with the window, takes text input for the mixing language. Its weakness is that it cannot be the audio edge: a page's audio goes through WebAudio, with its own buffering and clocks, and the sync hunt measures the *whole* round trip. |
| A toolkit (GTK, Qt, Dear ImGui) | Rejected: a large dependency for a screen that mostly shows one map, and the licences and the build on AIX/Windows/macOS become the project's problem. |

So the **display and the controls** were already ported: a5web draws the same map (row = iteration, ribbon per take, levels green /
yellow / red) and has the keys. What aspface did that nothing else did was the **audio edge**, and that is where a portable layer
earns its keep. SDL2 is the right one *there*: it is small, ubiquitous (ALSA, Pulse, PipeWire, CoreAudio, WASAPI, even AIX has ports), and
the edge has no widgets to draw.

## What was built: `a5sdl`
`src/a5sdl.c`, `make bin/a5sdl` (needs libsdl2-dev; deliberately not in `make all`, so the core has no new dependency), installed by
`make install` when built. `a5sdl DIR [--in NAME] [--out NAME] [--frames N] [--soft-loop] [--list] [a5pace run options]`.
It follows docs/EDGE.md: raw s32 stereo 48 kHz to and from `a5pace run`, drops and starves counted and told to aspace
(`EDGE drop|starve`), a signal drains aspace with `QUIT` before closing.
- **Limits, plainly.** SDL2 has no duplex device, so capture and playback have two clocks and may drift; a5sdl reports it and does
  not resample (EDGE rule 1 says the edge should; a resampler is the next job). The loopback for the sync hunt is the user's, as for
  a5card (`--soft-loop` does it in software and is only for trials and the test). The ring is eight ticks.
- **Tested**: `tests/test_sdl.sh` (soft loop with SDL's dummy driver: sync, a primed round trip of 9600 frames, a signal closing it).
  **Not tested**: a real card through SDL, macOS, Windows. `a5start` does not choose it yet; set `edge` handling by hand
  (`a5sdl DIR` instead of `a5card DIR`).

## What is left of aspface that a5web lacks
- The zoom and the dial of one take (a5web has the map and details on hover).
- Feeding a track from the GUI (`a5import` exists as a command).
- Mouse-wheel gain: deliberately not carried over; gain is typed in bit shifts (the Gain rule).
