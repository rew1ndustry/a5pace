# Index: where everything is

One line each. Written 2026-11-12 so that a later session need not scan the tree. Layout: `src/` programs and library, `include/` headers
(the library API, one header a module), `tools/` shell scripts and offline tools, `tests/`, `docs/`, `web/` (the a5web page),
`profiles/` gate profiles, `share/` and `etc/` installed data. `bin/` and `build/` are generated.

## Data, bottom up
- `aspchip.h/.c` the 128-byte chip header (crc, endian-safe pack/unpack). `asptank.h/.c` the loop file: chips back to back, streamed with `chip_read/write`.
- `takefile.h/.c` one take on disk (header + codec stream). `codec.h`, `codec_flac.c`, `codec_raw.c` the only place that knows the bytes. `flacio.*`, `aspflac.c` libFLAC.
- `aspstore.h/.c` a session = a directory + `session.db` (sqlite; schema `tools/schema.sql`, `src/schema_sql.h` is generated). State is a fold of a log; the `moves` table lists started moves. `keeper.*` the one servant thread (disk waits). `sha256.c`.
- `asppeaks.*` a level byte a tick per take. `settings.*` key=value setups and the config dir. `plat.h` the platform shims (byte order, fseek).
## The core (a pure function over buffers, no clocks)
- `aspcore.h/.c` the tick loop: sync phase, play phase, window of chips, moves/jobs, live feed, drain. `aspengine.*` the loop kept from the store (takes, placements, mix). `sync.*` the FSK round-trip hunt. `gate.*`, `gatesim.*` when sound starts.
## The library
- `make lib` builds `build/libaspace.a` from `LIBOBJ` (every module above, no main, no sockets, no audio device). `include/aspace.h` is the one header and says how a host opens, ticks and closes; `examples/embed.c` is the smallest host (`bin/embed`), `tests/test_embed.sh` its test. `a5pace` is one host of the library, `a5sdl` another.
## Programs (all `a5` names; `aspstore`, `asptank`, `aspflac` are low-level debug tools)
- `aspace_main.c` -> `a5pace`: `new`, `run` (pipe mode or `-S LAT` simulated card), the control port (`ctlsrv.*`). `a5control` (+ `ctlclient.*`) one command to a running space.
- Shells: `a5hell` (+ `a5term`, `a5map`, `a5mixer`) terminal; `a5web` (+ `web/index.html`, `web/a5web.js`) browser; both parse the mixing language in `a5mixer.c`.
- Edges: `tools/a5card` ALSA (arecord | a5pace | aplay), `src/a5sdl.c` SDL2 (portable, docs/GUI.md), simulated card in `a5pace -S`. `tools/a5start` starts a space on the right edge.
- Making and moving audio: `a5create`, `a5click` (+ `clicklib`), `a5import`, `a5export`; repair: `a5recover` (dead run), `a5rebuild` (last resort tank rebuild), `a5peaks` (levels for old takes).
- Gate: `a5tune`, `tools/a5cal` + `share/calibration.script`, `tools/gateprobe.c`, `tools/gatetest.c`. `tools/tickbench.c` timing on small boards. `tools/a5doctor`, `tools/a5play`, `tools/store.py` helpers.
## Tests (`make test`, about 5 minutes, `timeout 500`)
- C units: `test_chip test_codec test_takefile test_store test_tank test_sync test_core test_job test_mix test_gate test_keeper test_mixparse`. Shell end to end: `test_runner test_start test_web test_sdl test_import test_click test_export test_recover test_rebuild test_cal`. `gate_diff`, `sync_diff` compare with asp4's numbers.
## Docs
- Start: `DESIGN-NOTES.md` (firm design), `TODO.md`, `SHELL.md`, `DATA.md`, `STORE-DESIGN.md`, `EDGE.md`, `GUI.md`.
- Users: `A5HELL-GUIDE.md`, `A5WEB.md`, `CREATE.md`, `EXPORT.md`, `CALIBRATION.md`, `PROFILES.md`, `RECOVER.md`, `REBUILD.md`. Platforms: `PORTABILITY.md`, `CUBIE.md`. History: `LESSONS.md`, `A5HELL-PLAN.md`. `NAMING.md` the name rules.
