#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspcore.h"
#include "aspengine.h"
#include "simedge.h"
#include "sqlite3.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)
#define LOOP 300017
#define LAT  7001

static int count_type( const char *dir, const char *type )
  {
  char p[400]; sqlite3 *db; sqlite3_stmt *q; int n = -1;
  snprintf( p, sizeof p, "%s/s/session.db", dir );
  sqlite3_open_v2( p, &db, SQLITE_OPEN_READONLY, NULL );
  sqlite3_prepare_v2( db, "select count(*) from log where type=?", -1, &q, NULL );
  sqlite3_bind_text( q, 1, type, -1, SQLITE_STATIC );
  if( sqlite3_step( q ) == SQLITE_ROW ) n = sqlite3_column_int( q, 0 );
  sqlite3_finalize( q ); sqlite3_close( db );
  return n;
  }

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
    if( memcmp( x, y, (size_t)n * 16 ) ) { if( getenv( "DBG" ) ) { int64_t i; for( i = 0; i < 2 * n && x[i] == y[i]; i++ ) {} { int64_t cnt = 0, lastd = 0; for( int64_t j = 0; j < 2 * n; j++ ) if( x[j] != y[j] ) { cnt++; lastd = pos + j / 2; } printf( "first difference at frame %lld: live %lld rebuilt %lld; %lld differ in this block, last at %lld\n", (long long)( pos + i / 2 ), (long long)x[i], (long long)y[i], (long long)cnt, (long long)lastd ); } } ok = 0; }
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
  CHECK( s.st[0].mono && s.st[0].l == 1 && s.st[0].r == 1 && s.st[1].mono && s.st[1].l == 1 );
  CHECK( strstr( "x", "x" ) );
  AspCoreStatus st; asp_core_status( r.c, &st );
  CHECK( st.jobs == 0 );
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  /* --- told the instrument's lowest note, a loud bump is not kept and a note is --- */
  asp_core_defaults( &cfg ); cfg.lowcut = 190; cfg.strict = 1;
  rig_open( &r, &cfg, 60 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst_hz( &r.sim, base, 2, 0.004, 6000 );                    /* a small high edge opens the gate, as a real bump's does ... */
  sim_burst_hz( &r.sim, base + 12000, 2, 0.03, 80 );               /* ... then the thump itself, -30 dBFS */
  sim_burst_hz( &r.sim, base + 2 * LOOP, 2, 0.1, 440 );            /* a note, a pass and a half later */
  sim_until( &r.sim, base + 3 * LOOP + 48000 * 3 );
  { States s; states( &r, &s );
    printf( "bump test: %d take(s) stored, %d discarded\n", s.n, count_type( r.dir, "discarded" ) );
    CHECK( s.n == 1 && count_type( r.dir, "discarded" ) == 1 && count_type( r.dir, "new" ) == 2 );
    CHECK( matches( &r ) ); }                                      /* and the thump was taken out of the loop again */
  rig_close( &r );

  /* --- minpeak: a take that never reaches the quietest level worth keeping is not kept --- */
  asp_core_defaults( &cfg ); cfg.minpeak = -45;
  rig_open( &r, &cfg, 60 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst_hz( &r.sim, base, 2, 0.003, 440 );                     /* -50 dBFS: opens the gate, too quiet to keep */
  sim_burst_hz( &r.sim, base + 2 * LOOP, 2, 0.05, 440 );           /* -26 dBFS: kept */
  sim_until( &r.sim, base + 3 * LOOP + 48000 * 3 );
  { States s; states( &r, &s );
    printf( "minpeak test: %d take(s) stored, %d discarded\n", s.n, count_type( r.dir, "discarded" ) );
    CHECK( s.n == 1 && count_type( r.dir, "discarded" ) == 1 && count_type( r.dir, "new" ) == 2 );
    CHECK( matches( &r ) ); }
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
  { States s; states( &r, &s ); AspTake k; CHECK( s.n == 1 && asp_take_get( r.s, s.id[0], &k ) == 0 && k.channels == 2 && ! s.st[0].mono && s.st[0].l == 0 && s.st[0].r == 0 ); CHECK( matches( &r ) ); }
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
    CHECK( s.st[0].l == 0 && s.st[0].r == ASP_LEV_OFF );
    CHECK( s.st[1].l == ASP_LEV_OFF && s.st[1].r == 0 );
    CHECK( s.st[2].l == 0 && s.st[2].r == ASP_LEV_OFF );
    CHECK( s.st[3].flip != s.st[4].flip );
    }
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  /* --- the output fades when it has to come down further to fit, and is exact when steady --- */
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 30 );
  {
  static int64_t loud[2 * ASP_TICK];
  for( int i = 0; i < ASP_TICK; i++ ) { loud[2*i] = loud[2*i+1] = ( i & 1 ) ? -1200000000LL : 1200000000LL; }   /* peak to peak 2.4e9: needs 2 bits down */
  for( int64_t pos = 0; pos < LOOP; pos += ASP_TICK ) asp_tank_put( r.t, pos, LOOP - pos < ASP_TICK ? LOOP - pos : ASP_TICK, loud );
  int32_t in[2 * ASP_TICK] = { 0 }, a[2 * ASP_TICK], b[2 * ASP_TICK], c[2 * ASP_TICK];
  asp_core_tick( r.c, in, a );                                /* the first: nothing known about the peak yet */
  asp_core_tick( r.c, in, b );                                /* the shift has to change: this one fades */
  asp_core_tick( r.c, in, c );                                /* steady at the new shift */
  double ma = 0, mb0 = 0, mb1 = 0, mc = 0;
  for( int i = 0; i < 100; i++ ) { ma += llabs( a[2*i] ); mb0 += llabs( b[2*i] ); mb1 += llabs( b[2*(ASP_TICK-1-i)] ); mc += llabs( c[2*i] ); }
  printf( "output shift: first tick %.3g, fading tick %.3g .. %.3g, after %.3g\n", ma / 100, mb0 / 100, mb1 / 100, mc / 100 );
  CHECK( ma / 100 > 1.1e9 && mb0 / 100 > 1.1e9 * 0.99 && mb1 / 100 < 3.2e8 && mc / 100 > 2.9e8 && mc / 100 < 3.1e8 );
  int smooth = 1; for( int i = 2; i < ASP_TICK - 2; i++ ) if( llabs( llabs( (int64_t)b[2*i] ) - llabs( (int64_t)b[2*(i-2)] ) ) > 400000 ) smooth = 0;
  CHECK( smooth );                                            /* no step anywhere inside the fading tick */
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
