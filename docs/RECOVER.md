# When a run dies

A run can die: it is killed, the machine loses power, the process crashes. aspace is built so that this loses as little as possible,
and says so plainly the next time.

## What is kept, and what could be lost

- The log (`session.db`) is written as it happens and never waits for the disk; after a power cut it may lose its last moments, never
  its consistency.
- A take being recorded is appended to a file in `tmp/` as it comes, a block at a time. If the run dies, what had been written
  decodes (to the last whole block).
- A take that has ended is handed to the keeper thread, which puts it in its place, makes it safe on the disk, and only then makes its
  row. A run that dies between the end of a take and its row leaves the take (its file) but no row; the log has a `take_pending`
  event naming it.
- The loop (`cache/loop.d64`) is only a cache: a run that dies leaves it marked as not closed, and the next start rebuilds it from the
  takes and the log.

## Noticing

Every run that can write marks the log when it opens (`store_open`) and closes (`store_closed`) the session. A start that finds an
open with no close after it, and no process alive with that pid, knows the last run died. It says so once, on stderr and as a
`crash_detected` event (a5hell shows it among the events):

```
aspace: the last run (pid 4093547) did not close properly. a5recover DIR looks for the takes it left behind
```

Running again after a clean close says nothing. A start never changes any take; looking is `a5recover`'s job.

## a5recover

```
a5recover DIR --dry        what it would do
a5recover DIR              do it
```

The space must not be running. It looks at:

- **takes handed on whose row was never made** (the log's `take_pending` events with no `take_added`): if the file, at its place or still at
  its part in `tmp/`, decodes whole, the take is put back under its own id and slot, and the state the log gives it applies, so it
  returns to the loop as it was. If it does not decode whole, or the file is gone, it is reported as lost.
- **takes that were being recorded** (a `new` event with no `end`, no `discarded`, no row): what decodes of the part is written again as a take
  of its own, in its slot, marked damaged and out of the loop (state off, so nothing in a mix changes without you asking); the part itself is
  moved to `tmp/recovered/`. The mono or stereo decision is not made for it (a take that was damaged is left as heard).
- **empty spare files** made ahead of time by the keeper: removed (they are empty).
- **rows whose file is missing or unfinished**: told, not mended.

When it has put anything back it also drops the loop file, so the next start rebuilds it with those takes in. It logs a `recovered` event
with the counts, and exits 0 when nothing was lost, 1 otherwise. Nothing of yours is deleted; the nearest thing is the loop cache.

## For tests

The keeper has two test-only pauses, `ASP_KEEPER_PAUSE_BEFORE_MS` and `ASP_KEEPER_PAUSE_AFTER_MS` (environment), which hold a take before it is placed or
after it is placed and safe but not yet reported. `tests/test_recover.sh` uses them, with the stepped simulated card, to kill a run at each of
the three moments that matter and check that `a5recover` brings the take back.
