# asp5

A looping recorder with measured zero latency, built for networked, multi-user playing.
Design in progress; see `docs/`.

- `docs/DATA.md`    data store: SQLite log + immutable FLAC takes + a derived loop cache
- `docs/EDGE.md`    audio edge contract, latency calibration, safe mode
- `docs/LESSONS.md` what asp4 taught us
- `src/aspflac.c`   raw 32-bit PCM <-> FLAC with libFLAC only (`make`)
- `tools/`          `import_asp4.py` (flat files -> session dir), `verify.py`

Build: `make` (needs libFLAC; by default a source tree in `$FLACDIR`, built into `build/flac`).
