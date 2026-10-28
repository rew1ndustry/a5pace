/* aspengine: the loop kept from the store. takes are immutable flac; the tank is the sum of
   those in the loop at their levels, so it can always be rebuilt from the store and the log. */
#ifndef ASPENGINE_H
#define ASPENGINE_H
#include "aspstore.h"
#include "asptank.h"

/* where a take sits in the loop: an output level per channel (a right shift, or ASP_LEV_OFF) and
   whether the source channels are swapped. a mono take feeds both outputs. */
typedef struct { int l, r, flip, gdb, pan, mixed; } AspPlace;
#define ASP_PLACE_OFF  ((AspPlace){ ASP_LEV_OFF, ASP_LEV_OFF, 0, 0, 0, 0 })

/* an explicit mix (mixed != 0) replaces l, r and flip: gain gdb whole dB (absolute), pan -100 (left) .. 100 (right).
   a mono take is panned across both outputs with the centre at half each (the same as level shift 1); a stereo take
   keeps both channels: its pan is stored but has no effect yet. integer arithmetic, so a take taken out
   subtracts exactly what was added. */
#define ASP_GDB_MIN (-60)
#define ASP_GDB_MAX 24
int     asp_place_eq( AspPlace a, AspPlace b );
void    asp_place_mult( AspPlace p, int mono, int64_t *ml, int64_t *mr );    /* Q24 gains for the two outputs */
int64_t asp_gain( int64_t x, int64_t m );
void    asp_place_to_mix( AspPlace p, int mono, int *gdb, int *pan );        /* what an unmixed placement amounts to */

/* a move: take the old placement of some data out of the loop and put the new one in, as one exact
   add-the-difference. old and new may be different data (a stereo take converted to mono). a job does
   it a slice at a time so the tick loop is never held; asp_move_take runs one to the end. */
typedef struct AspJob AspJob;
AspJob *asp_job_new( int64_t pos0, int64_t lo, int64_t hi,
                     int32_t *oldpcm, int64_t oldframes, int oldch, AspPlace from,
                     int32_t *newpcm, int64_t newframes, int newch, AspPlace to );   /* takes the buffers */
/* a take through a view: src is the one channel of it that is heard (mono), or -1 for the channels as stored; each side has its own */
AspJob *asp_job_take( AspStore *s, int64_t take_id, int64_t lo, int64_t hi, AspPlace from, int from_src, AspPlace to, int to_src );
int     asp_job_step( AspJob *j, AspTank *t, int64_t budget_frames );    /* 1 done, 0 more, -1 error */
/* a job goes along the loop from where the take starts, one position at a time, and each position gets everything
   the take puts there (a take longer than the loop lies over itself) in one write; so a position is never half
   changed. limit: how far along (in frames from the start of the take) it may go; asp_job_step has none. */
int     asp_job_step_to( AspJob *j, AspTank *t, int64_t budget_frames, int64_t limit );
int64_t asp_job_cursor( const AspJob *j );
int     asp_job_retarget( AspJob *j, AspPlace to );                     /* before it has started: 1 if it is now nothing to do */
int64_t asp_job_span( const AspJob *j );                                /* frames it will touch, from pos0 */
int64_t asp_job_pos0( const AspJob *j );
void    asp_job_free( AspJob *j );
int     asp_move_take( AspTank *t, AspStore *s, int64_t take_id, int src, int64_t lo, int64_t hi, AspPlace from, AspPlace to );

/* a take's frames as interleaved int32 (malloc'd, caller frees), and its peak */
int  asp_pcm_load( const char *flac, int32_t **pcm, int64_t *frames, int *channels );
int  asp_take_peak( AspStore *s, int64_t take_id, int src, int64_t *peak );     /* largest |sample|: of channel src, or of all (-1) */

/* the older call: same data, no swap */
int asp_apply_take( AspTank *t, AspStore *s, int64_t take_id, int64_t lo, int64_t hi,
                    int ol, int orr, int nl, int nr );

/* zero the tank, then put in every take that is in the loop as of log point upto (< 0 = now) */
int asp_tank_clear( AspTank *t );
int asp_rebuild( AspTank *t, AspStore *s, int64_t upto );

/* the take-state fields the engine reads from the json state */
typedef struct { char status[8]; int l, r, flip, mono, src; int64_t gone, peak, len; int gdb, pan, mixed; } AspTakeState;
void asp_parse_state( const char *json, AspTakeState *st );
#endif
