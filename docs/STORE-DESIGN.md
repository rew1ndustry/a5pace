# Takes: one codec module, streamed, never whole

Draft for discussion, 2026-10-28 (revised after the first round: no hash, own header, N channels, codec chosen at build time). Nothing here is built. The numbers are measured (test programs in the session
scratchpad, libFLAC from `~/arb/flac`, a real 27.6 s take from `Desktop/transylvanianLullaby`).

## What astream already does (the score everything should follow)

- Encoder: `init_stream(write_cb, 0, 0, 0)`: no seek, no tell. Block size 9600 = one aspace tick, so **one write
  callback = one complete FLAC frame = one tick**. Header bytes arrive first with `samples == 0`. Level 5, non-subset
  (32 bit), verify off.
- Decoder: `init_stream(read_cb, ...)`, driven one frame at a time (`process_single`); the read callback hands over
  whole frames. A damaged frame is `flush`ed and the stream goes on.
- Neither side knows what carries the bytes (socket, jitter buffer). The codec is bytes in, bytes out.
- It also decodes with **no header at all** (measured: 0 errors, same samples), so the header is only for outside readers.

## What asp5 does now (and where it differs)

- `flacio.c` is file only: `init_file` to write, `init_file` to read, verify on, whole take decoded into memory by
  `asp_pcm_load`. astream and aspace share nothing.
- `take_end_` (aspcore.c) does **whole-take work on the audio thread**:
  1. mono take: decode the entire take, copy one channel, re-encode the entire take (`part2`), keep the stereo in RAM
     for a move job;
  2. discarded take (bump/quiet): decode the entire take again to build a removal job;
  3. `asp_take_adopt`: fsync, SHA-256 over the whole file, rename, fsync dir, sql commit;
  4. the end move: `asp_job_take` decodes the entire take again.
- Measured cost per 76 s take (scaled from 27.6 s): encode with verify ~240 ms, decode ~50 ms. Mono path alone is
  ~290 ms. Observed slow ticks: 231, 250, 274, 324 ms, once per loop pass. That fits; it is not yet proven (the
  `slow_phase`/`adopt_slow` timing run will say).
- Cost grows with take length, so an hour-long take is a multi-second stall and gigabytes of RAM
  (32 bit stereo is 23 MB/min decoded).

Corrections to what I said earlier: encoding is cheap (0.3% of one core, worst tick ~1 ms, verify on or off). A
running hash of the file bytes is possible when there is no seek callback. The stall is whole-take work, not hashing.

## Design

### 1. flacio becomes the codec, nothing else

```
AspEnc *asp_enc_new( channels, rate, sink, user )   sink( bytes, n, samples, frame_no, user )   // = astream encoder_write
int     asp_enc_put( e, pcm, frames )
int     asp_enc_end( e )
AspDec *asp_dec_new( source, user )                 source( buf, *n, user )                      // = astream decoder_read
int     asp_dec_get( d, pcm, max_frames )           // frames out, 0 at end, <0 error; a bad frame is skipped
```

Same callbacks astream has, so astream can call this module instead of libFLAC directly, and the sink/source there
stay the packetiser and the jitter buffer. A raw-pcm module with the same six functions replaces it: `put` is a
copy, `get` is a copy, header is empty. Nothing outside this module includes a FLAC header.

World-readable FLAC (`a5import`, `a5export`, `aspflac`) uses libFLAC's file API in a **separate small file**
(`flacfile.c`), the only place `init_file` survives. Export needs it: only a finished file has a true length and MD5.

### 2. A take is a stream file, written as it is recorded

- sink = append to the `.part` fd; every ~1 MB `sync_file_range(WRITE)` so the kernel is already writing; at the end
  one `fdatasync`, `rename`, row. A crash leaves a `.part` that decodes up to the last whole frame (today it is
  unreadable), so `a5doctor` can recover it.
- Source = the file, read in blocks. No decoded take is ever held; a take can be hours long.
- Naming: `takes.<artist12>/<iter>/<off>.a5t`: the folder carries the first 12 hex digits of the artist ID, the file name
  has no prefix, and the header carries the full 128-bit ID (a mismatch between folder and header refuses the take).
  The key becomes `UNIQUE(artist, iter, off)`. **No hash.** Integrity at rest is the codec's own (FLAC: a CRC-8 per frame header and a CRC-16 per frame,
  checked on every decode). If a hash is ever needed it comes back as a column; the sink can feed a running SHA-256 for
  free, but nothing depends on one.
- Length: the row's `frames` and the file header's `frames` come from the core's own count.

### 2a. The take container: our header, then the codec's bytes

```
[ 96 byte header ][ codec payload, as the sink delivered it ]
magic "A5TK", version, codec id (4cc: "FLAC", "PCM "), artist id (16 bytes), channels n (<= 8), rate, bits,
finished flag, frames (u64), then per channel: peak, clipped samples, active ticks; and identical-channels flag
```

The header is written as a placeholder (`finished = 0`) when the take starts and patched with one `pwrite` at the end:
we own the fd, so "no seek callback" is a libFLAC constraint, not a file one. A crashed `.part` is recognisable
(`finished = 0`) and its payload still decodes, so recovery can count frames and adopt it. The container is the same
whatever the codec; only the payload changes. These are the facts of the recording, fixed once finished. Mix decisions
(level, pan, mono-as-used, on/off) stay in the log as `take_state`, as now: facts here, decisions there.

The header stores **metrics, not a verdict**: the same numbers the core already counts while recording (`t_peak`, `t_clips`, `t_act`, `t_ident`). Whether a take is mono, and which channel, is a function of those metrics (today's rule: one channel active at least 20 times more than the other, or both identical). It is evaluated when the take ends and the result is logged in `take_state`; any later open, or a tool with a different threshold, can evaluate it again from the file alone. Nothing is baked in.

### 3. The file is what came in; mono is a decision, not a format

Every take stores all the input channels exactly as received (a mono instrument on a stereo card is stored as two
channels). The metrics in the header say which channels carried signal; "mono, from channel src" is decided from them (see 2a),
and the mover then reads only that channel. This deletes `part2`, the
stereo-in-RAM buffer, the stereo-to-mono swap job and the end-of-take decode, and a told-mono take stops being a special
case in `take_put`. Measured on a real 27.6 s take (left channel 3.89 MB alone):

| second channel | stored size |
|---|---|
| digital silence, or identical to the first | 3.89 MB (free) |
| noise at -126 dBFS | 4.57 MB (+18%) |
| noise at -108 dBFS | 5.07 MB (+30%) |
| noise at -96 dBFS | 5.40 MB (+39%) |
| 4 channels, three silent | 3.89 MB (free) |

So a real interface's noise floor costs 20-40% on a mono take. That is the price of keeping the input as it came.

### 3a. More than two channels

The container and codec boundary are N-channel from the start (libFLAC allows 8; with more than two channels each is
coded independently and a silent one is free, measured above). The per-channel metrics generalise mono/stereo. The core
(tick buffers, gate, tank, sync) is still two channels; widening it later does not change the take format.

### 4. A move job streams

`AspJob` holds a decoder and reads the take in slices inside the tick budget, applying `-old +new` to the tank
exactly as now. It visits take frames in order instead of tank positions in order (which needs random access that
a stream file does not have). The "only behind the head" rule is unchanged (`lim`). Difference: for a take
**longer than the loop**, a level change reaches its successive loop-lengths one revolution apart instead of all at
once, i.e. a staggered fade rather than a step. The sum stays exact. The discard path becomes a normal move job on the
stored part, no special load. `asp_pcm_load` stays for the offline tools (small files only).

### 5. Disk on the tick

After 1–4 the tick does only page-cache writes plus, once per take, one `fdatasync`. Then sqlite: `synchronous=NORMAL`
in WAL mode (log inserts stop fsyncing; checkpoint fsyncs remain) with `wal_autocheckpoint` off and a small helper
thread checkpointing on its own connection every ~30 s. Only if the timing run still shows a stall from the final
`fdatasync` do we hand it to a finisher thread. I'd measure before adding any thread.

## Where the interface asks for fragile things (and what I'd do instead)

| demand | today | proposal |
|---|---|---|
| decide mono at take end | whole-take re-encode | mono is a flag; stereo stored |
| drop a bump / quiet take | decode, build job | ordinary streaming move |
| change a take's level | decode whole take into a job | streaming job |
| name by content | reread + hash | name by iter/off; CRCs are in the stream |
| long take longer than loop | position-ordered strided reads | sequential, staggered per revolution |

## One module, chosen at build time

The codec functions are the whole interface (`include/codec.h`). `make CODEC=flac` (default) builds `src/codec_flac.c`;
`CODEC=raw` builds `src/codec_raw.c` with the same header (put and get are copies, no header). `tests/test_codec.c` runs unchanged against both. `flacio.c` stays as the world-readable file adapter until step 4. astream links the same file.
Contract that makes this work for any codec: the encoder is given a block size, a `put` of one block produces exactly
one sink call (`samples > 0`), and the decoder returns one block per `get`. For astream that block is the tick, which is
what it does today.

What fits: any lossless, sample-exact, block-streamable codec (FLAC, raw PCM, probably WavPack or ALAC).
What does not: **mp3 or any lossy codec as the store.** The tank is exact sums: a take is added as recorded while it
is live, then later subtracted as *decoded*, so a lossy store leaves a residue in the loop. Encoder delay and fixed
frame sizes (1152) also break sample alignment, which is the point of the project. Lossy belongs on the wire (astream's
masked bits are already that) and in export. The header's codec id means a build without a codec says so plainly when
it meets a take written with another.

## Artist ID

A random 128-bit number, made locally the first time aspace runs and kept in `~/.config/aspace/artist`. No registry, so
nothing to contend over: 2^128 makes a collision a non-event, and an offline machine can mint one. The name people see
is separate and changeable (it lives in the session descriptor / peers table, not in any path). If identity needs to be
proved later (a hub, signed takes), the ID becomes the hash of a public key: same width, same place in the header, no
format change. Handles chosen by people ("knight") are display only, so they never need to be unique.

## Threads, and the cubieboard (measured on echo, 2026-10-28)

echo: Allwinner A10, one Cortex-A8 at 1 GHz, 1 GB, ext4 on an sd card (`commit=600`), 6.18 kernel. Timings of the card
(python, 55 KB appended per tick, about one stereo take tick):

| operation | median | p90 | worst |
|---|---|---|---|
| `write()` of a tick's bytes (page cache) | 0.5 ms | 0.5 | 1.2 |
| `fdatasync` after each tick | 17 ms | 91 | **1.5 s** |
| `fdatasync` after each 1.1 MB (a take being recorded for 60 s) | 181 ms | | **4.3 s** |
| rename + `fsync` of the directory | 43 ms | 67 | 147 |
| sqlite insert, `synchronous=FULL` (the default) | 13.6 ms | 17 | 114 |
| sqlite insert, `synchronous=NORMAL` | 0.2 ms | 0.2 | 0.5 |
| sqlite WAL checkpoint (after NORMAL inserts) | 1.05 s | | |
| sequential write | 5 MB/s | | |

The pipes to the card hold about 500 ms each way; a loop that is more than about 300 ms late underruns. So on this card
the page cache is free and **anything that waits for the card is a hazard**: one `fdatasync` can cost 4 s.

What the core still does that can wait for the card: the `fdatasync` in `asp_tw_close` at the end of every take; the dir
`fsync` in adopt; every sqlite commit (`synchronous=FULL` by default: several near the top of every pass); the automatic WAL
checkpoint; a read of a take that is not in the page cache (a job); the tank, if it is on the card (it must be in ram:
`--tank /dev/shm/...`).

Three ways to deal with it, in order of how much they ask:

1. **No threads, no waiting.** `PRAGMA synchronous=NORMAL`, `wal_autocheckpoint=0` (checkpoint at close), no `fdatasync` in
   the core, `posix_fadvise` read-ahead on takes (done). The kernel writes everything out within about 30 s. Cost: a power
   cut can lose the last half minute or so, and a row may outlive its take's last bytes; so on open the store checks the
   newest takes (header finished, size) and reports the doubtful. The core stays one serial loop. Not safe against a hard
   power cut on stage.
2. **One keeper thread, servant not controller.** As 1, plus a thread that does only what waits: `fdatasync` + dir `fsync` of a
   finished take, then a note to the core that it is durable; and the WAL checkpoint on its own connection. It shares no
   state with the core but a short mutex-guarded queue and a done flag per job; the core never waits for it (only
   `core_close` joins it). sqlite, the tank, the tick, the takes stay in the core thread. About 60 lines of pthread. A power
   cut costs only what was not yet synced, and no take is in the database before its file is in place and safe (its `take_added` event is the moment it became so).
3. **Do the waiting between ticks, in the loop.** Right after the output tick is written the card has its fullest buffer. Fine
   on a disk (stalls of 20-100 ms), not on this card (seconds).

Recommendation: 1 now (needed whichever), 2 for the cubie. The loop stays serial; the keeper is an I/O servant like the
kernel's writeback, not another place where decisions are made.

### The cubieboard as the baseline: where a tick goes (echo, 2026-10-28)

`tools/tickbench` times the parts of a tick; `bench.sh`-style runs (real-time simulated card, session on the sd card, loop in
`/dev/shm`, 150 s, takes coming every 2.7 s, `AUTOMIX overlap 2` so jobs run) give the whole picture. A tick is 200 ms.

| part of a tick | x86 | echo before | echo now |
|---|---|---|---|
| gate scan (filters + 75 transforms) | 1.2 ms | 115 ms | **43 ms** |
| gate switch | 0.1 | 6.4 | 5.3 |
| tank get / put / mix out, each | 0.03 | 1.2 | 1.0 |
| whole tick, average (sim run) | | 304 ms | **164 ms** |

What got the gate down: both channels in one transform (they are due together; the spectrum of each is recovered from the
bin and its mirror: half the transforms), and `-mfpu=neon -funsafe-math-optimizations` for `gate.c` on armv7 (the A8's vfp
is not pipelined). Its decisions are unchanged: `tests/gate_diff.sh` against asp4's own gate, 873 ticks on echo with the
flags and 1374 here, 0 differences.

What is left, from the `slow_phase` log of that run: `take_end` median 508 ms, p90 1.7 s, worst 11 s (the `fdatasync` of the
finished take, see above); `gate` median 48 ms, p90 64 ms; `moves` median 35 ms (the job budget of 48000 frames a tick).
So the core's own work now fits; the remaining stalls are the card. Nothing was run with the real audio device.

### With the keeper (echo, same run)

The keeper now puts each take in its place (folders, rename), syncs it, and checkpoints the log; the row of a take is made when
its file is safe. Same 150 s run on the card: `take_end` median 508 -> 76 ms, p90 1.7 s -> 84 ms, worst 11 s -> 177 ms; the
average tick 164 -> 123 ms. What was left of the card's stalls was the creation of the next take's file (`take_open`: two stalls
of 1.3 s in 52 takes, the create waiting behind the keeper's journal commit). So the keeper now makes the next part file ahead
of time (`tmp/spare_*.part`, two kept ready, handed out by `asp_tmp_path`; `asp_tw_open` accepts an empty file but never one with
anything in it); in two further runs `take_open` never took 20 ms. It is not a card-only measure: any storage is spared a
create in the loop. Not done: the job budget by time instead of by frames (`moves` takes 33 ms a tick on the A8 while a job runs).
The encoder is at level 2 (above), and with verify on.

### The encoder level (echo, 25 s of real takes, stereo, verify on)

| FLAC level | file size, % of raw (three takes) | cost on the A8, ms of every 200 ms tick |
|---|---|---|
| 0 | 73.7 | 11 |
| 2 (chosen) | 72.2 / 71.7 / 58.3 | 14 |
| 3 | 71.7 / 71.9 / 58.6 | 19 |
| 5 (libFLAC's default) | 70.3 / 70.3 / 57.8 | **50** |

Level 2 is used for takes on every machine (one format, whatever the box), verify stays on.

## Order of work

(2026-10-28: steps 1 and 2 are done. Step 2 in the code: `takefile.[ch]` (header, writer, reader, `asp_hdr_mono`); the store keeps
`takes.<artist12>/<iter>/<off>.a5t`, schema v2 (no blob_sha/md5, `UNIQUE(artist, iter, off)`), an artist id made on first
use in `$XDG_CONFIG_HOME/aspace/artist`; the core writes every take through the writer, stereo as heard, and decides mono
from the header when the take ends, so `take_end_` reads nothing back; jobs take a view (`src`) per side. Whole `make test`
passes with `CODEC=flac` and `CODEC=raw`. A session made before this is refused with a message. Not yet converted:
`tools/verify.py` and `tools/import_asp4.py` still speak the old layout - step 4.)

1. DONE 2026-10-28: `codec.h`, `codec_flac.c`, `codec_raw.c`, `test_codec` (round trips at 1/2/4 channels, no-header decode, 7-byte-at-a-time source, truncated stream, damaged block, bad args). Still to do here: check that finalized (old) flac files decode through `asp_dec_*` once the take reader exists.
2. Take writer/reader on the store (`asp_take_adopt` = fdatasync, rename, row); mono as metadata; tests: store, core.
3. Streaming `AspJob`; rebuild; tests: mix, job, tank, core (exact-sum tests already exist).
4. Importers/exporters and `a5doctor`, `verify.py` to the new paths; old sessions keep working (path fallback).
5. Timing run; sqlite pragmas; helper thread only if needed.
6. astream moves to the same module (its own branch; your decision, last).

## Open questions

1. Artist ID: 128 random bits made on first run and kept in the user's config (below).
2. FLAC for takes: level 5, verify ON in the store (about +20% of 0.3% CPU), off on the wire as astream has it.
3. Header metrics: peak, clipped samples, active ticks per channel, identical flag. Anything else worth keeping (rms, first/last active tick)?
4. astream onto the same module: last, on its own branch.
