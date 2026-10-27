/* a simulated sound card for tests and headless runs: the loop's output comes back in only while syncing (the shell's
   loopback), and bursts of a tone are played in by a "player" at chosen input times and channels. */
#include <math.h>
#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include "aspcore.h"

#define SIM_BLEN 19200
typedef struct { int64_t at; int ch; double amp; int on; } SimBurst;     /* ch: 0 left, 1 right, 2 both */
typedef struct
  {
  AspCore *core; int64_t tickn, lat, cap; int32_t *outs; unsigned rs; double noise; int dup;   /* outs: ring of cap frames */
  SimBurst b[16]; int32_t tone[SIM_BLEN];
  } Sim;

static double sim_rnd( Sim *s ) { s->rs = s->rs * 1664525u + 1013904223u; return ( (int)( s->rs >> 8 ) & 0xffff ) / 32768.0 - 1; }

static void sim_init( Sim *s, AspCore *c, int64_t lat, int64_t seconds )
  {
  memset( s, 0, sizeof *s );
  s->core = c; s->lat = lat; s->cap = ( seconds > lat / 48000 + 2 ? seconds : lat / 48000 + 2 ) * 48000LL; s->rs = 99; s->noise = 21474;
  s->outs = calloc( (size_t)s->cap * 2, 4 );
  for( int i = 0; i < SIM_BLEN; i++ )
    s->tone[i] = (int32_t)( 2147483648.0 * sin( 2 * M_PI * 440 * i / 48000.0 ) * ( 0.5 - 0.5 * cos( 2 * M_PI * i / SIM_BLEN ) ) );
  }

static void sim_burst( Sim *s, int64_t at, int ch, double amp )   /* at: input frame */
  { for( int i = 0; i < 16; i++ ) if( ! s->b[i].on ) { s->b[i].at = at; s->b[i].ch = ch; s->b[i].amp = amp; s->b[i].on = 1; return; } }

static void sim_step( Sim *s )
  {
  for( int b = 0; b < 16; b++ ) if( s->b[b].on && s->tickn * ASP_TICK > s->b[b].at + SIM_BLEN ) s->b[b].on = 0;
  AspCoreStatus ps; asp_core_status( s->core, &ps );
  int looped = ps.phase == 0;
  int32_t in[2 * ASP_TICK], out[2 * ASP_TICK];
  for( int i = 0; i < ASP_TICK; i++ )
    {
    int64_t t = s->tickn * ASP_TICK + i, d = t - s->lat;
    (void)0;
    double n0 = sim_rnd( s ) * s->noise, n1 = s->dup ? n0 : sim_rnd( s ) * s->noise;
    for( int ch = 0; ch < 2; ch++ )
      {
      double v = looped && d >= 0 ? (double)s->outs[2*( d % s->cap )+ch] : 0;
      v += ch ? n1 : n0;
      for( int b = 0; b < 16; b++ )
        if( s->b[b].on && t >= s->b[b].at && t < s->b[b].at + SIM_BLEN && ( s->b[b].ch == 2 || s->b[b].ch == ch ) )
          v += s->tone[t - s->b[b].at] * s->b[b].amp;
      in[2*i+ch] = (int32_t)v;
      }
    }
  asp_core_tick( s->core, in, out );
  memcpy( s->outs + 2 * ( ( s->tickn * ASP_TICK ) % s->cap ), out, sizeof out );
  s->tickn++;
  }

static void sim_sync( Sim *s )
  { AspCoreStatus st; int g = 0; do { sim_step( s ); asp_core_status( s->core, &st ); } while( st.phase == 0 && ++g < 1000 ); }
static void sim_until( Sim *s, int64_t frame ) { while( s->tickn * ASP_TICK < frame ) sim_step( s ); }
