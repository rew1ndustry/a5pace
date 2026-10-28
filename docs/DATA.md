# asp5 data store

Status: agreed design, no code yet. Decided 2026-10-26.

## Layout: one session = one directory

```
<session>/session.db            SQLite (WAL): facts + append-only event log
<session>/takes.<artist12>/<iter>/<off>.a5t  immutable takes: a header of facts, then the codec's bytes (docs/STORE-DESIGN.md)
<session>/cache/                 disposable: the .d64 tank, decoded PCM, checkpoints
```

- `.d64` stays as the hot loop ("tank"). It is a derived cache, never the source of truth.
- Takes are immutable and written once. FLAC at the take's native bit depth, MD5 in the header.
- aspace is the single writer ("aspace keeps the books"). aspface, the shell, aspview, prod tools
  and remote clients go through aspace's control service, or open session.db read-only. Nothing
  parses filenames for state any more.
- Never put session.db on a network filesystem, never run the audio path against a remote DB.

## Why this split

| | flat files | SQLite only | client/server SQL | **hybrid (chosen)** |
|---|---|---|---|---|
| realtime audio | best | poor (BLOB copy, decode per read) | unusable | plain PCM cache |
| state | fragile (extension = state, renames race) | transactional | transactional | transactional |
| network / p2p | rsync-shaped, no conflict model | single-writer file | native but needs a server | replicate log + fetch takes by artist/iter/offset |
| contributor install | none | one file | heavy | one directory |
| integrity | none | WAL | yes | blob name is its hash; FLAC carries an MD5 |

Facts (takes, passes, gains, pans, envelopes, cues, notes) are small, relational, changing:
SQL. Audio is large, immutable, sequential: files. Measured on a 5 min stereo 24-bit take:
FLAC decode about 600x realtime (0.2% of a core), encode about 170x (0.6%), 57 MB against
120 MB raw. Silent channels cost almost nothing.

## The log

One append-only table, `log(seq, tick, wall, actor, type, args_json)`. Everything is an event:
take start/end, gate change, sync result, insert/remove, gain/pan/envelope, cue, safe on/off,
error, drop/starve report, revert. Unknown `type` values are stored and skipped by older
readers, so new control ideas need no schema change.

- **Rebuild:** state at log point N = fold of events 1..N over the FLAC takes. The tank, levels
  and mixes can always be regenerated from the takes plus the log.
- **Time travel:** returning to an earlier state appends `revert{to: N}`. It never truncates,
  so "log everything" holds and a revert can itself be reverted. Optional `.d64` checkpoints
  every K events make a rebuild quick.
- **Named mixes** are labels on log points. Per-track gain, pan and envelopes are events
  (`env{track, points:[[tick,gain],...]}`); a later event for the same track supersedes.
- One aspace per user: one writer, one tick order, no flock.

## Tables (v1 sketch)

```
session(id TEXT PRIMARY KEY, descriptor_json, schema_version, created)
takes(id, artist, iter, offset, len, rate, channels, bits, status, gone, wrapped, t_start)
log(seq INTEGER PRIMARY KEY, tick, wall, actor, type, args_json)
mixes(name PRIMARY KEY, seq)            -- label on a log point
notes(id, take_id, seq, text, kind)     -- ledger, cues, journal lines
peers(id, addr, last_seen)
```

`status` replaces the `.trk/.mix/.off` extensions. Session id is SHA-256 of a versioned
canonical descriptor (name, artist, conception date, optional fields).

## Network

- Mostly p2p; every peer ends with a copy of everything published.
- The hub is rendezvous only: it connects peers across NAT/CGNAT/firewalls (relay as fallback).
  It holds peer addresses and the session descriptor, not takes.
- Publishing is outside aspace. aspace records everything that passes the gate; a separate
  publisher watches the log and ships takes over astream: automatically for live performance,
  or when the user marks a take ready ("keep" mode).
- New in asp5: a logged **safe** switch. While safe, nothing leaves the machine and nothing is
  committed as a published take. Both switches are in the log.
- astream's low-bit-zeroed live copies are stored as lower resolution layers of the same take
  and replaced by the full blob when it arrives.

## Audio edge (separate contract, recorded here for context)

aspace stays a stdin/stdout pipe of ticks, independent of drivers. The runner must watch for
dropped or starved samples and report them into the log: they mean an unstable system or
buffers that are too short. Input and output must share one clock. Latency is calibrated by
loopback sync (default) or an acoustic chirp; declared latency only for adapters we control.
