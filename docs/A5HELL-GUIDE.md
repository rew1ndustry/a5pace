# a5hell: the guide

a5hell is the window you play with. It shows the loop as a map, tells you what has just happened, and has a few keys.
It does not record anything itself: the space (`a5pace`) does; a5hell only looks and asks.

```
a5hell DIR              open the space DIR (it is started first if it is not running: live or safe as it was last time, in keep mode)
a5hell DIR --live       the same, but a space it starts is recording (--safe: not, whatever it was)
a5hell DIR --loop       the same, but a space it starts puts every take into the loop
a5hell DIR --dim        quieter colours
```
`--live`, `--safe` and `--loop` only matter when a5hell starts the space. Opening a space that is already running changes nothing.
It needs a terminal (a real one, any size; it follows the window when you resize it).

## What you see
```
LIVE keep   transylvanianLullaby                                   LIVE   <- red LIVE: recording. green SAFE: not
PLAY 12.4s loop 31.0/76.0s iter 110 takes 7 ...                           <- status in words
 103 ---===---------------::::::::------------                          <- one row an iteration, the whole loop across
 ...
 110 ::::::::::::::::::::::::::::::::::::::                              <- the latest iteration is the bottom row
 recorded: iteration 110 at 31s, 4.2 s long, peak -9 dB                    <- the last thing that happened
 input L ####.... R ###.....   output L ...                                <- levels, with a mark at the recent peak
 LIVE [l]ive [s]afe [g]ate [a]mode ...                                     <- keys
```
- **The map.** Each row is one trip round the loop; the left is the start of the loop, the right its end. `=` is a take that
  is in the loop, `-` a take that is muted or kept out. A take that runs past the end carries on in the next row, from the
  left. Where takes overlap, one in the loop hides one that is not. Blank is nothing.
- **Rows below a blank line** are the highest-numbered iterations above the one being played: the generated tracks (clicks, an
  imported file, numbered from 90001) and takes of older runs. They are part of the map: the cursor reaches them and every key
  works on them. An iteration with nothing in it (it went by while SAFE) is a blank row. A take whose levels were never worked
  out (`a5peaks DIR` does it) is drawn grey.
- **Colour** is how loud: green quiet, yellow about right, red peaking (levels are rms, -24 dB and -6 dB by default).
  Words and numbers are grey. A long loop on a narrow window shares a character between a few moments.
- **LIVE or SAFE** in the corners. SAFE: nothing is recorded, the loop keeps playing. Next to it, the mode: `loop`, `keep`,
  `hold`, `ping-pong`.

## Keys
| key | what |
|---|---|
| `l` / `s` | LIVE (record) / SAFE (do not) |
| `g` | the gate: then `l` listen again for the quiet (nothing playing), `+` more sensitive, `-` less (3 dB) |
| `a` | the mode: `l` loop, `k` keep, `0` hold off, `1`-`9` hold n, `p` ping-pong |
| `u` / `U` | undo the last take / the last iteration (not in keep mode) |
| `h` | history: go back to how the loop was. up/down, `.` from each take's next start, `!` now, esc leaves |
| `x` | export: `a` all takes, `m` the mix, `l` the loop (a5export, into DIR/export) |
| `w` | wipe the loop (it asks; nothing is deleted) |
| `r` | sync again |
| `f` | choose a gate profile (made with a5cal) |
| `T` | every take as text |
| `PgUp` `PgDn` `Home` `End` | scroll the map; `End` follows the latest iteration |
| arrows | move the cursor on the map; with shift they select |
| `?` | help |
| `q` / `Q` | leave a5hell (the space carries on) / stop the space |

## Mixing on the map
Move the cursor onto a take. **Left / right** jump to the start of the previous / next take in the row (past the first, onto the
iteration number at the left: then every take that starts in that iteration is the target). **Up / down** go to the nearest
row that has a take in it, and the take there nearest to the same column. The cursor is a solid white block. Keys typed
quickly are all acted on first and the screen is drawn once, so a run of arrows lands where it should. Every take that covers the
cursor's cell **lights up (reverse video), all along its length**, including the part in later rows: those are the takes the next
keys will change. When a take has just been recorded the cursor moves onto it (if you are following the latest iteration), so
`p` or `m` is at once about the take you have just played. With shift+arrows you select a block, and every take
the block touches is changed.

| keys | |
|---|---|
| `m` / `p` | mute / play (take out of / back into the loop), heard from the take's next start |
| `M` / `P` | the same, at once |
| `.` / `!` | switch it the other way, next start / at once |
| `+3.` `-2.` | gain by bit shifts: `+3` shifts every sample left 3 bits (louder, it saturates), `-2` right 2 bits (quieter); `/2.` pan 2 bits right (the left side shifted 2 more), `\3.` 3 bits left; end with `. ! m p M P` |
| `+1/2M` | gain and pan together, and mute at once |

Gain and pan are absolute: typed again, they replace. A sign or slash with no number keeps the take's value but makes it a
mix of its own, which the automix then leaves alone. esc throws away what you have typed and the selection.
To name a take instead of using the cursor, type its place: `123.15-2/2.` (iteration 123, the take at second 15),
`123m` mutes every take that starts in iteration 123, `123..` switches them all.

In **keep** mode takes are recorded but not put into the loop; put one in with `p`.

## If something looks wrong
- *"needs a terminal"*: a5hell cannot run from a script or a pipe. Use `a5control` for that.
- *A take shows as `-` with no colour, or the map is empty for old takes*: they were recorded before levels were kept. Run
  `a5peaks DIR` once (it works out the levels; the space can be left running).
- *Colours look wrong or the background is not black*: edit `~/.local/etc/a5hell.cfg` (see docs/SHELL.md), or try `--dim`.
- *The window is garbled after a crash*: type `reset`.
