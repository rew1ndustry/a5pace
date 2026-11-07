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
  scroll; the cursor at the bottom row follows the latest iteration. The take under the cursor is the one covering that
  cell in that row, newest first if several (Tab to cycle; to confirm). It is highlighted.
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
0. Read `a5mixer.c`, `gate.c`, `tools/a5start`, `aspcore.c` take close; settle the key clashes; update this file.
1. `a5term`: size, colour, painted background, corners, `--dim`, cfg file; `l`/`s`/`g` menu, `--live`, `--loop`, default keep.
2. Core peaks + `MAP` + backfill; check against decoded audio on a test take.
3. Map drawing, cell scale, wrap into later rows, scrolling, latest at the bottom.
4. Cursor, selection, editing keys wired to the mixer language; remove `t`.
5. Meters.
6. Docs: `SHELL.md` and the help screen, the user guide, a README line.
Test with the simulated card (`-S`, `--fast`, `SIM BURST`) for a clean session, run in a pty (tmux `capture-pane`) at 80x24
and larger, then on the copy of transylvanianLullaby.

## Portability
macOS: fine (ANSI, POSIX). Linux: fine. Windows: the drawing is ANSI (Windows 10+ Terminal, or conhost with virtual terminal
processing); the shell also uses `poll`, sockets and `fork` for export, so native Windows needs a port of a5term and the
export call; WSL works as is. Plan: do the split now, port later if wanted.

## Open points
- Take under the cursor when several overlap (proposal above).
- 23 pairs of takes overlap inside one iteration in the test data (old ones, e.g. 1.3 and 1.50). The user's view is that the
  gate is too quick. Check in `gate.c` at step 0 and note it here; the map will show such overlaps as live over muted.
- Whether the space or the shell should hold `--loop`/keep after a restart is not needed: keep is the shell's default at start.
