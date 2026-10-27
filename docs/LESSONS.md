# Lessons carried over from asp4

First pass, from the working notes. Extend as the importer finds more.

## Audio and sync
- Alignment is sample-exact: a burst inserted at sample 49001 records at 49001. Keep that test.
- The FSK sync measures the whole round trip, so the pipe and adapter buffering are included.
  Re-measure on every start; a retry fixes the occasional "device busy" / silent sync, caused
  by PipeWire holding the card.
- Past bugs worth a regression test: stale sync second after preload, sync confirm needing an
  exact match, a returning sync being recorded as a take, resync that never relocked (tick/ring
  misalignment), loopback left on after quit.
- A tick is 1/5 s (9600 samples at 48 kHz). The loop length need not divide into ticks, so
  insert and wrap code must handle odd loop sizes.
- Hosts differ: the mixer/loopback hook is per host (box = UR22C; some machines have no
  output-to-input route and need a cable).

## Gate
- The gate must sit above the input noise floor (input 2 on the UR22C is about 2.3M peak-to-peak).
- Single-cycle clicks must not open it; real knocks must. The ring rule (power above 30 Hz in
  4 ms blocks, `RING` dB over a learnt floor in 3 of the last 4 blocks) keeps all 5 labelled
  knocks and rejects all 13 clicks at RING 7-8; 6 lets one click in, 9 loses two knocks.
- The labelled test set (18 short takes in the transylvanianLullaby space) is the regression
  fixture for any gate change.

## Takes and state
- Takes are never overwritten (O_EXCL, move to the next iteration). Keep that as immutability.
- State in file extensions raced and drifted: `.mix/.out` bookkeeping disagreed with what was in
  the loop. This is the main thing the log replaces.
- State fields that mattered: per-channel level shift (L R, `-` = out), `wrapped` (pre-roll began
  in the pass before the one it is numbered in), `len` (-1 while recording), `gone` (samples
  already taken out of the loop, for long takes aged out pass by pass).
- Removals must wait for a running insert to finish unless their range ends; a ranged removal
  that would leave a hole must fail (rc 3), not silently proceed.
- asp4 kept per-take levels as shifts and moved takes by adding (new - old). asp5 has no per-take levels: a
  take is in or out, the loop has one gain, applied on the way out and faded over the next output tick.

## Mix behaviour users asked for
- Fade over N repeats (now the loop gain), drop takes covered by more than N later ones (OVERLAP), ping-pong
  successive takes L/R (SWAP). Gain and placement are log events, not shell arithmetic.
- LAYERS n keeps the newest n iterations; a long take counts one layer per pass.
- Undo takes the newest take.

## Process
- aspace keeps the books; shells and GUIs only read.
- Control: TCP for control and events, UDP for audio, TCP_NODELAY always on control sockets.
- The shell, aspface and the producer tools all depend on the on-disk layout today; asp5 must give
  them a service or read-only DB instead.
- User files are never deleted; move to Trash.
- Safe mode (nothing leaves the machine) was missing in asp4 and is a first-class event in asp5.
