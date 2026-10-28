#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sqlite3.h"
#include "aspcore.h"
#include "aspengine.h"
#include "flacio.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

#define LOOP   300017             /* an odd length, 6.25 s */
#define LAT    7001               /* the simulated round trip */
#define MAXT   (48000 * 70)       /* frames of history kept */
#define AMP    0.1
#define BLEN   19200              /* a burst: 0.4 s */

static int32_t *outs;             /* everything played, stereo */
static int64_t tickn;             /* ticks run */
static unsigned rs = 99;
static double rnd( void ) { rs = rs * 1664525u + 1013904223u; return ( (int)( rs >> 8 ) & 0xffff ) / 32768.0 - 1; }

typedef struct { int64_t at; int on; } Burst;
static Burst bursts[4];
static int32_t burst[BLEN];
static void mkburst( void )
  { for( int i = 0; i < BLEN; i++ ) burst[i] = (int32_t)( 2147483648.0 * AMP * sin( 2 * M_PI * 440 * i / 48000.0 ) * ( 0.5 - 0.5 * cos( 2 * M_PI * i / BLEN ) ) ); }

static void step( AspCore *c )
  {
  AspCoreStatus ps; asp_core_status( c, &ps );       /* the shell's mixer: the loopback is on only while syncing */
  int looped = ps.phase == 0;
  int32_t in[2 * ASP_TICK], out[2 * ASP_TICK];
  for( int i = 0; i < ASP_TICK; i++ )
    {
    int64_t t = tickn * ASP_TICK + i, s = t - LAT;
    for( int ch = 0; ch < 2; ch++ )
      {
      double v = looped && s >= 0 ? (double)outs[2*s+ch] : 0;
      v += rnd() * 21474;
      for( int b = 0; b < 4; b++ )
        if( bursts[b].on && t >= bursts[b].at && t < bursts[b].at + BLEN ) v += burst[t - bursts[b].at];
      in[2*i+ch] = (int32_t)v;
      }
    }
  asp_core_tick( c, in, out );
  memcpy( outs + 2 * tickn * ASP_TICK, out, sizeof out );
  tickn++;
  }

static int count( const char *dir, const char *type )
  {
  char p[400]; sqlite3 *db; sqlite3_stmt *q; int n = -1;
  snprintf( p, sizeof p, "%s/session.db", dir );
  sqlite3_open_v2( p, &db, SQLITE_OPEN_READONLY, NULL );
  sqlite3_prepare_v2( db, "select count(*) from log where type=?", -1, &q, NULL );
  sqlite3_bind_text( q, 1, type, -1, SQLITE_STATIC );
  if( sqlite3_step( q ) == SQLITE_ROW ) n = sqlite3_column_int( q, 0 );
  sqlite3_finalize( q ); sqlite3_close( db );
  return n;
  }

typedef struct { int32_t *p; int64_t n, cap; } Buf;
static int collect( const int32_t *pcm, unsigned frames, int ch, void *u )
  { Buf *b = u; (void)ch; if( b->n + frames > b->cap ) return -1; memcpy( b->p + 2 * b->n, pcm, 8 * (size_t)frames ); b->n += frames; return 0; }

int main( void )
  {
  char dir[] = "/tmp/aspcore_testXXXXXX", sd[300], tp[300], e[ASP_ERR], cmd[600];
  if( ! mkdtemp( dir ) ) return 1;
  { char x[300]; snprintf( x, sizeof x, "%s/cfg", dir ); setenv( "XDG_CONFIG_HOME", x, 1 ); }
  snprintf( sd, sizeof sd, "%s/s", dir ); snprintf( tp, sizeof tp, "%s/loop.d64", dir );
  AspStore *s = asp_create( sd, "{\"name\":\"core\"}", e );
  AspTank *t = asp_tank_create( tp, LOOP, 48000, e, sizeof e );
  asp_tank_bind( t, asp_session_id( s ) );
  outs = calloc( (size_t)MAXT * 2, 4 );
  mkburst();
  AspCoreCfg cfg; asp_core_defaults( &cfg );
  AspCore *c = asp_core_open( s, t, &cfg, e, sizeof e );
  CHECK( c );

  /* 1. sync */
  AspCoreStatus st;
  int guard = 0;
  do { step( c ); asp_core_status( c, &st ); } while( st.phase == 0 && ++guard < 1000 );
  CHECK( st.phase == 1 && st.latency == LAT );
  printf( "synced after %lld ticks, latency %lld, loop starts at tick %lld\n", (long long)tickn, (long long)st.latency, (long long)st.origin );
  int64_t origin = st.origin;
  CHECK( origin == tickn );                                  /* the loop begins on the tick after sync */

  /* 2. play: a burst, not on a tick boundary, then another a pass and a half later */
  int64_t T0 = ( origin + 120 ) * ASP_TICK + 1234;
  int64_t T1 = T0 + LOOP + LOOP / 2;
  bursts[0].at = T0; bursts[0].on = 1;
  bursts[1].at = T1; bursts[1].on = 1;
  while( tickn * ASP_TICK < T1 + BLEN + 48000 * 3 ) step( c );
  asp_core_status( c, &st );
  CHECK( ! st.take_open );
  if( getenv( "DUMP" ) ) { char q[400]; snprintf( q, sizeof q, "sqlite3 %s/session.db \"select seq,tick,type,args_json from log where type not in ('take_state','take_added','top','wrap')\"", sd ); if( system( q ) ) {} }
  CHECK( count( sd, "new" ) == 2 && count( sd, "end" ) == 2 );
  CHECK( count( sd, "sync_found" ) == 1 && count( sd, "top" ) >= 3 );

  /* the first take holds the burst at the right place in the loop, sample for sample */
  int64_t P0 = ( T0 - origin * ASP_TICK ) % LOOP, sec0 = P0 / 48000;
  AspTake k;
  int found = 0;
  for( int64_t it = 1; it < 6 && ! found; it++ ) if( asp_take_find( s, it, sec0, &k ) == 0 ) found = 1;
  CHECK( found );
  if( found )
    {
    char path[1200]; Buf b = { malloc( 8 * 48000 * 40 ), 0, 48000 * 40 };
    asp_take_path( s, &k, path, sizeof path );
    CHECK( asp_take_frames( path, -1, collect, &b ) == 0 );
    int64_t at = P0 - sec0 * 48000, bad = 0;
    for( int j = 0; j < BLEN && at + j < b.n; j++ )
      for( int ch = 0; ch < 2; ch++ )
        if( abs( b.p[2 * ( at + j ) + ch] - burst[j] ) > 70000 ) bad++;   /* the floor noise rides on it */
    CHECK( bad == 0 );
    CHECK( b.n > at + BLEN );
    free( b.p );
    }

  /* it comes back out one pass later, ahead of the recording by the round trip */
  {
  int64_t Tf = T0 - LAT + LOOP, bad = 0;
  for( int j = 0; j < BLEN; j++ )
    for( int ch = 0; ch < 2; ch++ )
      if( abs( outs[2 * ( Tf + j ) + ch] - burst[j] ) > 200000 ) bad++;
  CHECK( bad == 0 );
  if( bad ) printf( "%lld output samples differ from the burst\n", (long long)bad );
  }

  /* the live loop equals one rebuilt from the store */
  {
  AspTank *r = asp_tank_create( dir, 0, 48000, e, sizeof e );   /* fails: a directory */
  CHECK( r == NULL );
  char rp[300]; snprintf( rp, sizeof rp, "%s/rebuilt.d64", dir );
  r = asp_tank_create( rp, LOOP, 48000, e, sizeof e );
  CHECK( asp_rebuild( r, s, -1 ) == 0 );
  static int64_t a[2 * 5000], bq[2 * 5000];
  int64_t diff = 0;
  for( int64_t pos = 0; pos < LOOP; pos += 5000 )
    {
    int64_t n = LOOP - pos < 5000 ? LOOP - pos : 5000;
    asp_tank_get( t, pos, n, a ); asp_tank_get( r, pos, n, bq );
    if( memcmp( a, bq, (size_t)n * 16 ) ) diff++;
    }
  CHECK( diff == 0 );
  asp_tank_close( r );
  }

  /* 3. safe: a burst while safe records nothing; live again records it */
  asp_core_safe( c, 1 );
  bursts[0].at = tickn * ASP_TICK + 48000 * 2; bursts[0].on = 1; bursts[1].on = 0;
  int before = count( sd, "new" );
  for( int i = 0; i < 5 * 8; i++ ) step( c );
  CHECK( count( sd, "new" ) == before );
  asp_core_safe( c, 0 );
  bursts[0].at = tickn * ASP_TICK + 48000 * 2;
  for( int i = 0; i < 5 * 8; i++ ) step( c );
  CHECK( count( sd, "new" ) == before + 1 && count( sd, "safe_on" ) == 1 && count( sd, "safe_off" ) == 1 );
  bursts[0].on = 0;

  /* 4. the edge drops samples: the take is not trusted, the hunt starts again and the loop top moves */
  asp_core_edge( c, 0, 123 );
  asp_core_status( c, &st );
  CHECK( st.phase == 0 && count( sd, "edge_drop" ) == 1 );
  guard = 0;
  do { step( c ); asp_core_status( c, &st ); } while( st.phase == 0 && ++guard < 1000 );
  CHECK( st.phase == 1 && st.latency == LAT && st.origin > origin && count( sd, "sync_found" ) == 2 );

  asp_core_close( c );
  CHECK( ! asp_tank_dirty( t ) );
  asp_tank_close( t ); asp_close( s );
  free( outs );
  snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); if( system( cmd ) ) {}
  printf( fails ? "%d FAILED\n" : "all core tests passed\n", fails );
  return fails != 0;
  }
