# Audio edge contract

aspace is a pipe: ticks of 32-bit stereo 48 kHz PCM in on stdin, ticks out on stdout (one tick =
1/5 s = 9600 frames). It knows nothing about drivers or hardware. Whatever runs it is the *edge*:
aspface/SDL, an ALSA/JACK/CoreAudio adapter, a network feed from another machine, or a file.

## Rules for an edge
1. **One clock.** Input and output come from the same device clock. Separate devices drift; the
   adapter must resample, aspace will not.
2. **Never block lock.** aspace loops forever without losing lock as long as no samples are
   dropped or invented. Any drop (input overrun) or starve (output underrun) is reported, not hidden.
3. **Report drops.** The edge tells aspace over the control service: `EDGE drop N` / `EDGE starve N`
   (frames). aspace writes them to the log (`edge_drop`, `edge_starve`) and marks the take being
   recorded as damaged. After a drop the sync must be re-hunted before anything else is kept.
4. **Buffers.** Repeated drops mean an unstable system or driver buffers that are too short; the
   shell/GUI says so in plain words and suggests longer buffers.

## Calibrating latency
- **Loopback sync (default):** aspace writes the FSK sync, the edge returns it through a loopback
  (hardware, driver or cable) and aspace measures the whole round trip, edge buffering included.
  Re-measure on every start. The loopback is only needed for the hunt and may be off in play.
- **Acoustic chirp:** for users with no loopback. Speaker to mic, correlation gives the delay to
  about a sample. It includes the air path (about 3 ms/m), which is real latency for the player.
- **Declared latency:** only for edges whose driver we control. Not zero-latency by itself; offered
  as a tool to check reported latency against the sync test and see whether it is stable.
- **Sim edge** (optional): feeds output back to input with a fixed delay, no sound card, so a
  session can run headless (tests, CI).

## Safe mode
`SAFE on` / `SAFE off` are control events, logged as `safe_on` / `safe_off`. While safe, nothing
leaves the machine and no take is committed as published. aspace itself keeps recording whatever
passes the gate; publishing happens outside aspace (auto for live performance, or by marking a take
ready in keep mode).

## The sync hunt in detail (`include/sync.h`)
- One second at a time: `asp_sync_out` gives the second to play, `asp_sync_in` takes the second recorded
  while it played. Tone coding and decoding are asp4's (CPV/MSK), held to it by `tests/sync_diff.sh`.
- **Level:** starts at -48 dB (re asp4's level) and rises 6 dB for each second not heard, to at most
  `max_db`. Once heard it measures the input peak and moves the output in one step so the input peak
  lands on `target_db` (default -12 dBFS), never above `max_db`. If the shell has muted what is heard
  (`output_muted`), it starts at the top level instead; the shell does the muting around the hunt.
- **Polarity:** each second tries the louder channel then the other, each both ways up, and reports
  which one read. (asp4 only ever tried one polarity per second, alternating by second.)
- **Answer:** the median of `need` (3) readings at the settled level; if they spread by more than `tol`
  (2) samples the path is unsteady and it keeps trying until `give_up` seconds, then reports -1 with the
  spread. Zero crossings through real converters move by a sample with noise, so precision is +-1.
- A round trip of 0 frames is asp4's "not found" and cannot be reported.
- **Loop origin:** the loop will start at its top when sync is found (not at the elapsed offset); that is
  done in the tick loop, which sets the origin tick, not here.

## The core loop (`include/aspcore.h`)
`asp_core_tick(core, in, out)`: one tick (9600 stereo int32 frames) in, one out, no clocks or threads.
- **SYNC phase:** plays the sync tone, collects each returning second, runs the hunt. When found, the
  loop's **origin is the next tick**: the loop begins at its top then, and a tick's place in the loop is its
  distance from the origin, not the elapsed time. Recording position = origin-relative; the loop goes
  out ahead of it by the measured round trip so what is played against it lands back in place.
- **PLAY phase:** gate (learning waits for the returning sync to pass), takes recorded as FLAC streamed to
  `tmp/`, each tick fed into the tank at its level as it is recorded, take committed to the store
  at its end (one transaction: row, `take_added`, `take_state`). Frame 0 of a take is the start of its
  second of the loop, so it can be replayed into a rebuilt tank sample for sample.
- **Events logged:** sync_start, sync_found/failed, gate, new, end, top, wrap, safe_on/off, edge_drop/starve,
  resync, tank_dirty, error. Control calls: safe, resync, edge, gate, gate_relearn.
- **Automix, mono, level moves:** see docs/SHELL.md.
- **SAFE:** no takes are recorded (an open one is ended); the loop keeps playing and the gate keeps
  learning. Nothing new reaches the store, so nothing can be published.
- **Edge drop/starve:** logged; a take in progress is ended and marked `damaged`; the hunt starts again,
  so the loop top moves to a fresh origin. The loopback is the shell's business: on while phase is SYNC,
  off in PLAY (asp4's `mixer sync` / `mixer live`).
