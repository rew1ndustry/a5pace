# CLAUDE.md - Context & Token Efficiency Guidelines

## Critical Directives
- **talk to me** please apply your own discretion first, and talk to me, before blindy obeying what follows.
- **You are operating on a PRO subscription where every interaction re-reads and re-bills the full context.** Token conservation is a non-negotiable architectural priority.
- **Do not proactively scan directories or search files.** Rely strictly on explicit file targets passed directly in the prompt or command line.
- **Enforce Plan Mode:** If a task requires inspecting files outside the paths explicitly provided by the user, stop immediately, present your proposed shell commands and wait for manual user confirmation before executing them.
- **Avoid open-ended, repository-wide reasoning.** Limit your field of view to the immediate scope of the requested change, ask for permission to dig deeper if necessary or appropriate.
- **Almost always force binary exclusion:** When using grep, you must strictly include the -I flag to skip non-text files globally, unless you intend to search binaries also.

## Proactive Log & Context Management
- **Wipe Terminal Session Memory:** remind me to run `/clear` when appropriate, like at the completion of a bug fix or code porting milestone to erase cumulative conversational weight.
- **Log Archiving & Truncation:** Proactively check the size of any active debug or execution traces. If a trace approaches or exceeds 50 MB, compress it immediately into the local `logs/archive/` folder via `gzip` and truncate the active log file. Never let traces bloat your context footprint.
- **Rely on Pre-Compiled Errors:** ask me to pipe clean compilation logs or compiler warning files to you. Read *only* the targeted log file rather than inspecting the raw `.c` or `.h` source folders directly.

## Legacy Code & Structural Bounds
- **Maintain Fixed-Width Structs:** Retain the `bd_i32` / `bd_u32` typedefs in data headers. Never introduce or promotionally shift to native `long`, `size_t`, or pointer fields which break the 4-byte on-disk record profiles.
- **Code Alignment Safety:** When reviewing older string or field copies, match byte boundaries using bounded copies (`strncmp`, `memcpy`). Avoid C-string length assumptions (`strcpy`, `strlen`) which cause stack overruns under modern memory layouts.
- **Escape Menu Loop Runaways:** Ensure shell execution wrappers check for valid terminal states (`[ -t 0 ] || exit 0`) to prevent dead-terminal respawn loops from pinning CPU cores.

## nut added suggestions, apply only if reasonable
- run searches quietly or pipe output to a file so you don't dump thousands of lines into the chat context.  is this rule redundant?
- you may rewrite this file to make it more efficient, if you can, but please let me know.

## Firm design rules (summary; full text with dates and reasons in docs/DESIGN-NOTES.md - read it before touching aspcore.c, the tank or jobs)
- A gain/pan change is a **move**: an additive job on the looping tape (tank += to - from). Order does not matter. NOW (`!`) starts at the chip being played; a normal move waits for the output to reach its take's first chip. Moves are never cancelled or dropped; **quit = drain**.
- **No dirty tank, no rebuild, ever** in aspace or its core (only `a5rebuild`, outside it). Started moves are rows in the SQL `moves` table, resumed after a kill. Safe mode never touches queued moves.
- **Chip model**: the tank is chips of one tick, each with its own full header; no tank header, no seeks in the loop. One read (front) and one write (back) per tick through a window of chips; jobs edit the front chip, the live feed the record head. A take's stored state is its state *before* a job; it becomes `to` only when the job completes.
- A started job is never cut short; the budget (`--budget PCT`, default 80) gates only jobs not yet begun. Take files are read forward only.

## a5space project conditions
- you are in charge of git and the github repository, generally keep branching, so we can get back to stuff if necessary, and otherwise commit and update when appropriate.
- you are in charge of ~/arb/asp5 folder and are invited to organise as efficiently as possible, use archive, and claude ignore - if you can overide when necessary.
- migrate to ~/.local/bin - keep up to date, and tidy.  .sh is for shells, .py is python, else is binary, create/use ~/.local/etc for configuration and other files.
- everything in system folders is prefixed a5, and s words, like a5pace and a5hell, are spelled as in these examples.
- a5pace file extensions are .a5x where x is our own code, and .a5p is for anything with a clickable gui.
- unless we go public, nothing is legacy or needs to be preserved, i will take care of exporting/storing anything i need to keep.
- pretend we will go public, and maintain user (dummy guide) as well as developer documentation, just in case.

## Gain rule (nut, firm)
- Gain, pan and the sync tone are **bit shifts only**: no dB, `pow`, `log`, multiplies in anything that changes a sample. Shell `+N` = left shift N bits (louder, saturates), `-N` = right shift (quieter); pan `/N` right, `\N` left = the far side shifted N more. Field `gsh` (was `gdb`); `asp_place_shifts` is the one place it becomes tank right-shifts. `--level N` on a5click/import/create stays a right shift. One bit is about 6 dB, only ever said in prose.
- The smooth output fade in `asp_mix_out` is deliberate: leave it. Real gain maths belongs to future envelopes only. Meters/peaks/gate/a5tune keep their dB measurement maths (they only read levels).

## Where things are (do not scan the repo; read these)
- `docs/INDEX.md` the map: every source file and doc in a line. `docs/DESIGN-NOTES.md` firm design and old status. `docs/TODO.md` open work. `docs/GUI.md` the GUI decision (a5web is the display; a5sdl the portable audio edge). `docs/A5HELL-PLAN.md`, `A5HELL-GUIDE.md`, `SHELL.md`, `A5WEB.md` the shells.
- Testing: `make test` (use `timeout 500`). No tmux: `screen -dmS`. Simulated space: `A5START_EDGE=sim a5start DIR [--live]`. Stop spaces with `a5control DIR QUIT`; never `pkill a5pace`. Install: `make install PREFIX=$HOME/.local`, reinstall after every change.
- Habits that worked: one step, test, commit with the Co-Authored-By line, say plainly what was and was not tested, `make test` before every commit, ask before outward or destructive actions.
