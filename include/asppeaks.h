/* asppeaks: the level of a take, a byte a tick (ASP_TICK frames, 0.2 s). the tick k of a take is frames k*ASP_TICK ..
   from the start of the file, whose frame 0 is the start of the take's second of the loop, so a take at second s has
   tick k at loop tick 5*s + k. the byte: 0 = nothing there (digital silence, or before the take began), else
   1 + 2*(db + 120) rounded, db being the rms of the tick against full scale (the louder channel), -120 .. 0 dB in
   half dB steps. */
#ifndef ASPPEAKS_H
#define ASPPEAKS_H
#include <stdint.h>

typedef struct { uint8_t *v; int n, cap; double s[2]; int64_t cnt; int64_t cur; } AspPk;

void   asp_pk_reset( AspPk *p );                        /* a new take; the buffer is kept */
void   asp_pk_free( AspPk *p );
/* pcm: n interleaved stereo int32 frames that start at frame pos of the take. ch: only that channel (0, 1), else the louder */
void   asp_pk_add( AspPk *p, const int32_t *pcm, int64_t n, int64_t pos, int ch );
double asp_pk_db( uint8_t b );                          /* the level of a byte in dB; -200 for 0 */
/* make the levels of every take that has none (reads the take files); *made and *bad count them. 0 when every take has levels */
struct AspStore;
int asp_peaks_backfill( struct AspStore *s, int *made, int *bad );
#endif
