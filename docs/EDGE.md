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
