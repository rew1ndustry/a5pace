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
#endif
