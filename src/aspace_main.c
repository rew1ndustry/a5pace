/* aspace: the runner. hosts the core on a pipe (or a simulated card) and serves the control port.
     aspace new DIR NAME SECONDS            make a session with a loop of that many seconds
     aspace run [options] DIR               run it
   pipe mode (default): raw little endian s32 stereo 48 kHz on stdin, ticks of 9600 frames, the loop on stdout:
     arecord ... | aspace run DIR | aplay ...      (the edge does the device, and the loopback, see docs/EDGE.md)
   options: -S LAT  simulated card with LAT frames of round trip (no pipe), paced in real time
            --fast  simulated: as fast as it will go     --step  simulated: only on STEP n
            -C PORT control port (0 any free; default 0)  --safe  start with recording off
            -g DB -r DB  gate margin, ring  --automix SPEC (off | fade N | overlap N | swap [each])
   the port is written to DIR/ctl as "PORT PID". logs go to stderr; stdout is audio in pipe mode. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aspcore.h"
#include "aspengine.h"
#include "ctlsrv.h"
#include "plat.h"
#include "simedge.h"

static volatile sig_atomic_t quit;
static void onsig( int s ) { (void)s; quit = 1; }

typedef struct
  {
  AspStore *store; AspTank *tank; AspCore *core; CtlSrv *ctl;
  Sim sim; int simmode, step, step_cl; int64_t steps; int run;
  } Run;

static int64_t json_num( const char *j, const char *key, int64_t d )
  {
  char k[64]; snprintf( k, sizeof k, "\"%s\":", key );
  const char *p = strstr( j, k );
  return p ? atoll( p + strlen( k ) ) : d;
  }

static int usage( void )
  {
  fprintf( stderr, "usage: aspace new DIR NAME SECONDS\n       aspace run [-S LAT [--fast|--step]] [-C PORT] [--safe] [-g DB] [-r DB] [--automix SPEC] DIR\n" );
  return 2;
  }

static int cmd_new( int argc, char **argv )
  {
  if( argc != 5 ) return usage();
  char err[ASP_ERR], desc[400], tp[1100];
  double secs = atof( argv[4] );
  int64_t frames = (int64_t)( secs * 48000 + 0.5 );
  if( frames < 4 * ASP_TICK ) { fprintf( stderr, "a loop needs at least %d frames\n", 4 * ASP_TICK ); return 1; }
  snprintf( desc, sizeof desc, "{\"v\":1,\"name\":\"%s\",\"loop\":%lld,\"rate\":48000,\"created\":%lld}", argv[3], (long long)frames, (long long)time( NULL ) );
  AspStore *s = asp_create( argv[2], desc, err );
  if( ! s ) { fprintf( stderr, "%s\n", err ); return 1; }
  snprintf( tp, sizeof tp, "%s/cache/loop.d64", argv[2] );
  AspTank *t = asp_tank_create( tp, frames, 48000, err, sizeof err );
  if( ! t ) { fprintf( stderr, "%s\n", err ); return 1; }
  asp_tank_bind( t, asp_session_id( s ) );
  asp_tank_close( t ); asp_close( s );
  printf( "%s: %lld frames (%.3f s)\n", argv[2], (long long)frames, frames / 48000.0 );
  return 0;
  }

static void ev_cb( int64_t seq, int64_t tick, const char *actor, const char *type, const char *args, void *u )
  {
  Run *r = u; char b[900];
  (void)seq; (void)actor;
  if( ! strcmp( type, "take_state" ) || ! strcmp( type, "take_added" ) ) return;
  snprintf( b, sizeof b, "@%lld %s %s", (long long)tick, type, args );
  ctl_broadcast( r->ctl, b );
  }

static void fold_cb( int64_t id, const char *st, void *u )
  {
  Run *r = u; AspTake k;
  if( asp_take_get( r->store, id, &k ) ) return;
  ctl_say( r->ctl, r->run, "take %lld iter %lld off %lld frames %lld ch %d %s", (long long)id, (long long)k.iter, (long long)k.off, (long long)k.frames, k.channels, st );
  }

static const char *mode_name( int m ) { return m == ASP_AUTOMIX_FADE ? "fade" : m == ASP_AUTOMIX_OVERLAP ? "overlap" : m == ASP_AUTOMIX_SWAP ? "swap" : "off"; }

static void handle( void *u, int cl, const char *line )
  {
  Run *r = u; CtlSrv *c = r->ctl;
  char w[8][128]; int n = 0;
  {
  const char *p = line;
  while( *p && n < 8 )
    {
    while( *p == ' ' ) p++;
    if( ! *p ) break;
    int k = 0; while( *p && *p != ' ' && k < 127 ) w[n][k++] = *p++;
    w[n++][k] = 0;
    }
  }
  if( ! n ) return;
  for( char *q = w[0]; *q; q++ ) if( *q >= 'a' && *q <= 'z' ) *q -= 32;
  AspCoreStatus st; asp_core_status( r->core, &st );
  r->run = cl;

  if( ! strcmp( w[0], "STATE" ) )
    {
    ctl_say( c, cl, "{\"phase\":\"%s\",\"tick\":%lld,\"seconds\":%.1f,\"iter\":%llu,\"live\":%d,\"take_open\":%d,\"take_iter\":%lld,\"take_off\":%lld,"
      "\"take_frames\":%lld,\"latency\":%lld,\"origin\":%lld,\"loop_frames\":%lld,\"loop_takes\":%d,\"jobs\":%d,"
      "\"gain_steps\":%d,\"automix\":\"%s\",\"automix_n\":%d,\"gate_db\":%.1f,\"ring_db\":%.1f,\"gate_learnt\":%d,\"sync_db\":%.1f,\"log_seq\":%lld,\"session\":\"%.12s\"}",
      st.phase ? "play" : "sync", (long long)st.tick, st.tick / 5.0, (unsigned long long)st.iter, st.live, st.take_open, (long long)st.take_iter,
      (long long)st.take_off, (long long)st.take_frames, (long long)st.latency, (long long)st.origin, (long long)st.loop_frames, st.loop_takes, st.jobs,
      st.gain_steps, mode_name( st.automix.mode ), st.automix.n, st.gate_db, st.ring_db, st.gate_learnt, st.sync_level_db, (long long)st.log_seq, asp_session_id( r->store ) );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "LINE" ) )          /* one line a person can read */
    {
    int64_t pos = st.phase ? ( ( st.tick - st.origin ) * ASP_TICK ) % st.loop_frames : 0;
    ctl_say( c, cl, "%s %5.1fs  loop %4.1f/%4.1fs  iter %llu  takes %d%s%s  %s  gain %ddB  automix %s%s  gate %.0fdB%s  lat %.1fms",
      st.phase ? "PLAY" : "SYNC", st.tick / 5.0, pos / 48000.0, st.loop_frames / 48000.0, (unsigned long long)st.iter, st.loop_takes,
      st.jobs ? " (moving)" : "", st.take_open ? "  REC" : "", st.live ? "live" : "SAFE", -6 * st.gain_steps, mode_name( st.automix.mode ),
      st.automix.mode == ASP_AUTOMIX_FADE || st.automix.mode == ASP_AUTOMIX_OVERLAP ? " n" : "", st.gate_db, st.gate_learnt ? "" : " learning",
      st.latency * 1000.0 / 48000 );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "SAFE" ) ) { asp_core_safe( r->core, 1 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "LIVE" ) ) { asp_core_safe( r->core, 0 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "SYNC" ) ) { asp_core_resync( r->core ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "RELEARN" ) ) { asp_core_gate_relearn( r->core ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "GATE" ) && n > 1 )
    { double v = atof( w[1] ); if( w[1][0] == '+' || w[1][0] == '-' ) v += st.gate_db; asp_core_gate( r->core, v, 0 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "RING" ) && n > 1 ) { asp_core_gate( r->core, 0, atof( w[1] ) ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "AUTOMIX" ) && n > 1 )
    {
    for( char *q = w[1]; *q; q++ ) if( *q >= 'a' && *q <= 'z' ) *q -= 32;
    int m = ! strcmp( w[1], "FADE" ) ? ASP_AUTOMIX_FADE : ! strcmp( w[1], "OVERLAP" ) ? ASP_AUTOMIX_OVERLAP :
            ! strcmp( w[1], "SWAP" ) ? ASP_AUTOMIX_SWAP : ! strcmp( w[1], "OFF" ) ? ASP_AUTOMIX_OFF : -1;
    if( m < 0 ) { ctl_err( c, cl, "AUTOMIX OFF | FADE n [RESTORE] | OVERLAP n | SWAP [EACH]" ); return; }
    int arg = n > 2 ? atoi( w[2] ) : 0, bits = 0;
    for( int k = 2; k < n; k++ )
      { if( ! strncasecmp( w[k], "each", 4 ) ) bits |= 1; else if( ! strncasecmp( w[k], "restore", 4 ) ) bits |= 2; }
    if( m == ASP_AUTOMIX_SWAP ) arg = 0;
    if( ( m == ASP_AUTOMIX_FADE || m == ASP_AUTOMIX_OVERLAP ) && arg < 1 ) arg = m == ASP_AUTOMIX_FADE ? 1 : 4;
    asp_core_automix( r->core, m, arg, bits );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "EDGE" ) && n > 2 )
    { asp_core_edge( r->core, ! strcasecmp( w[1], "starve" ), atoll( w[2] ) ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "GAIN" ) && n > 1 ) { asp_core_gain( r->core, atoi( w[1] ), 0 ); ctl_ok( c, cl ); }          /* absolute: steps of 6 dB down */
  else if( ! strcmp( w[0], "DOWN" ) ) { asp_core_gain( r->core, n > 1 ? atoi( w[1] ) : 1, 1 ); ctl_ok( c, cl ); }   /* knock the loop down n * 6 dB */
  else if( ! strcmp( w[0], "UP" ) ) { asp_core_gain( r->core, -( n > 1 ? atoi( w[1] ) : 1 ), 1 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "TAKES" ) ) { asp_fold( r->store, -1, fold_cb, r ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "REVERT" ) && n > 1 )
    {
    int64_t q = asp_core_revert( r->core, atoll( w[1] ) );
    if( q < 0 ) ctl_err( c, cl, "REVERT needs SAFE first, and a log point that exists" ); else { ctl_say( c, cl, "%lld", (long long)q ); ctl_ok( c, cl ); }
    }
  else if( ! strcmp( w[0], "WATCH" ) ) { ctl_watch( c, cl, 1 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "QUIT" ) ) { ctl_ok( c, cl ); quit = 1; }
  else if( ! strcmp( w[0], "SIM" ) && r->simmode && n > 3 && ! strcasecmp( w[1], "BURST" ) )
    {
    /* SIM BURST ch amp [ticks_from_now]: ch 0 left 1 right 2 both; amp as a fraction of full scale */
    int ch = atoi( w[2] ); double amp = atof( w[3] );
    sim_burst( &r->sim, ( r->sim.tickn + ( n > 4 ? atoi( w[4] ) : 1 ) ) * ASP_TICK, ch, amp );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "SIM" ) && r->simmode && n > 2 && ! strcasecmp( w[1], "DUP" ) ) { r->sim.dup = atoi( w[2] ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "STEP" ) && r->simmode ) { r->steps += n > 1 ? atoll( w[1] ) : 1; r->step_cl = cl; }   /* OK comes when they are done */
  else ctl_err( c, cl, "unknown or incomplete: %s", line );
  }

static int read_tick( int32_t *in )
  {
  unsigned char b[ASP_TICK * 8];
  size_t got = 0;
  while( got < sizeof b )
    {
    ssize_t n = read( 0, b + got, sizeof b - got );
    if( n <= 0 ) return -1;
    got += (size_t)n;
    }
  for( int i = 0; i < 2 * ASP_TICK; i++ ) in[i] = (int32_t)asp_ld32( b + 4 * i );
  return 0;
  }

static int write_tick( const int32_t *out )
  {
  unsigned char b[ASP_TICK * 8];
  for( int i = 0; i < 2 * ASP_TICK; i++ ) asp_st32( b + 4 * i, (uint32_t)out[i] );
  size_t put = 0;
  while( put < sizeof b )
    {
    ssize_t n = write( 1, b + put, sizeof b - put );
    if( n <= 0 ) return -1;
    put += (size_t)n;
    }
  return 0;
  }

static void nap( long ms ) { struct timespec t = { ms / 1000, ( ms % 1000 ) * 1000000L }; nanosleep( &t, NULL ); }

static int cmd_run( int argc, char **argv )
  {
  Run r; memset( &r, 0, sizeof r ); r.step_cl = -1;
  AspCoreCfg cfg; asp_core_defaults( &cfg );
  int port = 0, fast = 0, quiet = 0; int64_t lat = 0;
  const char *dir = NULL;
  for( int i = 2; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "-S" ) && i + 1 < argc ) { lat = atoll( argv[++i] ); r.simmode = 1; }
    else if( ! strcmp( argv[i], "--fast" ) ) fast = 1;
    else if( ! strcmp( argv[i], "--step" ) ) r.step = 1;
    else if( ! strcmp( argv[i], "-C" ) && i + 1 < argc ) port = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--safe" ) ) cfg.live = 0;
    else if( ! strcmp( argv[i], "-q" ) ) quiet = 1;
    else if( ! strcmp( argv[i], "-g" ) && i + 1 < argc ) cfg.gate_db = atof( argv[++i] );
    else if( ! strcmp( argv[i], "-r" ) && i + 1 < argc ) cfg.ring_db = atof( argv[++i] );
    else if( ! strcmp( argv[i], "--automix" ) && i + 1 < argc )
      {
      const char *s = argv[++i];
      cfg.automix.mode = ! strncmp( s, "fade", 4 ) ? ASP_AUTOMIX_FADE : ! strncmp( s, "overlap", 7 ) ? ASP_AUTOMIX_OVERLAP :
                         ! strncmp( s, "swap", 4 ) ? ASP_AUTOMIX_SWAP : ASP_AUTOMIX_OFF;
      const char *sp = strchr( s, ' ' );
      cfg.automix.n = sp ? atoi( sp + 1 ) : ( cfg.automix.mode == ASP_AUTOMIX_FADE ? 1 : 4 );
      cfg.automix.each = sp && strstr( sp, "each" ) != NULL;
      }
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else return usage();
    }
  if( ! dir ) return usage();
  char err[ASP_ERR], tp[1100], ctlp[1100];
  r.store = asp_open( dir, 0, err );
  if( ! r.store ) { fprintf( stderr, "%s\n", err ); return 1; }
  int64_t frames = json_num( asp_descriptor( r.store ), "loop", 0 );
  snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
  r.tank = frames ? asp_tank_open( tp, err, sizeof err ) : NULL;
  int rebuild = 0;
  if( r.tank && ( asp_tank_dirty( r.tank ) || asp_tank_frames( r.tank ) != frames || asp_tank_legacy( r.tank ) ) ) { rebuild = 1; asp_tank_close( r.tank ); r.tank = NULL; }
  if( ! r.tank )
    {
    if( ! frames ) { fprintf( stderr, "%s: no loop length in the session\n", dir ); return 1; }
    r.tank = asp_tank_create( tp, frames, 48000, err, sizeof err );
    if( ! r.tank ) { fprintf( stderr, "%s\n", err ); return 1; }
    asp_tank_bind( r.tank, asp_session_id( r.store ) );
    if( ! quiet ) fprintf( stderr, "aspace: %s, rebuilding the loop from the store\n", rebuild ? "the loop was not closed cleanly" : "no loop file" );
    if( asp_rebuild( r.tank, r.store, -1 ) ) { fprintf( stderr, "aspace: rebuild failed\n" ); return 1; }
    }
  r.core = asp_core_open( r.store, r.tank, &cfg, err, sizeof err );
  if( ! r.core ) { fprintf( stderr, "%s\n", err ); return 1; }
  int chosen = 0;
  r.ctl = ctl_open( port, &chosen );
  if( ! r.ctl ) { fprintf( stderr, "aspace: no control port\n" ); return 1; }
  snprintf( ctlp, sizeof ctlp, "%s/ctl", dir );
  { FILE *f = fopen( ctlp, "w" ); if( f ) { fprintf( f, "%d %d\n", chosen, (int)getpid() ); fclose( f ); } }
  if( r.simmode ) sim_init( &r.sim, r.core, lat, 4 );
  signal( SIGINT, onsig ); signal( SIGTERM, onsig ); signal( SIGPIPE, SIG_IGN );
  if( ! quiet ) fprintf( stderr, "aspace: %s, control port %d%s\n", dir, chosen, r.simmode ? ", simulated card" : "" );

  int32_t in[2 * ASP_TICK], out[2 * ASP_TICK];
  int64_t last = asp_log_last( r.store );
  struct timespec t0; clock_gettime( CLOCK_MONOTONIC, &t0 );
  int64_t started = 0;
  while( ! quit )
    {
    ctl_poll( r.ctl, handle, &r );
    if( r.simmode )
      {
      if( r.step ) { if( r.steps <= 0 ) { nap( 5 ); continue; } r.steps--; }
      else if( ! fast )                                   /* real time */
        {
        struct timespec t; clock_gettime( CLOCK_MONOTONIC, &t );
        double due = started * 0.2, now = ( t.tv_sec - t0.tv_sec ) + ( t.tv_nsec - t0.tv_nsec ) / 1e9;
        if( now < due ) { nap( (long)( ( due - now ) * 1000 ) + 1 ); continue; }
        }
      started++;
      sim_step( &r.sim );
      if( r.step && r.steps <= 0 && r.step_cl >= 0 ) { last = asp_log_since( r.store, last, ev_cb, &r ); ctl_ok( r.ctl, r.step_cl ); r.step_cl = -1; }
      }
    else
      {
      if( read_tick( in ) ) break;                        /* the edge closed */
      asp_core_tick( r.core, in, out );
      if( write_tick( out ) ) break;
      }
    if( ctl_watchers( r.ctl ) || ( r.sim.tickn % 25 ) == 0 )
      last = asp_log_since( r.store, last, ev_cb, &r );
    }
  if( ! quiet ) fprintf( stderr, "aspace: closing\n" );
  asp_log_since( r.store, last, ev_cb, &r );
  asp_core_close( r.core );
  ctl_close( r.ctl ); unlink( ctlp );
  asp_tank_close( r.tank ); asp_close( r.store );
  return 0;
  }

int main( int argc, char **argv )
  {
  if( argc >= 2 && ! strcmp( argv[1], "new" ) ) return cmd_new( argc, argv );
  if( argc >= 2 && ! strcmp( argv[1], "run" ) ) return cmd_run( argc, argv );
  return usage();
  }
