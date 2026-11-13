# a5web: the browser shell

`a5web DIR [--port N] [--www DIR]` serves a page on `http://127.0.0.1:5055/` (default port) for the running space DIR.
Open it in a current browser. Checked with headless Chrome (data, hover, keys, menus); in Firefox the page was seen to run and lay out, but a live-data screenshot was not possible, so try it there and say what is off. It shows what a5hell shows, drawn rather than typed, and takes
the same keys plus clicks. The space does not know it is there: a5hell, a5control and a5web can all run at once.
Install with `make install`: the program goes to `~/.local/bin`, the page to `~/.local/etc/a5web/`.

## Using it (users)
- **The map.** One row an iteration, the loop across. A take is a ribbon: the thicker, the louder (rms). Green is quiet, yellow
  middling, red loud (the thresholds of `~/.local/etc/a5hell.cfg`). Takes out of the loop are faint. The pale column is the playhead.
- **Hover** over a take and it lights up, with its details on the bottom row: iteration.offset, length, in or out of the loop,
  gain and pan, mono or stereo, the level of the tick under the pointer, and whether a move is pending.
- **Select** by clicking a take (shift adds); click an iteration number for the whole iteration. Arrow keys move from take to
  take (left / right) and row to row (up / down); shift adds; esc clears.
- **Mix.** Type as in a5hell: `+2` gain in bits, `-1` quieter, `/1` `\1` pan, optionally with an iteration or take first
  (`7`, `7.3`), then a terminator: `.` queue for the take's next start, `!` at once, `m` mute, `p` play, `M` `P` at once.
  It acts on the selection, or on the take under the pointer if nothing is selected.
- **Buttons and keys.** `l` live, `s` safe, `r` sync, `u` / `U` undo take / iteration, `g` gate, `a` mode, `w` wipe, `h` history
  (click or `.` puts the loop back from each take's next start, shift-click or `!` at once), `T` takes as text, `?` the keys.
  Wheel, PgUp, PgDn, Home, End scroll; End follows the newest iteration.
- **Right-click.** On a take: a menu with mute and play, and sliders for gain and pan (bits, -8 to +8; pan runs `\8` left to
  `/8` right). Letting go of a slider queues the move for the take's next start; hold shift for at once (the same for mute / play).
  With several takes selected it acts on all of them. Anywhere else: live / safe, loop / keep, the gate (learn, more, less),
  output gain (forget clips) and **Quit...**, which asks "are you sure" and then stops the space (it drains: moves under way
  finish) and exits a5web. Esc or a click outside closes a menu. The **Quit** button on the top row does the same as the menu's Quit… (it asks first).
- **Window title.** The space's name, or the folder's name if the space has none, then ` - a5web`.
- Not in the web shell yet: `x` export, `f` profile (use a5hell or a5export).

## How it works (developers)
- `a5web DIR [--live|--safe] [--no-start] [--quiet]`: if the space is not running, a5web starts it with `a5start DIR` (a5start decides card or simulated, and passes `--live` / `--safe` on to a5pace); `--no-start` does not. It talks on stderr about what it does (starting the space, the address, every write the page makes); `--quiet` silences it, and a5start with it. Starting happens once, at launch: if the space dies later the page says "no space"; restart a5web.
- `src/a5web.c`, one child process per connection, a fresh control connection (`ctlclient`) per request. No state.
- Reads are GET: `/api/dir` (the folder's own name) and `/api/state|line|meter|takes|grid|history|map?from=&to=|maphigh?n=&above=|cfg` return the control port's lines as
  text. `/api/watch` is the log as server-sent events (`WATCH`); the page ignores the `top` and `wrap` ticks.
- Writes are POST only, with an `Origin` equal to our own address, and are a closed list: `/api/do?c=NAME` (live, safe, sync, quit, gainreset,
  undotake, undoiter, relearn, gate_more, gate_less, wipe, wipeall, mode_*, restore) and `/api/mix?s=TYPED&t=iter.off,...`.
  A request never carries a word to the control port: a5web builds the line. `quit` (QUIT, then a5web ends itself) and `gainreset` are in that list; REVERT, EDGE, EVENT, CLICK, SIM are not reachable.
- The mixing language is parsed once, in `a5mixer.c`, linked into both a5hell and a5web. The page sends what was typed so far; a5web
  answers `WAIT`, `BAD`, `NONE` (no take) or `OK n` (n takes sent). The page holds no grammar.
- The context menus are drawn on the canvas (`openTakeMenu`, `openSpaceMenu`, `openQuit`); sliders send `+N.` / `/N.` through `/api/mix`. Only `index.html` and `a5web.js` are served, by fixed name; the `Host` header must be the loopback address (dns rebinding).
- Listens on 127.0.0.1 only. Reaching it from another machine needs a tunnel (`ssh -L`), not a changed bind.
- `web/a5web.js` draws on a canvas (no libraries, no build step): rows from `MAP`, geometry as a5hell (tick k of a take at second
  `off` is row `iter + (5 off + k) / T`, column `(5 off + k) mod T`), level byte = `1 + 2 (dB + 120)`.
- Test: `tests/test_web.sh` (relay, refusals, a write reaching a take). The drawing is checked by eye: headless Chrome with a
  stubbed `EventSource` (a never-ending stream stops it taking its screenshot).
