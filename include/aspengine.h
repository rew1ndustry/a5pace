/* aspengine: the loop kept from the store. takes are immutable flac; the tank is the sum of
   those in the loop at their levels, so it can always be rebuilt from the store and the log. */
#ifndef ASPENGINE_H
#define ASPENGINE_H
#include "aspstore.h"
#include "asptank.h"

/* where a take sits in the loop: which output channels it feeds (1 or 0) and whether the source channels
   are swapped. a mono take feeds the outputs it is placed on. there are no levels. */
typedef struct { int l, r, flip; } AspPlace;
#define ASP_PLACE_OFF     ((AspPlace){ 0, 0, 0 })
#define ASP_PLACE_STEREO  ((AspPlace){ 1, 1, 0 })

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

/* zero the tank, then put in every take that is in the loop as of log point upto (< 0 = now) */
int asp_tank_clear( AspTank *t );
int asp_rebuild( AspTank *t, AspStore *s, int64_t upto );

/* the take-state fields the engine reads from the json state: status, "out":"lr" (or "l", "r", ""),
   flip, mono, gone, peak, len. asp4 states ("lev":[..]) are read as on or off: their levels are dropped.
   take id 0 is not a take: it holds the loop gain, {"gain_steps":n} (see aspcore.h) */
typedef struct { char status[8]; int l, r, flip, mono; int64_t gone, peak, len; } AspTakeState;   /* l, r: 1 if fed to that output */
void asp_parse_state( const char *json, AspTakeState *st );
#endif
