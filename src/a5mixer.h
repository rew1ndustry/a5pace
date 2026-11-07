/* a5mixer: the little language of the map of a5hell (typed on the map).
     [ITER[.TAKE]] [+N|-N] [/N|\\N] TERMINATOR
   ITER alone is every take that starts in that iteration; ITER.TAKE one take (TAKE is its offset, as in 7.3). the first
   dot after ITER is always the separator, so 123. waits for a take number or a terminator: 123.. is the whole iteration.
   +N / -N is the gain in bits (+N shifts every sample left N bits, louder; -N right, quieter), /N the pan N bits right (the left side shifted N more), \\N N bits left: all absolute. a sign or slash with no number keeps the
   take's current value (and so gives the take a mix of its own). the terminator is what happens:
     .  queue it for the take's next start (with nothing else typed, mute <-> play)      !  the same, at once
     m  mute   p  play at the take's mix, queued       M  P  the same, at once
   without an ITER it is the take under the cursor, or what is selected. */
#ifndef A5MIXER_H
#define A5MIXER_H
#include <stddef.h>
typedef struct { int has_iter; long iter; int has_take; long take; int set_gain, gsh, set_pan, pan, act, now; } MixCmd;
enum { MP_WAIT = 0, MP_DONE = 1, MP_BAD = -1 };
/* 0 more is needed, 1 complete, -1 not a command. set_gain / set_pan: 1 a value, 2 keep the current one. act: 0 none, 1 mute, 2 play, 3 toggle */
int  a5mix_parse( const char *s, MixCmd *c );
/* the control line for one take (off < 0: every take that starts in the iteration); returns its length */
int  a5mix_line( char *b, size_t n, long iter, long off, const MixCmd *c );
#endif
