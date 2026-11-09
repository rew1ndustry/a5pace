/* asptank: the loop. a .d64 file is chips back to back, one per tick (see aspchip.h for the 128 byte chip header:
   magic, iteration, pass, seq, peak, output gain, session, crcs). each chip is ASP_TICK frames of two s64 (L then R),
   all little endian; the last chip has what is left. there is no file header: every chip is complete in itself.
   the tank is a derived cache: asp_rebuild (aspengine.h) regenerates it from the store. */
#ifndef ASPTANK_H
#define ASPTANK_H
#include <stdint.h>
#include "aspchip.h"

#define ASP_TICK   9600           /* frames in a tick: 1/5 s at 48 kHz */
#define ASP_LEV_OFF 99            /* the channel is not in the loop */
#define ASP_LEV_MIN (-8)
#define ASP_LEV_MAX 31

typedef struct AspTank AspTank;
AspTank *asp_tank_create( const char *path, int64_t frames, int rate, char *err, int errn );
AspTank *asp_tank_open( const char *path, char *err, int errn );
void     asp_tank_close( AspTank *t );
int64_t  asp_tank_frames( AspTank *t );
uint64_t asp_tank_iter( AspTank *t );
uint64_t asp_tank_peak( AspTank *t );
int      asp_tank_set_head( AspTank *t, uint64_t iter, uint64_t peak );
int      asp_tank_rate( AspTank *t );
uint64_t asp_tank_seq( AspTank *t );
int      asp_tank_set_seq( AspTank *t, uint64_t seq );
int      asp_tank_bind( AspTank *t, const char *session_hex );      /* 64 hex chars */
int      asp_tank_bound( AspTank *t, const char *session_hex );     /* 1 if bound to it (or unbound) */
uint32_t asp_tank_chips( AspTank *t );
int      asp_tank_chip_frames( AspTank *t, uint32_t k );

/* the stream: chip k read or written whole, by handles of their own, so the tick reads chip k+1 and writes chip k with
   no seek (but the wrap from the last chip to the first). read: 0 good, -1 io, -2 bad header, -3 frames fail their crc.
   write: the caller sets iter, pass, seq, peak, out_shift, flags in h; the rest is filled in. mix has 2 * frames int64. */
int  asp_tank_chip_read( AspTank *t, uint32_t k, AspChipHead *h, int64_t *mix );
int  asp_tank_chip_write( AspTank *t, uint32_t k, const AspChipHead *h, const int64_t *mix );

/* n frames at pos, wrapping round the loop. mix has 2n int64 */
int  asp_tank_get( AspTank *t, int64_t pos, int64_t n, int64_t *mix );
int  asp_tank_put( AspTank *t, int64_t pos, int64_t n, const int64_t *mix );

/* a take's level is a right shift (0 as recorded, 1 half, -1 double) or ASP_LEV_OFF.
   moving a take from one level to another adds the difference, so insert, remove and a
   change of level are one operation, and exact. */
int64_t asp_level( int64_t x, int lev );
void    asp_mix_level( const int32_t *pcm, int64_t n, int64_t *mix, int ol, int orr, int nl, int nr );
int     asp_tank_level( AspTank *t, int64_t pos, const int32_t *pcm, int64_t n, int ol, int orr, int nl, int nr );

/* one tick of the loop to output pcm; returns the new peak (peak to peak) */
/* shift: how far the last tick was brought down to fit (kept between calls). when it changes, this tick fades from
   the old scale to the new one, so the change is never a step. */
uint64_t asp_mix_out( uint64_t peak, int32_t *pcm, int64_t n, const int64_t *mix, int *shift );
#endif
