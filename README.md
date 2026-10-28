# aspace (version 5)

A looper with **measured zero latency**, built so that several people can play into the same loop over a network.
It measures the round trip of your sound card (a coded tone out, read back), lines every take up to the sample, keeps
every take as an immutable FLAC file and every action as an event in a log, so any earlier state of a session can be
rebuilt or returned to.

Status: work in progress, running on Linux/ALSA (a Steinberg UR22C), with a simulated card for testing. The core is
plain C99 meant to build on Linux, macOS, Windows and AIX; only Linux has been built so far.

## Parts
- `aspace` the runner: a pipe (raw s32 stereo 48 kHz on stdin/stdout) or a simulated card, and a control port
- `a5shell` a terminal shell: status line, events and a few keys (safe/live, mixing screen, undo, history, export, automix, profile; docs/SHELL.md)
- `a5control` one command to a running aspace (also how a program, or Claude, drives a session without a sound card)
- `a5create` makes a space (seconds, bars, or an audio file); `a5click` makes metronome tracks and `a5import` brings in an audio file as a track, for a space that is closed or running (docs/CREATE.md)
- `a5export` takes the audio out as flac: every take, the takes in the loop, or the loop itself (docs/EXPORT.md)
- `a5cal` (in `tools/`) prepares a gate profile for a named setup, outside any session (docs/CALIBRATION.md)
- `a5card` (in `tools/`) aspace on an ALSA card: the loopback for the sync, dropout reporting
- `a5doctor` (in `tools/`) gathers what is needed to find out what is wrong on a machine, into one file to send
- a session is a directory: `session.db` (SQLite: the log and the facts), `blobs/` (the takes as FLAC), `cache/` (the loop)

## Build
Needs a C compiler, make, cmake (for libFLAC), curl and unzip.
```
git clone https://github.com/rew1ndustry/a5pace.git aspace && cd aspace
third_party/fetch.sh             # the SQLite amalgamation (or install libsqlite3-dev)
make FLACDIR=/path/to/flac       # a libFLAC source tree; it is built into build/flac (or FLACDIR= with libflac-dev)
make test
```
## Try it with no sound card
```
bin/a5pace new /tmp/demo demo 8
bin/a5pace run -S 7001 /tmp/demo &      # a simulated card, 7001 frames of round trip
bin/a5control /tmp/demo SIM BURST 2 0.1    # a tone burst in
bin/a5control /tmp/demo TAKES
bin/a5shell /tmp/demo
```
## Read next
`docs/SHELL.md` (running it), `docs/EDGE.md` (the audio edge and the sync), `docs/DATA.md` (the store),
`docs/PROFILES.md` (defaults and gate profiles), `docs/LESSONS.md`, `docs/TODO.md`.

MIT licence. The idea and the method are by James Knight; the implementation was written with Claude (Anthropic).
