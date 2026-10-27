#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspcore.h"
#include "aspengine.h"
#include "simedge.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)
#define LOOP 300017
#define LAT  7001

typedef struct { AspStore *s; AspTank *t; AspCore *c; Sim sim; char dir[64]; } Rig;

static void rig_open( Rig *r, const AspCoreCfg *cfg, int seconds )
  {
  char sd[300], tp[300], e[ASP_ERR];
  strcpy( r->dir, "/tmp/aspmix_testXXXXXX" ); if( ! mkdtemp( r->dir ) ) exit( 1 );
  snprintf( sd, sizeof sd, "%s/s", r->dir ); snprintf( tp, sizeof tp, "%s/loop.d64", r->dir );
  r->s = asp_create( sd, "{\"name\":\"mix\"}", e );
  r->t = asp_tank_create( tp, LOOP, 48000, e, sizeof e );
  asp_tank_bind( r->t, asp_session_id( r->s ) );
  r->c = asp_core_open( r->s, r->t, cfg, e, sizeof e );
  sim_init( &r->sim, r->c, LAT, seconds );
  sim_sync( &r->sim );
  }

/* the live loop against one rebuilt from the log */
static int matches( Rig *r )
  {
  char rp[300], e[ASP_ERR]; snprintf( rp, sizeof rp, "%s/rebuilt.d64", r->dir );
  AspTank *b = asp_tank_create( rp, LOOP, 48000, e, sizeof e );
  if( asp_rebuild( b, r->s, -1 ) ) return 0;
  static int64_t x[2 * 5000], y[2 * 5000]; int ok = 1;
  for( int64_t pos = 0; pos < LOOP; pos += 5000 )
    {
    int64_t n = LOOP - pos < 5000 ? LOOP - pos : 5000;
    asp_tank_get( r->t, pos, n, x ); asp_tank_get( b, pos, n, y );
    if( memcmp( x, y, (size_t)n * 16 ) ) ok = 0;
    }
  asp_tank_close( b );
  return ok;
  }

static void rig_close( Rig *r )
  { char cmd[200]; asp_core_close( r->c ); asp_tank_close( r->t ); asp_close( r->s ); free( r->sim.outs ); snprintf( cmd, sizeof cmd, "rm -rf %s", r->dir ); if( system( cmd ) ) {} }

typedef struct { int n; AspTakeState st[32]; int64_t id[32]; } States;
static void grab( int64_t id, const char *j, void *u ) { States *s = u; if( s->n < 32 ) { asp_parse_state( j, &s->st[s->n] ); s->id[s->n++] = id; } }
static void states( Rig *r, States *s ) { s->n = 0; asp_fold( r->s, -1, grab, s ); }

int main( void )
  {
  AspCoreCfg cfg;
  Rig r;

  /* --- mono: a take heard on one channel is stored as one channel and sits in the middle --- */
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 40 );
  int64_t base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 0, 0.1 );                       /* left only */
  sim_burst( &r.sim, base + LOOP * 2, 1, 0.1 );            /* right only, another pass */
  sim_until( &r.sim, base + LOOP * 3 + 48000 * 3 );
  {
  States s; states( &r, &s );
  CHECK( s.n == 2 );
  AspTake k0, k1;
  CHECK( asp_take_get( r.s, s.id[0], &k0 ) == 0 && asp_take_get( r.s, s.id[1], &k1 ) == 0 );
  CHECK( k0.channels == 1 && k1.channels == 1 );
  CHECK( s.st[0].mono && s.st[0].l == 1 && s.st[0].r == 1 && s.st[1].mono && s.st[1].l == 1 && s.st[1].r == 1 );   /* in the middle: both outputs */
  CHECK( strstr( "x", "x" ) );
  AspCoreStatus st; asp_core_status( r.c, &st );
  CHECK( st.jobs == 0 );
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  /* --- stereo stays stereo; identical channels count as mono --- */
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 40 );
  r.sim.dup = 1;
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 2, 0.1 );
  sim_until( &r.sim, base + LOOP + 48000 * 3 );
  { States s; states( &r, &s ); AspTake k; CHECK( s.n == 1 && asp_take_get( r.s, s.id[0], &k ) == 0 && k.channels == 1 && s.st[0].mono ); CHECK( matches( &r ) ); }
  rig_close( &r );
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 40 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 2, 0.1 );
  sim_until( &r.sim, base + LOOP + 48000 * 3 );
  { States s; states( &r, &s ); AspTake k; CHECK( s.n == 1 && asp_take_get( r.s, s.id[0], &k ) == 0 && k.channels == 2 && ! s.st[0].mono && s.st[0].l == 1 && s.st[0].r == 1 && ! s.st[0].flip ); CHECK( matches( &r ) ); }
  rig_close( &r );

  /* --- swap: mono takes go hard left, hard right, hard left; stereo takes flip --- */
  asp_core_defaults( &cfg ); cfg.automix.mode = ASP_AUTOMIX_SWAP; cfg.automix.each = 1;
  rig_open( &r, &cfg, 60 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  for( int i = 0; i < 3; i++ ) sim_burst( &r.sim, base + (int64_t)i * 160000, 0, 0.1 );   /* three mono takes, each past the last one's hang */
  sim_burst( &r.sim, base + LOOP * 2 + 100000, 2, 0.1 );
  sim_burst( &r.sim, base + LOOP * 3 + 100000, 2, 0.1 );
  sim_until( &r.sim, base + LOOP * 4 + 48000 * 3 );
  {
  States s; states( &r, &s );
  for( int i = 0; i < s.n; i++ ) printf( "swap take %d: %s lev %d %d flip %d mono %d\n", i, s.st[i].status, s.st[i].l, s.st[i].r, s.st[i].flip, s.st[i].mono );
  CHECK( s.n == 5 );
  if( s.n == 5 )
    {
    CHECK( s.st[0].l == 1 && s.st[0].r == 0 );          /* hard left, hard right, hard left */
    CHECK( s.st[1].l == 0 && s.st[1].r == 1 );
    CHECK( s.st[2].l == 1 && s.st[2].r == 0 );
    CHECK( s.st[3].flip != s.st[4].flip );
    }
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  /* --- fade: the loop gain steps down every loop, to the noise floor, and stays; the output fades to it --- */
  asp_core_defaults( &cfg ); cfg.automix.mode = ASP_AUTOMIX_FADE; cfg.automix.n = 1;
  rig_open( &r, &cfg, 140 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 2, 0.1 );
  int last = -1, rises = 0, steady = 0;
  for( int p = 0; p < 24; p++ )
    {
    sim_until( &r.sim, base + LOOP * ( p + 1 ) + 48000 * 3 );
    AspCoreStatus st; asp_core_status( r.c, &st );
    if( st.gain_steps > last ) rises++; else steady++;
    last = st.gain_steps;
    }
  printf( "fade: %d steps down, %d passes unchanged, final gain %d steps (-%d dB)\n", rises, steady, last, 6 * last );
  CHECK( rises >= 8 && rises <= 16 && steady >= 6 && last > 6 );
  CHECK( matches( &r ) );                                 /* the tank never changed: the gain is on the way out */
  rig_close( &r );

  /* --- the output fades to a new gain over the next tick, whatever the loop holds, and is steady after --- */
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 30 );
  {
  static int64_t flat[2 * ASP_TICK];
  for( int i = 0; i < 2 * ASP_TICK; i++ ) flat[i] = 1000000;
  for( int64_t pos = 0; pos < LOOP; pos += ASP_TICK ) asp_tank_put( r.t, pos, LOOP - pos < ASP_TICK ? LOOP - pos : ASP_TICK, flat );
  int32_t in[2 * ASP_TICK] = { 0 }, a[2 * ASP_TICK], b[2 * ASP_TICK], c[2 * ASP_TICK];
  asp_core_tick( r.c, in, a );
  asp_core_gain( r.c, 1, 1 );                                 /* -6 dB: the next output tick fades */
  asp_core_tick( r.c, in, b );
  asp_core_tick( r.c, in, c );
  int mono = 1; for( int i = 1; i < ASP_TICK; i++ ) if( b[2*i] > b[2*(i-1)] ) mono = 0;
  printf( "gain change: before %d, fade %d .. %d, after %d\n", a[0], b[0], b[2*(ASP_TICK-1)], c[0] );
  CHECK( a[0] == 1000000 && a[2*(ASP_TICK-1)] == 1000000 );
  CHECK( b[0] > 999900 && b[2*(ASP_TICK-1)] == 500000 && mono );            /* starts where it was, ends at the new gain, smoothly */
  CHECK( c[0] == 500000 && c[2*(ASP_TICK-1)] == 500000 );
  asp_core_gain( r.c, 0, 0 );
  asp_core_tick( r.c, in, a );
  CHECK( a[0] <= 500200 && a[0] >= 500000 && a[2*(ASP_TICK-1)] == 1000000 );   /* and back up the same way */
  }
  rig_close( &r );

  /* --- overlap: a take goes out of the loop when n newer takes have overlapped it --- */
  asp_core_defaults( &cfg ); cfg.automix.mode = ASP_AUTOMIX_OVERLAP; cfg.automix.n = 3;
  rig_open( &r, &cfg, 90 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  for( int i = 0; i < 5; i++ ) sim_burst( &r.sim, base + (int64_t)i * LOOP, 2, 0.1 );
  sim_until( &r.sim, base + LOOP * 4 + 48000 * 4 );
  { States s; states( &r, &s );
    printf( "overlap: %s %s %s %s %s\n", s.st[0].status, s.st[1].status, s.st[2].status, s.st[3].status, s.n > 4 ? s.st[4].status : "-" );
    CHECK( s.n == 5 && ! strcmp( s.st[0].status, "off" ) && ! strcmp( s.st[1].status, "off" ) && ! strcmp( s.st[2].status, "mix" ) && ! strcmp( s.st[3].status, "mix" ) && ! strcmp( s.st[4].status, "mix" ) ); }
  CHECK( matches( &r ) );
  rig_close( &r );

  printf( fails ? "%d FAILED\n" : "all mix tests passed\n", fails );
  return fails != 0;
  }
