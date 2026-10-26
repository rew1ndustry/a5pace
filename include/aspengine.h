/* aspengine: the loop kept from the store. takes are immutable flac; the tank is the sum of
   those in the loop at their levels, so it can always be rebuilt from the store and the log. */
#ifndef ASPENGINE_H
#define ASPENGINE_H
#include "aspstore.h"
#include "asptank.h"

/* move a take (or frames [lo,hi) of it, hi < 0 = to its end) from levels ol/or to nl/nr in the
   tank. it sits at take.off seconds into the loop, wrapping. insert = from OFF, remove = to OFF. */
int asp_apply_take( AspTank *t, AspStore *s, int64_t take_id, int64_t lo, int64_t hi,
                    int ol, int orr, int nl, int nr );

/* zero the tank, then put in every take that is in the loop as of log point upto (< 0 = now) */
int asp_tank_clear( AspTank *t );
int asp_rebuild( AspTank *t, AspStore *s, int64_t upto );

/* the take-state fields the engine reads from the json state (status, lev, gone) */
typedef struct { char status[8]; int l, r; int64_t gone; } AspTakeState;
void asp_parse_state( const char *json, AspTakeState *st );
#endif
