/* asptank: the loop. a .d64 file is a 128 byte header then N frames of two s64 (L then R),
   all little endian:
     0   "ASP5TANK"          magic
     8   u32 version (1)    12  u32 header size (128)
     16  u64 frames         24  u32 rate   28 u32 channels (2)
     32  u64 iteration      40  u64 peak (peak to peak monitor)
     48  u64 log seq        the log point this loop reflects
     56  u32 flags (1 = open/dirty)   60 u32 reserved
     64  32 bytes           session id (sha-256, binary); zero if not bound
     96  32 bytes           reserved, zero
   an asp4 file (u64 iteration, u64 peak, then frames) is opened as legacy, header 16.
   the tank is a derived cache: asp_rebuild (aspengine.h) regenerates it from the store. */
#ifndef ASPTANK_H
#define ASPTANK_H
#include <stdint.h>

#define ASP_TICK   9600           /* frames in a tick: 1/5 s at 48 kHz */

typedef struct AspTank AspTank;
AspTank *asp_tank_create( const char *path, int64_t frames, int rate, char *err, int errn );
AspTank *asp_tank_open( const char *path, char *err, int errn );
void     asp_tank_close( AspTank *t );
int64_t  asp_tank_frames( AspTank *t );
uint64_t asp_tank_iter( AspTank *t );
uint64_t asp_tank_peak( AspTank *t );
int      asp_tank_set_head( AspTank *t, uint64_t iter, uint64_t peak );
int      asp_tank_legacy( AspTank *t );                      /* asp4 header: no seq, flags or session */
int      asp_tank_rate( AspTank *t );
uint64_t asp_tank_seq( AspTank *t );
int      asp_tank_set_seq( AspTank *t, uint64_t seq );
int      asp_tank_bind( AspTank *t, const char *session_hex );      /* 64 hex chars */
int      asp_tank_bound( AspTank *t, const char *session_hex );     /* 1 if bound to it (or unbound) */
int      asp_tank_dirty( AspTank *t );                       /* was not closed cleanly */
int      asp_tank_set_dirty( AspTank *t, int dirty );

/* n frames at pos, wrapping round the loop. mix has 2n int64 */
int  asp_tank_get( AspTank *t, int64_t pos, int64_t n, int64_t *mix );
int  asp_tank_put( AspTank *t, int64_t pos, int64_t n, const int64_t *mix );

/* the tank is a plain sum: a take goes in by being added, comes out by being subtracted (engine.h).
   there are no per-take levels; the loop as a whole has a gain, applied on the way out (aspcore.h) */
int  asp_tank_add( AspTank *t, int64_t pos, const int32_t *pcm, int64_t n );

/* one tick of the loop to output pcm; returns the new peak (peak to peak) */
uint64_t asp_mix_out( uint64_t peak, int32_t *pcm, int64_t n, const int64_t *mix );
#endif
