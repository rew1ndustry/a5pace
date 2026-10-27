# asp5

A looping recorder with measured zero latency, built for networked, multi-user playing.
Design in progress; see `docs/`.

- `docs/DATA.md`    data store: SQLite log + immutable FLAC takes + a derived loop cache
- `docs/EDGE.md`    audio edge contract, latency calibration, safe mode
- `docs/SHELL.md`   the runner, the terminal shell, the control port and the headless backend
- `docs/LESSONS.md` what asp4 taught us
- `include/aspstore.h`, `src/aspstore.c` the store library (C, sqlite + libFLAC); `src/aspstore_cli.c`
- `src/aspflac.c`   raw 32-bit PCM <-> FLAC (`bin/aspflac`)
- `tools/`          `import_asp4.py` (flat files -> session dir), `verify.py`

Build: `third_party/fetch.sh` (sqlite), then `make` and `make test`. libFLAC is built from `$FLACDIR` into `build/flac`.
