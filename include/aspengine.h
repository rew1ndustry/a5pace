/* aspengine: the loop kept from the store. takes are immutable flac; the tank is the sum of
   those in the loop at their levels, so it can always be rebuilt from the store and the log. */
#ifndef ASPENGINE_H
#define ASPENGINE_H
#include "aspstore.h"
#include "asptank.h"

/* where a take sits in the loop: an output level per channel (a right shift, or ASP_LEV_OFF) and
   whether the source channels are swapped. a mono take feeds both outputs. */
typedef struct { int l, r, flip; } AspPlace;
#define ASP_PLACE_OFF  ((AspPlace){ ASP_LEV_OFF, ASP_LEV_OFF, 0 })

/* a move: take the old placement of some data out of the loop and put the new one in, as one exact
   add-the-difference. old and new may be different data (a stereo take converted to mono). a job does
   it a slice at a time so the tick loop is never held; asp_move_take runs one to the end. */
typedef struct AspJob AspJob;
AspJob *asp_job_new( int64_t pos0, int64_t lo, int64_t hi,
                     int32_t *oldpcm, int64_t oldframes, int oldch, AspPlace from,
                     int32_t *newpcm, int64_t newframes, int newch, AspPlace to );   /* takes the buffers */
AspJob *asp_job_take( AspStore *s, int64_t take_id, int64_t lo, int64_t hi, AspPlace from, AspPlace to );
int     asp_job_step( AspJob *j, AspTank *t, int64_t budget_frames );    /* 1 done, 0 more, -1 error */
int64_t asp_job_span( const AspJob *j );                                /* frames it will touch, from pos0 */
int64_t asp_job_pos0( const AspJob *j );
void    asp_job_free( AspJob *j );
int     asp_move_take( AspTank *t, AspStore *s, int64_t take_id, int64_t lo, int64_t hi, AspPlace from, AspPlace to );

/* a take's frames as interleaved int32 (malloc'd, caller frees), and its peak */
int  asp_pcm_load( const char *flac, int32_t **pcm, int64_t *frames, int *channels );
int  asp_take_peak( AspStore *s, int64_t take_id, int64_t *peak );     /* largest |sample| over all channels */

/* the older call: same data, no swap */
int asp_apply_take( AspTank *t, AspStore *s, int64_t take_id, int64_t lo, int64_t hi,
                    int ol, int orr, int nl, int nr );

/* zero the tank, then put in every take that is in the loop as of log point upto (< 0 = now) */
int asp_tank_clear( AspTank *t );
int asp_rebuild( AspTank *t, AspStore *s, int64_t upto );

/* the take-state fields the engine reads from the json state */
typedef struct { char status[8]; int l, r, flip, mono; int64_t gone, peak, len; } AspTakeState;
void asp_parse_state( const char *json, AspTakeState *st );
#endif
