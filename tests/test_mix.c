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
  { char x[300]; snprintf( x, sizeof x, "%s/cfg", r->dir ); setenv( "XDG_CONFIG_HOME", x, 1 ); }
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
  /* a mono take is stored with both channels as heard; mono is what the state says */
  CHECK( k0.channels == 2 && k1.channels == 2 );
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

  /* --- keep: a take is recorded and stored, and the loop stays empty --- */
  asp_core_defaults( &cfg ); cfg.automix.mode = ASP_AUTOMIX_KEEP;
  rig_open( &r, &cfg, 40 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 2, 0.1 );
  sim_until( &r.sim, base + LOOP + 48000 * 3 );
  { States s; states( &r, &s ); AspCoreStatus st; asp_core_status( r.c, &st );
    CHECK( s.n == 1 && ! strcmp( s.st[0].status, "off" ) && st.loop_takes == 0 );
    CHECK( matches( &r ) );                                        /* and the loop file is empty, as a rebuild says */
    asp_core_automix( r.c, ASP_AUTOMIX_OFF, 0, 0 ); }
  rig_close( &r );

  /* --- told which channel is used: mono from the start, in the middle at once, no conversion, and the loop is the same
         in the very next pass. (a take 2.27 loops long, played on the right only; a loud left-only noise is ignored) --- */
  asp_core_defaults( &cfg ); cfg.input = 2;
  rig_open( &r, &cfg, 200 );
  {
  int64_t b0 = ( r.sim.tickn + 120 ) * ASP_TICK, nb = 0, nextb = 0;
  sim_burst( &r.sim, b0 - 2 * 48000, 0, 0.3 );                       /* a loud thump on the unused left channel: nothing */
  int64_t endf = b0 + 35 * 28800 + 48000 * 3;
  while( r.sim.tickn * ASP_TICK < endf )
    {
    sim_step( &r.sim );
    while( nextb < 35 ) { int fr = 0; for( int k = 0; k < 16; k++ ) if( ! r.sim.b[k].on ) fr++; if( ! fr ) break; sim_burst( &r.sim, b0 + (int64_t)nextb * 28800, 1, 0.1 ); nextb++; nb++; }
    }
  States s; states( &r, &s ); AspTake tk; AspCoreStatus st; asp_core_status( r.c, &st );
  CHECK( s.n == 1 && asp_take_get( r.s, s.id[0], &tk ) == 0 && tk.channels == 2 && s.st[0].mono && s.st[0].l == 1 && s.st[0].r == 1 );
  CHECK( count_type( r.dir, "new" ) == 1 );                           /* the left-only noise opened nothing */
  CHECK( st.jobs == 0 );                                              /* nothing left to convert */
  }
  rig_close( &r );

  /* --- stereo stays stereo; identical channels count as mono --- */
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 40 );
  r.sim.dup = 1;
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 2, 0.1 );
  sim_until( &r.sim, base + LOOP + 48000 * 3 );
  { States s; states( &r, &s ); AspTake k; CHECK( s.n == 1 && asp_take_get( r.s, s.id[0], &k ) == 0 && k.channels == 2 && s.st[0].mono ); CHECK( matches( &r ) ); }
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

  /* --- a change is made only behind the head: with the head in the middle of a take, what is ahead of it is not touched --- */
  asp_core_defaults( &cfg );
  rig_open( &r, &cfg, 60 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  sim_burst( &r.sim, base, 2, 0.1 );
  sim_until( &r.sim, base + 48000 * 4 );
  {
  States s; states( &r, &s ); AspTake k0; AspCoreStatus st;
  CHECK( s.n == 1 && asp_take_get( r.s, s.id[0], &k0 ) == 0 );
  int64_t st0 = ( k0.off * 48000 ) % LOOP, len = s.st[0].len;
  static int64_t snap[2 * LOOP], now[2 * LOOP];
  int64_t head;
  do {                                                                  /* wait until the head is a third of the way along the take */
    sim_step( &r.sim ); asp_core_status( r.c, &st );
    head = ( ( st.tick - 1 - st.origin ) * ASP_TICK + st.latency + ASP_TICK ) % LOOP;
    } while( ( head - st0 + LOOP ) % LOOP < len / 3 || ( head - st0 + LOOP ) % LOOP > len / 2 );
  CHECK( asp_tank_get( r.t, 0, LOOP, snap ) == 0 );
  AspMixCmd m = { 1, -12, 0, 0, 0, 0 };
  CHECK( asp_core_mix( r.c, k0.iter, k0.off, &m ) == 1 );
  int checked = 0, changed_behind = 0;
  for( int i = 0; i < 40 && asp_core_pending( r.c, s.id[0] ); i++ )
    {
    sim_step( &r.sim ); asp_core_status( r.c, &st );
    head = ( ( st.tick - 1 - st.origin ) * ASP_TICK + st.latency + ASP_TICK ) % LOOP;      /* the first position not yet played */
    int64_t ho = ( head - st0 + LOOP ) % LOOP;
    CHECK( asp_tank_get( r.t, 0, LOOP, now ) == 0 );
    if( ho < len )
      {
      for( int64_t o = ho; o < len; o++ ) { int64_t at = ( st0 + o ) % LOOP; if( now[2*at] != snap[2*at] || now[2*at+1] != snap[2*at+1] ) { CHECK( 0 ); break; } }
      checked++;
      }
    for( int64_t o = 0; o < ( ho < len ? ho : len ); o++ ) { int64_t at = ( st0 + o ) % LOOP; if( now[2*at] != snap[2*at] ) { changed_behind = 1; break; } }
    }
  CHECK( checked >= 2 && changed_behind && ! asp_core_pending( r.c, s.id[0] ) );
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  /* --- by hand: a mix of its own keeps a take out of the hold; gain and pan are absolute; changes trail the head --- */
  asp_core_defaults( &cfg ); cfg.automix.mode = ASP_AUTOMIX_OVERLAP; cfg.automix.n = 2;
  rig_open( &r, &cfg, 140 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  for( int i = 0; i < 5; i++ ) sim_burst( &r.sim, base + (int64_t)i * LOOP, 2, 0.1 );
  sim_until( &r.sim, base + LOOP + 48000 * 3 );                    /* two takes are in */
  {
  States s; states( &r, &s ); AspTake k0;
  CHECK( s.n == 2 && asp_take_get( r.s, s.id[0], &k0 ) == 0 );
  AspMixCmd m = { 1, 7, 1, -30, 0, 0 };
  CHECK( asp_core_mix( r.c, k0.iter, k0.off, &m ) == 1 );
  CHECK( asp_core_pending( r.c, s.id[0] ) == 1 );                  /* waiting for the head, not done in this call */
  m.gdb = 5;                                                        /* a second value replaces, never adds */
  CHECK( asp_core_mix( r.c, k0.iter, k0.off, &m ) == 1 );
  CHECK( asp_core_mix( r.c, 9999, -1, &m ) == 0 );
  sim_until( &r.sim, base + LOOP * 4 + 48000 * 4 );
  states( &r, &s );
  printf( "by hand: %s(%d dB, pan %d) %s %s %s %s\n", s.st[0].status, s.st[0].gdb, s.st[0].pan, s.st[1].status, s.st[2].status, s.st[3].status, s.n > 4 ? s.st[4].status : "-" );
  CHECK( s.n == 5 && ! strcmp( s.st[0].status, "mix" ) && s.st[0].mixed && s.st[0].gdb == 5 && s.st[0].pan == -30 );
  CHECK( ! strcmp( s.st[1].status, "off" ) && ! strcmp( s.st[2].status, "off" ) );     /* the hold went on for the rest */
  CHECK( asp_core_pending( r.c, s.id[0] ) == 0 );
  CHECK( matches( &r ) );
  /* mute takes it out whatever its mix, the mix can still be changed, play brings it back at that mix */
  AspTake k4; CHECK( asp_take_get( r.s, s.id[4], &k4 ) == 0 );
  AspMixCmd mu = { 0, 0, 0, 0, ASP_MIX_MUTE, 0 };
  CHECK( asp_core_mix( r.c, k4.iter, k4.off, &mu ) == 1 );
  sim_until( &r.sim, base + LOOP * 6 );
  states( &r, &s ); CHECK( ! strcmp( s.st[4].status, "off" ) && ! s.st[4].mixed );
  CHECK( matches( &r ) );
  AspMixCmd g = { 1, -9, 0, 0, 0, 0 };
  CHECK( asp_core_mix( r.c, k4.iter, k4.off, &g ) == 1 );          /* muted, so only stored */
  sim_until( &r.sim, base + LOOP * 7 );
  mu.act = ASP_MIX_PLAY; mu.now = 1;
  CHECK( asp_core_mix( r.c, k4.iter, k4.off, &mu ) == 1 );
  sim_until( &r.sim, base + LOOP * 9 );
  states( &r, &s ); CHECK( ! strcmp( s.st[4].status, "mix" ) && s.st[4].mixed && s.st[4].gdb == -9 );
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  /* --- undo: the last take goes out and what its arrival took out comes back; again, and the one before; restore a log point --- */
  asp_core_defaults( &cfg ); cfg.automix.mode = ASP_AUTOMIX_OVERLAP; cfg.automix.n = 1;
  rig_open( &r, &cfg, 140 );
  base = ( r.sim.tickn + 120 ) * ASP_TICK + 500;
  for( int i = 0; i < 3; i++ ) sim_burst( &r.sim, base + (int64_t)i * LOOP, 2, 0.1 );
  sim_until( &r.sim, base + LOOP * 2 + 48000 * 3 );                  /* three takes are in */
  {
  States s; states( &r, &s );
  CHECK( s.n == 3 && ! strcmp( s.st[0].status, "off" ) && ! strcmp( s.st[1].status, "off" ) && ! strcmp( s.st[2].status, "mix" ) );
  int64_t mark = asp_log_last( r.s );
  CHECK( asp_core_undo( r.c, 0, 0 ) == 1 );
  sim_until( &r.sim, base + LOOP * 4 );
  states( &r, &s );
  CHECK( ! strcmp( s.st[0].status, "off" ) && ! strcmp( s.st[1].status, "mix" ) && ! strcmp( s.st[2].status, "off" ) );
  CHECK( matches( &r ) );
  CHECK( asp_core_undo( r.c, 0, 1 ) == 1 );
  sim_until( &r.sim, base + LOOP * 5 );
  states( &r, &s );
  CHECK( ! strcmp( s.st[0].status, "mix" ) && ! strcmp( s.st[1].status, "off" ) && ! strcmp( s.st[2].status, "off" ) );
  CHECK( matches( &r ) );
  CHECK( asp_core_restore( r.c, mark, 0 ) == 2 );                    /* back to how it was: only the last in */
  sim_until( &r.sim, base + LOOP * 7 );
  states( &r, &s );
  CHECK( ! strcmp( s.st[0].status, "off" ) && ! strcmp( s.st[1].status, "off" ) && ! strcmp( s.st[2].status, "mix" ) );
  CHECK( matches( &r ) );
  }
  rig_close( &r );

  printf( fails ? "%d FAILED\n" : "all mix tests passed\n", fails );
  return fails != 0;
  }
