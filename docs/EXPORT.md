# a5export: the audio out, as flac

```
a5export DIR loop|mix|all [--to FOLDER] [--prefix NAME] [--fold] [--cleanup] [--dry]
```
Works whether or not the space is running. It only reads the session (never writes to it) and runs at the lowest cpu
and disk priority, so a running loop is not held up.

| what | writes |
|---|---|
| `all` (default) | every take, as it was recorded: no gain, no pan, muted ones too |
| `mix` | only the takes that are in the loop, otherwise the same |
| `loop` | the loop as it sounds, one file: `loop.flac` |

**Where.** `DIR/export/` (all three modes together), or `--to FOLDER`. Every call brings the folder up to date: files whose
source has not changed are left alone, new or changed ones are rewritten, and nothing is ever deleted.

**Cleanup.** `a5export DIR mix --cleanup` (the mix export only) moves the takes an earlier mix export wrote and that have left
the loop since into the system trash (`gio trash`), or into `DIR/trash/` if there is no trash to be had. A take that an `all`
export also wrote is left alone.

**Names.** A take is `PREFIX_ITER_OFF.flac`, iteration and offset (seconds) each zero padded to five digits: take 7.3 of the
session `tabcreek` is `tabcreek_00007_00003.flac`. PREFIX is the name of DIR, or whatever `--prefix NAME` (or `--prefix=NAME`)
says. The state of the take (in the loop or muted) is not part of the name. The loop is `PREFIX_mix_ITER.flac` at the iteration
it was taken; earlier ones stay, each is a moment. Generated tracks (clicks, imported files) are numbered in the 90000 series:
`tabcreek_90001_00000.flac`. Nothing else from the session is used in a name.

**Fold.** `--fold` cuts a take that is longer than the loop (counting where it starts in it) into clips of exactly the loop
length, the last one the tail, padded with silence: `tabcreekg2_00001_00006_000.flac`, `_001`, and so on. A take that fits in
the loop is one file with no `_nth`.

**Takes.** One file for each take. It starts where the take starts in the loop (a take that begins 3 s in has 3 s of silence
first) and is padded with silence to a whole number of loops, so every file fits the loop exactly and a take longer than the
loop is an exact multiple of it. A take stored mono, or stereo with identical channels, is written mono; the rest are stereo.

**The loop.** Built from the store at one log point into a scratch file, not copied from the live loop file, so a mix change
that is still on its way (automix, the mixing screen) is either wholly in or wholly out and the export never clicks. It
shows the mix as already decided, which can be a few seconds ahead of what the speakers play.

`--dry` lists the files it would make.
