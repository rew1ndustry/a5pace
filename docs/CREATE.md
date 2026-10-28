# Making a space, and the metronome

A session ("space") is a directory. Two tools make and fit one. `a5create` makes a new space; `a5click` works on any space, closed or running.

## a5create: make a space
```
a5create DIR NAME --seconds 7.25                      seconds, with decimals (a bare  a5create DIR NAME 7.25  works too)
a5create DIR NAME --sig 4/4 --bpm 100 --bars 4        by music: the loop is that many bars, and a click track is made
a5create DIR NAME --from tune.wav                     an audio file becomes the loop (48 kHz, mono or stereo; wav or flac)
```
options: `--click STYLE` (again for more tracks), `--no-click`, `--pan l|c|r`, `--level N`, `--profile NAME`.
By music the loop is exactly `bars x beats x 60 / bpm` seconds (rounded to a sample) and the default click track
(`beats`) is made. With `--seconds` or `--from` there is no click unless you ask for one, and that needs the music
(`--sig`, `--bpm`, `--bars`) too; or add it afterwards with `a5click`. Other audio formats: convert first (for example
`sox in.mp3 -r 48000 out.wav`); aspace reads wav (16/24/32 bit, float) and flac, and does not use ffmpeg.

## a5click: make click tracks for any space
By default a click is only *created*: kept as a take of its own (iteration -1, -2, ...) and not in the loop. `--insert`
puts it into the loop as well. It works whether or not the space is running: a running one is told (`CLICK ADD`), and
the click joins the loop behind the play head like any other mix change; a closed one is written to. `--bpm` can be left out:
the tempo is then worked out from the loop length and the bars.
```
a5click DIR --sig 4/4 --bpm 100 --bars 4                   the default: accent on the bar, a click on each beat
a5click DIR --sig 3/4 --bpm 90 --bars 8 --style beats --style eighths --style offbeat --pan l
a5click DIR --sig 4/4 --bpm 120 --bars 2 --accent hi.wav --beat lo.wav --sub tick.wav   your own sounds
a5click DIR --sig 4/4 --bars 4 --insert                    guess the tempo from the loop, and play the click
a5click DIR --clear                                         take the generated clicks out of the loop
```
Each `--style` is a take of its own, so a loop can carry several. Styles: `beats` (accent on 1), `bars` (downbeats only),
`eighths`, `triplets`, `sixteenths` (the beat plus quieter subdivisions), `offbeat` (only between the beats).
`--pan l|c|r` puts it left, middle or right; `--level N` is a right shift, 6 dB a step (default 1: -6 dB; 0 is full).

**Fitting a loop.** The bars are spread evenly over the loop, whatever its length: so a loop that is not exactly that many
bars at that tempo gets the tempo that fits, and `a5click` says so (`--strict` refuses instead). Because the clicks
are placed from the loop length, not accumulated, they never drift, and the last click lands the same distance
from the end as the first from the start.

**Signatures and tempo.** `bpm` counts quarter notes a minute. In 6/8, 9/8 and 12/8 a beat is a dotted quarter (so 6/8 has
two beats a bar); in other signatures a beat is the note named by the lower number (7/8 has seven eighth-note beats).

The clicks are generated takes with a negative iteration (-1, -2, ...), kept in the log like everything else, and
a revert can bring a cleared click back.
