# Defaults and profiles

Engineering a setup is kept apart from recording: a **profile** is made outside any session (by the calibration run, once
that exists, or by hand) and the loop shell only picks one from a list.

```
<config>/defaults           inherited by every new session
<config>/profiles/NAME      one named setup (an instrument, a room, a signal chain)
```
`<config>` is `$A5PACE_CONFIG`, else `$XDG_CONFIG_HOME/a5pace`, else `~/.config/a5pace`. A file is `key value` (or
`key=value`) a line, `#` comments; the first comment line is the description shown in the list.

| key | meaning |
|---|---|
| `gate` | the gate's margin over the learnt floor, dB (18) |
| `ring` | the ring rule, dB over the floor (7.5) |
| `lowcut` | the instrument's lowest note, Hz: nothing below it opens the gate; a take mostly below it is a bump and is not kept (30 = as asp4) |
| `minpeak` | dBFS (negative): a take that never reaches this level is not kept, and is taken out of the loop again (the quietest sound worth keeping; calibration finds it) |
| `strict` | 1: one band alone, twice running, does not open the gate |
| `mono` | 1: a take from one channel (or two identical ones) is stored as mono (default 1) |
| `sync_max`, `sync_start`, `sync_target` | the sync tone: ceiling and first level in dB re asp4's, and the wanted input peak in dBFS |
| `automix` | `off`, `overlap N`, `swap`, `swap each` |

Precedence, lowest first: built-in, `defaults`, the session's own stored settings, a profile chosen with `--profile` or
`PROFILE name`, then command-line flags. What is in force is stored in the session's log (as the state of take 0), so
a revert restores it and a session carries its setup with it.

```
a5pace profiles                  the list
a5pace profiles show NAME        what is in one
a5pace profiles install          copy the examples that ship in profiles/ (never over yours)
a5pace new DIR NAME SECS --profile violin-line
a5pace run --profile bass-line DIR
a5control DIR PROFILES | PROFILE name | SETTINGS      (and p in a5shell)
```
The sync keys apply at the next start; the gate keys, mono and automix are in force at once.

## Running on a board with an SD card (stability first)
- Every tick's processing time is measured: `STATE` has `tick_ms_avg`, `tick_ms_max` and `slow_ticks`; a tick over 100 ms
  (half the tick) is logged as `shell_slow_tick`. This is how to see what the storage does to you.
- `a5pace run --tank /dev/shm/loop.d64` keeps the loop file in RAM: it is only a cache, rebuilt from the store when it
  is missing, so nothing is lost and the SD card is spared the loop's constant rewriting.
- `A5CARD_BUFFER_MS` sets the ALSA buffer; do not go under about 600 ms (three ticks) on slow storage.
