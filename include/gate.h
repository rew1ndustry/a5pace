/* gate: when does sound start. see src/gate.c */
#ifndef ASPGATE_H
#define ASPGATE_H
#include <stdint.h>
typedef struct Gate AspGate;
/* say gets log lines ("GATE learnt ...", "SWITCH 1 muted") */
AspGate *asp_gate_new( void (*say)( void *user, const char *line ), void *user );
void asp_gate_free( AspGate *g );
void asp_gate_reset( AspGate *g );                       /* learn the floor again */
void asp_gate_set( AspGate *g, double gate_db, double ring_db );   /* <= 0 leaves a value alone */
double asp_gate_margin( AspGate *g );
double asp_gate_ring( AspGate *g );
/* feed one tick (count frames, stereo): offset of the earliest sound found in it, which may be
   negative (kept from before), or ASP_TICK when there is none */
int64_t asp_gate_scan( AspGate *g, int32_t *pcm, int count );
/* dc-step muting; prev is the previous tick or NULL; armed while the gate waits */
void asp_gate_switch( AspGate *g, int32_t *pcm, int32_t *prev, int count, int armed );
/* per channel: is it ringing/held open now; has the floor been learnt; the floor as an rms amplitude */
int asp_gate_active( AspGate *g, int ch );
int asp_gate_learnt( AspGate *g );
double asp_gate_floor( AspGate *g, int ch );
int asp_gate_why( AspGate *g );     /* rules that fired since last asked: 1 click, 2 ring, 4 two bands, 8 a band twice */
void asp_gate_set_lowcut( AspGate *g, double hz );   /* the instrument's lowest note: nothing below may open the gate (30 = as asp4) */
double asp_gate_lowcut( AspGate *g );
void asp_gate_set_strict( AspGate *g, int on );     /* 1: a single band over its threshold twice running no longer opens the gate */
int asp_gate_strict( AspGate *g );
double asp_gate_event_share( AspGate *g, int reset );   /* share of the open-gate power above the lowcut; reset starts a new count */
double asp_gate_dominance( AspGate *g );               /* the share below which an event is a bump (0.15) */
void asp_gate_set_channels( AspGate *g, int mask );   /* which input channels it listens to: 1 left, 2 right, 3 both (the default) */
int  asp_gate_channels( AspGate *g );
#endif
