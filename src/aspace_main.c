/* aspace: the runner. hosts the core on a pipe (or a simulated card) and serves the control port.
     a5pace new DIR NAME SECONDS            make a session with a loop of that many seconds
     a5pace run [options] DIR               run it
   pipe mode (default): raw little endian s32 stereo 48 kHz on stdin, ticks of 9600 frames, the loop on stdout:
     arecord ... | a5pace run DIR | aplay ...      (the edge does the device, and the loopback, see docs/EDGE.md)
   options: -S LAT  simulated card with LAT frames of round trip (no pipe), paced in real time
            --fast  simulated: as fast as it will go     --step  simulated: only on STEP n
            -C PORT control port (0 any free; default 0)  --safe  start with recording off
            --sync-max DB --sync-start DB --sync-target DB --sync-muted  the sync tone: ceiling, first level, wanted input peak
            --tank PATH  keep the loop file there (it is only a cache: rebuilt from the store when missing)
            --profile NAME  a named gate/sync setup from the list (a5pace profiles)
            --latency FRAMES  the round trip is declared, not measured: no sync tone (SYNC still measures)
            --lowcut HZ --strict  the gate ignores anything below the instrument's lowest note; no single-band opening
            -g DB -r DB  gate margin, ring  --automix SPEC (off | overlap N | swap [each])
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
#include "settings.h"
#include "simedge.h"

static volatile sig_atomic_t quit;
static void onsig( int s ) { (void)s; quit = 1; }

typedef struct
  {
  AspStore *store; AspTank *tank; AspCore *core; CtlSrv *ctl;
  Sim sim; int simmode, step, step_cl; int64_t steps; int run;
  double tick_ms_max, tick_ms_sum; int64_t ticks_timed, slow_ticks;   /* how long the core takes per tick: the cubieboard question */
  } Run;

static int64_t json_num( const char *j, const char *key, int64_t d )
  {
  char k[64]; snprintf( k, sizeof k, "\"%s\":", key );
  const char *p = strstr( j, k );
  return p ? atoll( p + strlen( k ) ) : d;
  }

static void addflag( char *text, const char *k, const char *v )
  {
  char one[300]; snprintf( one, sizeof one, "%s=%s", k, v );
  asp_settings_merge( text, 600, one );
  }

typedef struct { char settings[1100], profile[64]; } Stored;
static void stored_cb( int64_t id, const char *json, void *u )
  {
  Stored *st = u;
  if( id != 0 ) return;
  const char *s = strstr( json, "\"settings\":\"" ), *p = strstr( json, "\"profile\":\"" );
  if( p ) { p += 11; size_t l = strcspn( p, "\"" ); if( l > 63 ) l = 63; memcpy( st->profile, p, l ); st->profile[l] = 0; }
  if( s ) { s += 12; size_t l = strcspn( s, "\"" ); if( l > 1099 ) l = 1099; memcpy( st->settings, s, l ); st->settings[l] = 0; }
  }

/* defaults, then a named profile over them */
static int build_settings( const char *profile, char *out, size_t n )
  {
  char d[900], f[1000], t[1100];
  out[0] = 0;
  if( ! asp_config_dir( d, sizeof d ) ) { snprintf( f, sizeof f, "%s/defaults", d ); if( ! asp_settings_file( f, t, sizeof t ) ) asp_settings_merge( out, n, t ); }
  if( profile && *profile )
    {
    if( asp_profile_path( profile, f, sizeof f ) || asp_settings_file( f, t, sizeof t ) ) { fprintf( stderr, "aspace: no profile \"%s\" (a5pace profiles lists them)\n", profile ); return -1; }
    asp_settings_merge( out, n, t );
    }
  return 0;
  }

static void list_cb( const char *name, const char *note, void *u ) { (void)u; printf( "%-22s %s\n", name, note ); }

static int cmd_profiles( int argc, char **argv )
  {
  char d[900], f[1000], t[1100];
  if( asp_config_dir( d, sizeof d ) ) { fprintf( stderr, "no config directory (set A5PACE_CONFIG)\n" ); return 1; }
  if( argc >= 4 && ! strcmp( argv[2], "show" ) )
    {
    if( asp_profile_path( argv[3], f, sizeof f ) || asp_settings_file( f, t, sizeof t ) ) { fprintf( stderr, "no profile %s\n", argv[3] ); return 1; }
    printf( "%s\n", t ); return 0;
    }
  if( argc >= 3 && ! strcmp( argv[2], "install" ) )   /* the examples that ship with aspace, from profiles/ beside bin/ */
    {
    char self[1100], cmd[2300]; snprintf( self, sizeof self, "%s", argv[0] );
    char *sl = strrchr( self, '/' ); if( sl ) *sl = 0; else strcpy( self, "." );
    snprintf( cmd, sizeof cmd, "mkdir -p '%s/profiles' && for f in '%s'/../profiles/*; do [ -e '%s/profiles/'\"$(basename \"$f\")\" ] || cp \"$f\" '%s/profiles/'; done && echo installed into %s/profiles", d, self, d, d, d );
    return system( cmd ) ? 1 : 0;
    }
  if( ! asp_profile_list( list_cb, NULL ) ) printf( "no profiles in %s/profiles (a5pace profiles install copies the examples)\n", d );
  return 0;
  }

static int usage( void )
  {
  fprintf( stderr, "usage: a5pace new DIR NAME SECONDS [--profile NAME]\n       a5pace profiles | profiles show NAME | profiles install\n       a5pace run [-S LAT [--fast|--step]] [-C PORT] [--safe] [-g DB] [-r DB] [--automix SPEC] DIR\n" );
  return 2;
  }

static int cmd_new( int argc, char **argv )
  {
  if( argc != 5 && argc != 7 ) return usage();
  char err[ASP_ERR], desc[400], tp[1100], settings[1100];
  const char *profile = argc == 7 && ! strcmp( argv[5], "--profile" ) ? argv[6] : NULL;
  if( argc == 7 && ! profile ) return usage();
  if( build_settings( profile, settings, sizeof settings ) ) return 1;
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
  { char st[1300]; snprintf( st, sizeof st, "{\"profile\":\"%s\",\"settings\":\"%s\"}", profile ? profile : "", settings ); asp_take_set_state( s, 0, st, "aspace", 0 ); asp_log( s, "aspace", "settings", st, 0 ); }
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

static const char *mode_name( int m ) { return m == ASP_AUTOMIX_OVERLAP ? "overlap" : m == ASP_AUTOMIX_SWAP ? "swap" : "off"; }

static void prof_say( const char *name, const char *note, void *u )
  { Run *r = u; ctl_say( r->ctl, r->run, "%s\t%s", name, note ); }

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
      "\"automix\":\"%s\",\"automix_n\":%d,\"gate_db\":%.1f,\"ring_db\":%.1f,\"lowcut\":%.0f,\"strict\":%d,\"gate_learnt\":%d,\"sync_db\":%.1f,\"log_seq\":%lld,\"tick_ms_avg\":%.2f,\"tick_ms_max\":%.2f,\"slow_ticks\":%lld,\"profile\":\"%s\",\"session\":\"%.12s\"}",
      st.phase ? "play" : "sync", (long long)st.tick, st.tick / 5.0, (unsigned long long)st.iter, st.live, st.take_open, (long long)st.take_iter,
      (long long)st.take_off, (long long)st.take_frames, (long long)st.latency, (long long)st.origin, (long long)st.loop_frames, st.loop_takes, st.jobs,
      mode_name( st.automix.mode ), st.automix.n, st.gate_db, st.ring_db, st.lowcut, st.strict, st.gate_learnt, st.sync_level_db, (long long)st.log_seq, r->ticks_timed ? r->tick_ms_sum / r->ticks_timed : 0.0, r->tick_ms_max, (long long)r->slow_ticks, asp_core_profile( r->core ), asp_session_id( r->store ) );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "LINE" ) )          /* one line a person can read */
    {
    int64_t pos = st.phase ? ( ( st.tick - st.origin ) * ASP_TICK ) % st.loop_frames : 0;
    ctl_say( c, cl, "%s %5.1fs  loop %4.1f/%4.1fs  iter %llu  takes %d%s%s  %s  automix %s%s  gate %.0fdB%s  lat %.1fms%s%s",
      st.phase ? "PLAY" : "SYNC", st.tick / 5.0, pos / 48000.0, st.loop_frames / 48000.0, (unsigned long long)st.iter, st.loop_takes,
      st.jobs ? " (moving)" : "", st.take_open ? "  REC" : "", st.live ? "live" : "SAFE", mode_name( st.automix.mode ),
      st.automix.mode == ASP_AUTOMIX_OVERLAP ? " n" : "", st.gate_db, st.gate_learnt ? "" : " learning",
      st.latency * 1000.0 / 48000, asp_core_profile( r->core )[0] ? "  profile " : "", asp_core_profile( r->core ) );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "SAFE" ) ) { asp_core_safe( r->core, 1 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "LIVE" ) ) { asp_core_safe( r->core, 0 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "SYNC" ) ) { asp_core_resync( r->core ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "RELEARN" ) ) { asp_core_gate_relearn( r->core ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "PROFILES" ) ) { asp_profile_list( prof_say, r ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "SETTINGS" ) ) { ctl_say( c, cl, "profile %s", asp_core_profile( r->core ) ); ctl_say( c, cl, "%s", asp_core_settings_text( r->core ) ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "PROFILE" ) && n > 1 )
    {
    char pt[1100], cur[1100];
    if( build_settings( w[1], pt, sizeof pt ) ) { ctl_err( c, cl, "no profile %s", w[1] ); return; }
    snprintf( cur, sizeof cur, "%s", asp_core_settings_text( r->core ) );
    asp_settings_merge( cur, sizeof cur, pt );
    asp_core_settings( r->core, w[1], cur );      /* the sync keys wait for the next start; the rest is in force now */
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "GATE" ) && n > 1 )
    { double v = atof( w[1] ); if( w[1][0] == '+' || w[1][0] == '-' ) v += st.gate_db; asp_core_gate( r->core, v, 0 ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "LOWCUT" ) && n > 1 ) { asp_core_gate_options( r->core, atof( w[1] ), -1 ); ctl_ok( c, cl ); }   /* nothing below this opens the gate */
  else if( ! strcmp( w[0], "STRICT" ) && n > 1 ) { asp_core_gate_options( r->core, -1, atoi( w[1] ) ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "RING" ) && n > 1 ) { asp_core_gate( r->core, 0, atof( w[1] ) ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "AUTOMIX" ) && n > 1 )
    {
    for( char *q = w[1]; *q; q++ ) if( *q >= 'a' && *q <= 'z' ) *q -= 32;
    int m = ! strcmp( w[1], "OVERLAP" ) ? ASP_AUTOMIX_OVERLAP :
            ! strcmp( w[1], "SWAP" ) ? ASP_AUTOMIX_SWAP : ! strcmp( w[1], "OFF" ) ? ASP_AUTOMIX_OFF : -1;
    if( m < 0 ) { ctl_err( c, cl, "AUTOMIX OFF | OVERLAP n | SWAP [EACH]" ); return; }
    int arg = n > 2 ? atoi( w[2] ) : 0, each = n > 2 && ( w[2][0] == 'e' || w[2][0] == 'E' );
    if( m == ASP_AUTOMIX_SWAP ) arg = 0;
    if( m == ASP_AUTOMIX_OVERLAP && arg < 1 ) arg = 4;
    asp_core_automix( r->core, m, arg, each );
    ctl_ok( c, cl );
    }
  else if( ! strcmp( w[0], "EDGE" ) && n > 2 )
    { asp_core_edge( r->core, ! strcasecmp( w[1], "starve" ), atoll( w[2] ) ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "TAKES" ) ) { asp_fold( r->store, -1, fold_cb, r ); ctl_ok( c, cl ); }
  else if( ! strcmp( w[0], "REVERT" ) && n > 1 )
    {
    int64_t q = asp_core_revert( r->core, atoll( w[1] ) );
    if( q < 0 ) ctl_err( c, cl, "REVERT needs SAFE first, and a log point that exists" ); else { ctl_say( c, cl, "%lld", (long long)q ); ctl_ok( c, cl ); }
    }
  else if( ! strcmp( w[0], "EVENT" ) && n > 2 && ( ! strncmp( w[1], "edge_", 5 ) || ! strncmp( w[1], "shell_", 6 ) ) )
    {
    /* the edge or the shell tells the log something (a latency report, ...): EVENT edge_latency {json} */
    const char *js = strstr( line, w[1] ); js += strlen( w[1] ); while( *js == ' ' ) js++;
    asp_log( r->store, "edge", w[1], *js ? js : "{}", st.tick );
    ctl_ok( c, cl );
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

/* a tick is 200 ms of audio. the edge's buffers ride out a slow one, but a stall of a few hundred ms (an sd card
   syncing) is a dropout; so every tick is timed and the slow ones are logged (rate limited). */
static void timed( Run *r, double ms )
  {
  r->tick_ms_sum += ms; r->ticks_timed++;
  if( ms > r->tick_ms_max ) r->tick_ms_max = ms;
  if( ms > 100 )
    {
    r->slow_ticks++;
    if( r->slow_ticks <= 20 || ( r->slow_ticks % 100 ) == 0 )
      { char b[120]; snprintf( b, sizeof b, "{\"ms\":%.0f,\"count\":%lld}", ms, (long long)r->slow_ticks ); asp_log( r->store, "edge", "shell_slow_tick", b, -1 ); }
    }
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
  const char *dir = NULL, *profile = NULL, *tankpath = NULL;
  char flagtext[600] = "", settings[1100] = "", stored[1100] = "", pname[64] = "";
  for( int i = 2; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "-S" ) && i + 1 < argc ) { lat = atoll( argv[++i] ); r.simmode = 1; }
    else if( ! strcmp( argv[i], "--fast" ) ) fast = 1;
    else if( ! strcmp( argv[i], "--step" ) ) r.step = 1;
    else if( ! strcmp( argv[i], "-C" ) && i + 1 < argc ) port = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--safe" ) ) cfg.live = 0;
    else if( ! strcmp( argv[i], "--nofeed" ) ) cfg.feed = 0;       /* takes are stored but not put into the loop (alignment tests) */
    else if( ! strcmp( argv[i], "-q" ) ) quiet = 1;
    else if( ! strcmp( argv[i], "--tank" ) && i + 1 < argc ) tankpath = argv[++i];                   /* where the loop file lives (it is a cache: /dev/shm on a board with an sd card) */
    else if( ! strcmp( argv[i], "--profile" ) && i + 1 < argc ) profile = argv[++i];                   /* a named setup from the list (a5pace profiles) */
    else if( ! strcmp( argv[i], "--sync-max" ) && i + 1 < argc ) addflag( flagtext, "sync_max", argv[++i] );   /* never louder than this, dB re asp4's sync */
    else if( ! strcmp( argv[i], "--sync-start" ) && i + 1 < argc ) addflag( flagtext, "sync_start", argv[++i] );
    else if( ! strcmp( argv[i], "--latency" ) && i + 1 < argc ) cfg.declared_latency = atoll( argv[++i] );   /* declared round trip, frames: no hunt */
    else if( ! strcmp( argv[i], "--sync-muted" ) ) cfg.sync.output_muted = 1;                           /* the shell has muted what is heard */
    else if( ! strcmp( argv[i], "--sync-target" ) && i + 1 < argc ) addflag( flagtext, "sync_target", argv[++i] );
    else if( ! strcmp( argv[i], "-g" ) && i + 1 < argc ) addflag( flagtext, "gate", argv[++i] );
    else if( ! strcmp( argv[i], "-r" ) && i + 1 < argc ) addflag( flagtext, "ring", argv[++i] );
    else if( ! strcmp( argv[i], "--lowcut" ) && i + 1 < argc ) addflag( flagtext, "lowcut", argv[++i] );    /* the instrument's lowest note, Hz */
    else if( ! strcmp( argv[i], "--strict" ) ) addflag( flagtext, "strict", "1" );
    else if( ! strcmp( argv[i], "--automix" ) && i + 1 < argc ) addflag( flagtext, "automix", argv[++i] );
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else return usage();
    }
  if( ! dir ) return usage();
  char err[ASP_ERR], tp[1100], ctlp[1100];
  r.store = asp_open( dir, 0, err );
  if( ! r.store ) { fprintf( stderr, "%s\n", err ); return 1; }
  { /* the session's settings, then a chosen profile, then what was said on the command line */
    Stored sv; memset( &sv, 0, sizeof sv ); asp_fold( r.store, -1, stored_cb, &sv );
    snprintf( stored, sizeof stored, "%s", sv.settings ); snprintf( pname, sizeof pname, "%s", sv.profile );
    if( ! stored[0] && build_settings( NULL, settings, sizeof settings ) ) return 1;
    if( stored[0] ) snprintf( settings, sizeof settings, "%s", stored );
    if( profile )
      {
      char pt[1100]; if( build_settings( profile, pt, sizeof pt ) ) return 1;
      asp_settings_merge( settings, sizeof settings, pt ); snprintf( pname, sizeof pname, "%s", profile );
      }
    asp_settings_merge( settings, sizeof settings, flagtext );
    asp_settings_cfg( &cfg, settings );
  }
  int64_t frames = json_num( asp_descriptor( r.store ), "loop", 0 );
  if( tankpath ) snprintf( tp, sizeof tp, "%s", tankpath ); else snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
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
  if( profile || flagtext[0] || ! stored[0] ) asp_core_settings( r.core, pname, settings );   /* what is in force is in the log
  */
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
      { struct timespec a, b; clock_gettime( CLOCK_MONOTONIC, &a ); sim_step( &r.sim ); clock_gettime( CLOCK_MONOTONIC, &b ); timed( &r, ( b.tv_sec - a.tv_sec ) * 1e3 + ( b.tv_nsec - a.tv_nsec ) / 1e6 ); }
      if( r.step && r.steps <= 0 && r.step_cl >= 0 ) { last = asp_log_since( r.store, last, ev_cb, &r ); ctl_ok( r.ctl, r.step_cl ); r.step_cl = -1; }
      }
    else
      {
      if( read_tick( in ) ) break;                        /* the edge closed */
      { struct timespec a, b; clock_gettime( CLOCK_MONOTONIC, &a ); asp_core_tick( r.core, in, out ); clock_gettime( CLOCK_MONOTONIC, &b ); timed( &r, ( b.tv_sec - a.tv_sec ) * 1e3 + ( b.tv_nsec - a.tv_nsec ) / 1e6 ); }
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
  if( argc >= 2 && ! strcmp( argv[1], "profiles" ) ) return cmd_profiles( argc, argv );
  if( argc >= 2 && ! strcmp( argv[1], "run" ) ) return cmd_run( argc, argv );
  return usage();
  }
