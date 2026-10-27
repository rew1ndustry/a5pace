# aspace (version 5)

A looper with **measured zero latency**, built so that several people can play into the same loop over a network.
It measures the round trip of your sound card (a coded tone out, read back), lines every take up to the sample, keeps
every take as an immutable FLAC file and every action as an event in a log, so any earlier state of a session can be
rebuilt or returned to.

Status: work in progress, running on Linux/ALSA (a Steinberg UR22C), with a simulated card for testing. The core is
plain C99 meant to build on Linux, macOS, Windows and AIX; only Linux has been built so far.

## Parts
- `aspace` the runner: a pipe (raw s32 stereo 48 kHz on stdin/stdout) or a simulated card, and a control port
- `aspsh` a terminal shell: status line, events and a few keys (safe/live, profile, automix, revert)
- `aspctl` one command to a running aspace (also how a program, or Claude, drives a session without a sound card)
- `aspcard` (in `tools/`) aspace on an ALSA card: the loopback for the sync, dropout reporting
- `aspdoctor` (in `tools/`) gathers what is needed to find out what is wrong on a machine, into one file to send
- a session is a directory: `session.db` (SQLite: the log and the facts), `blobs/` (the takes as FLAC), `cache/` (the loop)

## Build
Needs a C compiler, make, cmake (for libFLAC), curl and unzip.
```
git clone git@github.com:rewindustry/aspace.git && cd aspace
third_party/fetch.sh             # the SQLite amalgamation (or install libsqlite3-dev)
make FLACDIR=/path/to/flac       # a libFLAC source tree; it is built into build/flac (or FLACDIR= with libflac-dev)
make test
```
## Try it with no sound card
```
bin/aspace new /tmp/demo demo 8
bin/aspace run -S 7001 /tmp/demo &      # a simulated card, 7001 frames of round trip
bin/aspctl /tmp/demo SIM BURST 2 0.1    # a tone burst in
bin/aspctl /tmp/demo TAKES
bin/aspsh /tmp/demo
```
## Read next
`docs/SHELL.md` (running it), `docs/EDGE.md` (the audio edge and the sync), `docs/DATA.md` (the store),
`docs/PROFILES.md` (defaults and gate profiles), `docs/LESSONS.md`, `docs/TODO.md`.

MIT licence. The idea and the method are by James Knight; the implementation was written with Claude (Anthropic).
