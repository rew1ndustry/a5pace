/* asptank: the loop. a .d64 file is
     u64 iteration, u64 peak              (little endian)
     N frames of two s64 samples, L then R
   the sum of every take in the loop, at its level. this matches asp4's .d64 byte for byte.
   a derived cache: asp_rebuild (aspengine.h) can regenerate it from the store. */
#ifndef ASPTANK_H
#define ASPTANK_H
#include <stdint.h>

#define ASP_TICK   9600           /* frames in a tick: 1/5 s at 48 kHz */
#define ASP_LEV_OFF 99            /* the channel is not in the loop */
#define ASP_LEV_MIN (-8)
#define ASP_LEV_MAX 31

typedef struct AspTank AspTank;
AspTank *asp_tank_create( const char *path, int64_t frames, char *err, int errn );
AspTank *asp_tank_open( const char *path, char *err, int errn );
void     asp_tank_close( AspTank *t );
int64_t  asp_tank_frames( AspTank *t );
uint64_t asp_tank_iter( AspTank *t );
uint64_t asp_tank_peak( AspTank *t );
int      asp_tank_set_head( AspTank *t, uint64_t iter, uint64_t peak );

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
uint64_t asp_mix_out( uint64_t peak, int32_t *pcm, int64_t n, const int64_t *mix, int64_t gain );
#endif
