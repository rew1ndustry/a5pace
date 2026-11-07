# a5hell upgrade: plan (nothing built yet)

Status: agreed in conversation 2026-11-06. Not yet read: `src/a5mixer.c`, `src/gate.c`, `tools/a5start`, the take-close
code in `aspcore.c`. Step 0 reads them and may change the details below, never the intent.

## What the user sees
```
LIVE keep   transylvanianLullaby    PLAY 12.4s loop 31.0/76.0s iter 110 takes 7    LIVE      <- top: corners + automix after LIVE/SAFE
                                    gate -12dB  lat 627ms  profile x
 103 ---===---------------::::::::------------    <- one row per iteration; X = the whole loop (position in the loop)
 104 ......                                          blank = nothing, --- muted, === / ::: live; shade = peak rms
 ...                                               older iterations above, as many as fit, scrolls (PgUp/PgDn/Home/End)
 110 ::::::::::::::::::::::::::::::::::::::          the latest iteration is always the bottom row
 input  L ####....   R ###.....   output ...        <- meters (live level)
 [l]ive [s]afe [g]ate [a]mode [u]ndo [x]port [h]istory [?]  last event in words              SAFE      <- bottom corners
```
- Fills the terminal (TIOCGWINSZ, redraw on SIGWINCH, fallback `$COLUMNS`/`$LINES`, then 80x24). Top and bottom lines are
  reserved for details and meters; everything between is the map.
- **Cell.** Loop is T ticks (0.2 s each; 380 here), usable width W columns. If T <= W each tick is W/T characters of `-` or
  `=`; otherwise each character is N = ceil(T/W) ticks (80 columns: N = 6). The scale depends on the loop length only, so
  it is the same on every row and nothing has to be scanned first.
- **Takes that run past the loop end** continue into the next iteration's row, from the left, and may span many iterations.
  A take starting in iteration i at offset `off` with length L has a segment in each row i .. i+floor((off+L)/loop).
- Where takes overlap in one cell: live over muted, shade is the max peak of everything in the cell.
- **LIVE** red, **SAFE** green, in all four corners; automix mode follows it top left (`LIVE keep`). Everything uncoloured is grey.
- Event log shrinks to one or two words-lines at the bottom. `h` history stays its own page; `t` (mixing) goes away
  because the front page does its job.

## Colours
- Default: bright 16-colour ANSI. Green = quiet, yellow = dialled in, red = peaking. Grey (white 37, not bright black 90,
  which vanishes on some themes) for the rest. `--dim` uses normal intensity and dims what it can; LIVE stays readable.
- Dark background is enforced by painting: set black, clear, paint every cell on black, reset on exit (a program cannot
  change the terminal's own background). Alternate screen buffer in, out on exit.
- `~/.local/etc/a5hell.cfg` (created with defaults by the installer; absent file = built-in defaults):
  `green_below_db -24`, `red_above_db -6` (yellow between), colour codes per role, `paint_background 1`, `dim 0`.
- Possible later: 256-colour ramp when `COLORTERM`/`TERM` allow it.

## Starting, and the keys
- Shell finds no running space: starts it through `a5start`, **safe** by default, or live if `a5hell --live`; the flag
  is passed to the space. Automix: **keep** by default, `a5hell --loop` starts in loop mode. Both apply only when the shell
  starts the space.
- A shell attaching to a running space changes nothing: it shows the state it sees and sends keys when asked.
- `l` LIVE, `s` SAFE (replacing `space`, which goes). `g` opens a gate menu: `l` learn the floor, `+` more sensitive,
  `-` less (3 dB), as `a` does now. `+`/`-` are free elsewhere.
- Existing keys kept: `a` mode, `u`/`U` undo, `x` export, `w` wipe, `h` history, `T` takes as text, `r` sync, `?`, `q`, `Q`.
- **To decide at step 0** (clashes with the mixing language): `p` is profile today but P/p/M/m are terminators in the
  edit language; proposal: profile moves to `f`, and bare `m p M P . !` act on the take under the cursor or the selection.
  Keep mode's `m`/`M` (LASTMIX) become a special case, since the cursor starts on the newest take.

## Cursor and editing (the `t` screen, on the front page)
- Arrows move a cursor over the map (row = iteration, column = place in the loop); shift+arrows select; PgUp/PgDn/Home/End
  scroll; the cursor at the bottom row follows the latest iteration. **Every take covering the cursor's cell is
  affected** (no choosing, no cycling: takes never overlap completely, and if they do it is allowed to happen). All the
  cells of all those takes are highlighted, underlined where the terminal can (SGR 4; reverse video as the fallback),
  including their continuation segments in later rows. A shift+arrow selection affects every take it touches, the same way.
- The `a5mix_parse` language is reused unchanged: `[ITER[.TAKE]] [+N|-N] [/N|\N] then . ! m p M P`, with no ITER meaning
  the cursor or selection. A command starts at a digit, `+`, `-`, `/` or `\`, so these do not collide with menu keys.
  `iter.take` addressing is as now (`123.15`, `123..` the whole iteration).

## The data this needs (the main work)
Per-tick levels are not stored anywhere. Today the log's `end` event has one peak pair per take only.
1. **Core**: at each tick of a take, keep the rms in dB (one byte, 0.5 dB steps, max of the two channels) and write it with
   the take when it closes (new `take_peaks` table or blob; schema bumped, nothing legacy per CLAUDE.md). The open take's
   array is held in memory so the live take colours as it records.
2. **Control port**: `MAP from_iter to_iter` returns one line per take: `iter off len status hexpeaks`. Lines then `OK`
   as for every command. The shell asks for the visible window, and refreshes the bottom rows about once a second.
   Also needed: input/output level in `STATE` (or a `METER` command) for the meters.
3. **Backfill tool** `a5peaks` (or in `a5recover`): decode the existing takes once and fill the table, for the test data.
   Test on a **copy** of `~/Desktop/transylvanianLullaby`, never the original.
4. Cell colour = max over the ticks in the cell; thresholds from the cfg.

## Code shape
`a5hell.c` is 273 lines and the draw loop gets big. Split: `a5term.c/.h` (size, raw mode, colour, alternate screen, key
decoding including arrows and shift+arrows; the only file using termios/ioctl, so a Windows console version is one file),
`a5map.c/.h` (cells, scroll, cursor), `a5hell.c` (loop and keys), `a5mixer.c` (parser kept; screen part retired).
Bounded copies only; no on-disk struct change in the shell. Redraw by overwriting lines with `ESC[K`, not full clears.

## Order of work (each step commits on branch `a5hell2`, tested before the next)
0. DONE 2026-11-06: read `a5mixer.c`, `gate.c`, `tools/a5start`, `aspcore.c` take close; key clashes settled; findings below.
1. DONE 2026-11-06 (notes below): `a5term`: size, colour, painted background, corners, `--dim`, cfg file; `l`/`s`/`g` menu, `--live`, `--loop`, default keep.
2. DONE 2026-11-06 (notes below): Core peaks + `MAP` + backfill; check against decoded audio on a test take.
3. DONE 2026-11-06 (notes below): Map drawing, cell scale, wrap into later rows, scrolling, latest at the bottom.
4. Cursor, selection, editing keys wired to the mixer language; remove `t`.
5. Meters.
6. Docs: `SHELL.md` and the help screen, the user guide, a README line.
Test with the simulated card (`-S`, `--fast`, `SIM BURST`) for a clean session, run in a pty (tmux `capture-pane`) at 80x24
and larger, then on the copy of transylvanianLullaby.

## Portability
macOS: fine (ANSI, POSIX). Linux: fine. Windows: the drawing is ANSI (Windows 10+ Terminal, or conhost with virtual terminal
processing); the shell also uses `poll`, sockets and `fork` for export, so native Windows needs a port of a5term and the
export call; WSL works as is. Plan: do the split now, port later if wanted.

## Step 0 findings (read 2026-11-06)
- **Keys settled**: profile `p` moves to `f`. Bare `m M p P . !` (and `+N -N /N \N` starts) act on the take(s) under the
  cursor or selection; `m`/`M` LASTMIX stays as the keep-mode shortcut and is just the same thing with the cursor on the
  newest take. `l` = LIVE, `s` = SAFE, `g` = gate menu (`l` learn, `+`, `-`: 3 dB, replacing today's `l`, `g`, `G`). Free
  keys used: `f`. A command starts at a digit, `+ - / \`; the parser needs no change.
- **Terminal**: `a5mixer.c` already has a usable `getkey` (arrows, shift via `;2`, 40 ms ESC timeout) and size via TIOCGWINSZ;
  both move into `a5term.c`. Its `K_NONE` on PgUp/PgDn/Home/End: those finals (`~` after digits, `H`, `F`) must be added.
  The mixing screen (rows/cells/`GRID`) is retired once `MAP` exists; the `GRID` control command can then go too.
- **Gate/overlaps**: the gate does not end takes. A take ends in `aspcore.c` after `HANG` = 10 ticks (2 s) with no gate hit
  (`play_tick`, `++c->shh < HANG`); the gate's own hang is only 0.25 s and a pre-roll of 50 ms. Only one take is open at a
  time, so overlaps cannot happen by time in the current code. The 23 overlapping pairs come from `off` being whole
  seconds (`take_new`: frame 0 is the start of its second, zero-padded), so neighbouring takes can overlap by under 1 s, and
  from older takes recorded by earlier code. Quick end is `HANG`, not the gate; if the user finds takes cut too soon,
  raise `HANG` (core) rather than touch `gate.c`. Not changed.
- **Where peaks go (step 2)**: `take_put` is the one place every frame of a take passes (plus the zero lead-in); per-tick
  level is computed there from `pcm` for `ASP_TICK` frames, `t_frames / ASP_TICK` indexes the array. `take_end_` stores the
  array with the take (`asp_take_adopt`) and writes it where `lt` is built; discarded takes drop it. Lead-in ticks hold
  "no level". Mono-fixed takes (`t_fix`): use the chosen channel.
- **Start**: `a5start DIR` takes args from `DIR/start` (`args ...`) and passes `$args` to `a5pace run` / `a5card`, and takes
  no extra arguments of its own after `DIR` (its `shift` swallows them). So `a5hell --live` needs `a5start` to accept
  `a5start DIR [--wait] [--live]` and append the flag to the args, or the shell passes `A5START_ARGS`. Small change, step 1.

## Step 1 notes
- Built: `src/a5term.c/.h` (size, raw, alt screen, painted bg, SGR roles, cfg, keys incl. PgUp/PgDn/Home/End), `share/a5hell.cfg`
  (installed to `$(PREFIX)/etc` if absent), `a5hell.c` now fills the terminal (corners, status line, log at the bottom
  of its rows, keys line), `l`/`s`, `g` menu (`l` `+` `-`), `f` profile, `--live` `--loop` `--dim`, needs a tty.
  `a5start DIR [opts]` passes opts on to the space. Tested on a simulated space in `screen` (80x24): starts live+keep,
  `s`, `g+` (gate 18 -> 15), `?`, `q`; `--dim` gives normal-intensity colours.
- Left for later: the `t` screen (a5mixer.c) and the `?` help page are not painted/fitted yet (help is longer than 24 rows;
  step 6 rewrites it); `SHELL.md` and the user guide still describe the old keys (step 6). `--loop` after the space
  already runs is ignored by design.

## Step 2 notes
- `include/asppeaks.h`, `src/asppeaks.c`: the byte format (documented in `docs/DATA.md`), used by the core and by `a5peaks`.
  Schema is v3 (`peaks` table); a v2 session is upgraded when a writer opens it, a read-only open of v2 just has no peaks.
- Core: `take_put` feeds `AspPk`; `take_end_` stores it with `asp_peaks_set` after the take is adopted; the open take's
  array is read in place. `asp_core_map` + control `MAP [from [to]]` (lines `iter off frames in|out|open tick0 hex`, 500
  ticks a line; `-` = no levels). `METER` is left to step 5.
- `a5peaks DIR` backfills (both channels, the louder, so a take recorded from one input may show the other's noise).
  Test copy: `scratch/tl` (session.db + takes of transylvanianLullaby, 180 takes, 4 s to backfill, 36892 ticks). Checked: tick count
  = ceil(frames/9600) for all 180, no tick's rms above the header's peak, largest peak-minus-rms gap 24.5 dB. A live
  simulated space (SIM BURST 0.3, 0.1, 0.02) gave 206, 186, 159 (-17.5, -27.5, -41 dB) and the open take showed the same.
- `a5recover` does not make peaks for takes it puts back: run `a5peaks` after it. `tests/test_start.sh` runs a5hell
  under `script` now (it needs a terminal). No unit test for `asp_pk_add` yet.

## Step 3 notes
- `src/a5map.c/.h`: `map_line` (MAP reply into a take list, chunks joined), `map_scale` (ticks and characters per cell),
  `map_draw` (rows of iterations from a screen row; live `=` over muted `-`, colour by the cell's peak using the cfg thresholds;
  takes run into later rows from the start iteration). `a5hell.c`: `map_update` asks STATE and `MAP top-60 bottom` about once a
  second, at once on a key, an event or a scroll; PgUp/PgDn/Home/End scroll, End follows. Layout: row 1 corners, 2 status, 3..h-3
  map, h-2 and h-1 the last two events, h keys. Meters (step 5) will take the row above the keys.
- Follows the loop's current iteration (`iter` in STATE), not the newest take: after a restart the count starts again and new
  takes fill low numbers (they were found at iteration 3, 4 on the test copy while old ones go up to 110). Tried a `last_iter`
  in STATE, reverted. A take that began more than 60 iterations above the top row is not drawn into it.
- Tested in `screen`/`script` on the copy (`scratch/tl`, a simulated space): 80x24 and 100x30, scrolling, colours present
  (green, yellow, red), live burst appears as `=` in the current row while REC. Not tried: terminals above 380 columns
  (several characters per tick), a real card.
- The `t` screen still exists (step 4 retires it) and still draws its own way.

## Open points
- (settled) Overlapping takes under the cursor: all are affected and all are highlighted.
- (checked) overlaps: see Step 0 findings. The map shows them as live over muted. Raising `HANG` is the user's call.
- Whether the space or the shell should hold `--loop`/keep after a restart is not needed: keep is the shell's default at start.

## Resume notes (everything learnt, so a cleared session can carry on from this file alone)
Working rules (CLAUDE.md): plan before acting; no repo-wide scans; `grep -I`; talk to the user first; commit on branches;
installs go to `~/.local/bin` (.sh shells, .py python, else binary), config in `~/.local/etc`; system names prefixed a5.
The user wants usage kept low and asked for the plan before any code; step 0 is done; step 1 waits for the user's go.

Facts about the current code (as read 2026-11-06):
- `src/a5hell.c` (273 lines): poll loop on stdin, the event socket and the export pipe; `LINE` is asked every 250 ms and
  parsed with `strstr` ("SAFE", "automix ..."); keys are a `switch`; `a` `x` `w` `p` `h` are sub-prompts that clear the
  screen and block on `read`. `draw()` prints the header, up to 8 events (`events[8][220]`) and the status line.
  `pretty()` turns `@tick type json` log lines into words. Commands: `ctlc_cmd(fd, "CMD", callback, user)`, replies are
  lines then `OK`/`ERR`. Keys now: space safe/live, r sync, l relearn, g/G gate -3/+3, a mode, t mixing, T takes, u/U undo,
  m/M lastmix (keep mode), x export, p profile, w wipe, h history, ? help, q leave, Q stop aspace.
- `src/a5mixer.h/.c`: `a5mix_parse(str, MixCmd*)` (kept) and `a5mixer(cmd, ev, space)` (the screen, to be retired).
- `docs/SHELL.md` documents the shell (keys section near the end); the help screen text is in `help()` in a5hell.c.
- Control: `STATE` json (live, tick, iter, loop_frames, loop_takes, automix, gate_db, take_open, take_iter, take_off ...),
  `LINE`, `SAFE`/`LIVE`, `AUTOMIX LOOP|KEEP|OVERLAP n|SWAP`, `TAKES`, `HISTORY n`, `UNDO`, `RESTORE`, `WIPE`, `GATE n|+n|-n`,
  `RELEARN`, `SIM BURST`, `STEP`. `aspace_main.c` handles them (~line 285 on). aspace starts SAFE unless run `--live`.
  `a5start DIR` starts a space; `DIR/start` can hold `args ...`; `A5START_EDGE=sim|card`.
- Store: `session.db` (sqlite, schema v2 in `src/schema_sql.h`): `takes(id, artist, iter, off, frames, rate, channels, bits,
  status trk|mix|off, lev_l, lev_r, wrapped, len, gone, t_start)`, `log(seq, tick, wall, actor, type, args_json)`.
  Takes are files `takes.<artist>/<iter>/<off>.a5t`. `off` is whole seconds into the loop. 48000 Hz; a tick is 9600 frames
  (0.2 s). The log's `end` event has `peak_db:[L,R]` for the whole take only: no per-tick level exists yet.
- Test data `~/Desktop/transylvanianLullaby` (do not modify; copy to `scratch/` first): loop 3,648,000 frames (76 s, 380 ticks),
  180 takes in 98 iterations (0..110), 173 status `off`, 7 `mix`; 56 takes run past the loop end; 60 iterations have more than
  one take; 23 pairs overlap inside an iteration. `sqlite3 -readonly` works for looking.

Decisions (all from the user): see sections above; plus colours bright by default with `--dim`; thresholds -24 / -6 dB in
`a5hell.cfg`; black background painted; `l`/`s`/`g`-menu keys; start safe unless `--live`; keep mode unless `--loop`;
no automix persistence; wrapped takes continue into later rows; overlapping takes all affected and all highlighted.
