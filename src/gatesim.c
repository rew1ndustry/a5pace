#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "gatesim.h"
#include "gate.h"
#include "asptank.h"

#define HANG 10                       /* as the core: ticks of quiet a take keeps */

void asp_gateparams_default( AspGateParams *p ) { p->gate = 18; p->ring = 7.5; p->lowcut = 30; p->strict = 0; }

int asp_gatesim( const int32_t *pcm, int64_t frames, const AspGateParams *p, AspGateEvent *ev, int max )
  {
  AspGate *g = asp_gate_new( NULL, NULL );
  int32_t cur[2 * ASP_TICK], prev[2 * ASP_TICK];
  int n = 0, track = 0, shh = 0, have_prev = 0;
  int64_t tick, born = 0, gone = 0, start = 0, peak = 0;
  asp_gate_set( g, p->gate, p->ring );
  asp_gate_set_lowcut( g, p->lowcut );
  asp_gate_set_strict( g, p->strict );
  for( tick = 0; ( tick + 1 ) * ASP_TICK <= frames; tick++ )
    {
    int64_t scan;
    memcpy( cur, pcm + 2 * tick * ASP_TICK, sizeof cur );
    asp_gate_switch( g, cur, have_prev ? prev : NULL, ASP_TICK, ! track || born + 1 >= tick );
    scan = asp_gate_scan( g, cur, ASP_TICK );
    if( scan < 0 && gone && gone + 1 == tick ) scan = 0;
    if( scan < ASP_TICK )
      {
      if( ! track ) { track = 1; born = tick; start = tick * ASP_TICK + scan; peak = 0; }
      shh = 0;
      }
    else if( track && ++shh >= HANG ) goto close;
    if( track )
      for( int i = 0; i < 2 * ASP_TICK; i++ ) { int64_t v = llabs( (int64_t)cur[i] ); if( v > peak ) peak = v; }
    goto next;
close:
    {
    double share = asp_gate_event_share( g, 1 );
    if( n < max )
      {
      ev[n].start = start; ev[n].end = ( tick + 1 ) * ASP_TICK; ev[n].share = share;
      ev[n].kept = ! ( p->lowcut > 30 && share < asp_gate_dominance( g ) );
      ev[n].peak_db = peak ? 20 * log10( peak / 2147483648.0 ) : -200;
      }
    n++; track = 0; gone = tick; shh = 0;
    }
next:
    memcpy( prev, cur, sizeof prev );
    have_prev = 1;
    }
  if( track && n < max )                                   /* still open at the end */
    {
    double share = asp_gate_event_share( g, 1 );
    ev[n].start = start; ev[n].end = frames; ev[n].share = share;
    ev[n].kept = ! ( p->lowcut > 30 && share < asp_gate_dominance( g ) );
    ev[n].peak_db = peak ? 20 * log10( peak / 2147483648.0 ) : -200;
    n++;
    }
  asp_gate_free( g );
  return n < max ? n : max;
  }
