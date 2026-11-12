/* a5droid: aspace on an Android phone, as a native program (docs/ANDROID.md). the smoke-test host: the phone's mic in, the headphones out.
     a5droid DIR [--secs N] [--latency FRAMES] [--preset voice|unprocessed|camcorder] [--rate-check]
   the pattern of include/aspace.h: AAudio callbacks only move frames through two lock-free rings; a worker thread ticks the core.
   there is no loopback here, so the round trip is declared (--latency, default 9600 frames = a guess, tune it by ear), not measured.
   a drop (input ring full) or a starve (output ring empty after output began) is told to the core, as EDGE.md asks. SIGINT / SIGTERM or --secs
   ends it: the core drains its queued moves first. needs API 29 (AAudio input presets). built by tools/a5droid-build.sh, run from adb shell or an app. */
#include <aaudio/AAudio.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "aspace.h"

#define RING_TICKS 8
#define RING_FRAMES ( ASP_TICK * RING_TICKS )

typedef struct { int32_t *buf; _Atomic uint64_t w, r; } Ring;        /* single producer, single consumer, stereo s32 frames */
static Ring inr, outr;
static _Atomic long drops, starves;
static _Atomic int flowing, quit;
static _Atomic float playpeak, cappeak;      /* the largest sample the devices saw since the log last looked: is anything actually reaching the speaker / the mic */
static _Atomic long capframes, playframes;   /* every frame the two devices handled, to see their clocks against the monotonic one */

static void ring_init( Ring *q ) { q->buf = calloc( (size_t)RING_FRAMES * 2, 4 ); atomic_init( &q->w, 0 ); atomic_init( &q->r, 0 ); }
static uint64_t ring_used( Ring *q ) { return atomic_load( &q->w ) - atomic_load( &q->r ); }
static void ring_put( Ring *q, const int32_t *f, int n )
  {
  uint64_t w = atomic_load( &q->w );
  for( int i = 0; i < n; i++, w++ ) { q->buf[2 * ( w % RING_FRAMES )] = f[2 * i]; q->buf[2 * ( w % RING_FRAMES ) + 1] = f[2 * i + 1]; }
  atomic_store( &q->w, w );
  }
static void ring_get( Ring *q, int32_t *f, int n )
  {
  uint64_t r = atomic_load( &q->r );
  for( int i = 0; i < n; i++, r++ ) { f[2 * i] = q->buf[2 * ( r % RING_FRAMES )]; f[2 * i + 1] = q->buf[2 * ( r % RING_FRAMES ) + 1]; }
  atomic_store( &q->r, r );
  }

static int32_t to_s32( float v ) { if( v > 0.9999999f ) v = 0.9999999f; if( v < -1.0f ) v = -1.0f; return (int32_t)( v * 2147483648.0f ); }

static aaudio_data_callback_result_t on_capture( AAudioStream *s, void *u, void *data, int32_t n )
  {
  (void)s; (void)u;
  atomic_fetch_add( &capframes, n );
  const float *f = data; int32_t tmp[2 * 480];
  while( n > 0 )
    {
    int k = n < 480 ? n : 480;
    for( int i = 0; i < 2 * k; i++ ) tmp[i] = to_s32( f[i] );
    if( RING_FRAMES - ring_used( &inr ) < (uint64_t)k ) atomic_fetch_add( &drops, k ); else ring_put( &inr, tmp, k );
    for( int i = 0; i < 2 * k; i++ ) if( fabsf( f[i] ) > atomic_load( &cappeak ) ) atomic_store( &cappeak, fabsf( f[i] ) );
    f += 2 * k; n -= k;
    }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

static aaudio_data_callback_result_t on_play( AAudioStream *s, void *u, void *data, int32_t n )
  {
  (void)s; (void)u;
  atomic_fetch_add( &playframes, n );
  float *f = data; int32_t tmp[2 * 480];
  while( n > 0 )
    {
    int k = n < 480 ? n : 480;
    if( ring_used( &outr ) >= (uint64_t)k ) { ring_get( &outr, tmp, k ); for( int i = 0; i < 2 * k; i++ ) f[i] = (float)tmp[i] / 2147483648.0f; }
    else { memset( f, 0, (size_t)k * 8 ); if( atomic_load( &flowing ) ) atomic_fetch_add( &starves, k ); }
    for( int i = 0; i < 2 * k; i++ ) if( fabsf( f[i] ) > atomic_load( &playpeak ) ) atomic_store( &playpeak, fabsf( f[i] ) );
    f += 2 * k; n -= k;
    }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

static void on_error( AAudioStream *s, void *u, aaudio_result_t e ) { (void)s; (void)u; fprintf( stderr, "a5droid: stream error %s\n", AAudio_convertResultToText( e ) ); atomic_store( &quit, 1 ); }
static void on_signal( int s ) { (void)s; atomic_store( &quit, 1 ); }

static AAudioStream *open_stream( int input, int preset, aaudio_data_callback_result_t (*cb)( AAudioStream *, void *, void *, int32_t ) )
  {
  AAudioStreamBuilder *b; AAudioStream *st = 0;
  if( AAudio_createStreamBuilder( &b ) != AAUDIO_OK ) return 0;
  AAudioStreamBuilder_setDirection( b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT );
  AAudioStreamBuilder_setPerformanceMode( b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY );
  AAudioStreamBuilder_setSharingMode( b, AAUDIO_SHARING_MODE_SHARED );
  AAudioStreamBuilder_setSampleRate( b, 48000 );
  AAudioStreamBuilder_setChannelCount( b, 2 );
  AAudioStreamBuilder_setFormat( b, AAUDIO_FORMAT_PCM_FLOAT );
  AAudioStreamBuilder_setDataCallback( b, cb, 0 );
  AAudioStreamBuilder_setErrorCallback( b, on_error, 0 );
  if( input ) AAudioStreamBuilder_setInputPreset( b, preset );
  else AAudioStreamBuilder_setUsage( b, AAUDIO_USAGE_MEDIA );
  aaudio_result_t r = AAudioStreamBuilder_openStream( b, &st );
  AAudioStreamBuilder_delete( b );
  if( r != AAUDIO_OK ) { fprintf( stderr, "a5droid: cannot open the %s stream: %s\n", input ? "input" : "output", AAudio_convertResultToText( r ) ); return 0; }
  fprintf( stderr, "a5droid: %s: %d Hz, %d channels, burst %d frames, device %d\n", input ? "input" : "output", AAudioStream_getSampleRate( st ),
           AAudioStream_getChannelCount( st ), AAudioStream_getFramesPerBurst( st ), AAudioStream_getDeviceId( st ) );
  return st;
  }

int main( int argc, char **argv )
  {
  const char *dir = 0; int secs = 0, preset = AAUDIO_INPUT_PRESET_VOICE_RECOGNITION; long latency = 9600;
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--secs" ) && i + 1 < argc ) secs = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--latency" ) && i + 1 < argc ) latency = atol( argv[++i] );
    else if( ! strcmp( argv[i], "--preset" ) && i + 1 < argc )
      { i++; preset = ! strcmp( argv[i], "unprocessed" ) ? AAUDIO_INPUT_PRESET_UNPROCESSED : ! strcmp( argv[i], "camcorder" ) ? AAUDIO_INPUT_PRESET_CAMCORDER :
                      ! strcmp( argv[i], "voice" ) ? AAUDIO_INPUT_PRESET_VOICE_RECOGNITION : AAUDIO_INPUT_PRESET_GENERIC; }
    else if( ! dir ) dir = argv[i];
    }
  if( ! dir ) { fprintf( stderr, "usage: a5droid DIR [--secs N] [--latency FRAMES] [--preset voice|unprocessed|camcorder|generic]\n" ); return 2; }

  char err[ASP_ERR], tp[1100]; struct stat st;
  snprintf( tp, sizeof tp, "%s/loop.d64", dir );
  int fresh = stat( dir, &st ) != 0;
  AspStore *s = fresh ? asp_create( dir, "{\"name\":\"phone\"}", err ) : asp_open( dir, 0, err );
  if( ! s ) { fprintf( stderr, "a5droid: %s\n", err ); return 1; }
  AspTank *t = fresh ? asp_tank_create( tp, 48000 * 12, 48000, err, sizeof err ) : asp_tank_open( tp, err, sizeof err );
  if( ! t ) { fprintf( stderr, "a5droid: %s\n", err ); return 1; }
  if( fresh ) asp_tank_bind( t, asp_session_id( s ) );
  AspCoreCfg cfg; asp_core_defaults( &cfg ); cfg.declared_latency = latency;
  AspCore *c = asp_core_open( s, t, &cfg, err, sizeof err );
  if( ! c ) { fprintf( stderr, "a5droid: %s\n", err ); return 1; }

  ring_init( &inr ); ring_init( &outr );
  signal( SIGINT, on_signal ); signal( SIGTERM, on_signal );
  AAudioStream *po = open_stream( 0, 0, on_play ), *pi = po ? open_stream( 1, preset, on_capture ) : 0;
  if( ! pi ) { asp_core_close( c ); asp_tank_close( t ); asp_close( s ); return 1; }
  AAudioStream_requestStart( pi ); AAudioStream_requestStart( po );
  fprintf( stderr, "a5droid: running in %s, round trip declared %ld frames; Ctrl-C or --secs to stop\n", dir, latency );

  static int32_t in[2 * ASP_TICK], out[2 * ASP_TICK];
  struct timespec t0; clock_gettime( CLOCK_MONOTONIC, &t0 );
  long told_d = 0, told_s = 0; int ticks = 0, drain = 0, drain_ticks = 0; struct timespec nap = { 0, 5000000 };
  for( ;; )
    {
    if( ( atomic_load( &quit ) || ( secs && ticks >= secs * 5 ) ) && ! drain ) { asp_core_drain( c ); drain = 1; }
    if( drain && ( asp_core_moves( c ) == 0 || ++drain_ticks > 150 ) ) break;      /* moves cannot finish while the core hunts the sync: give up after 30 s, they resume at the next open */
    if( ring_used( &inr ) < ASP_TICK && ! drain ) { nanosleep( &nap, 0 ); continue; }
    if( drain ) memset( in, 0, sizeof in ); else ring_get( &inr, in, ASP_TICK );
    long d = atomic_load( &drops ), sv = atomic_load( &starves );
    if( d != told_d ) { asp_core_edge( c, 0, d - told_d ); fprintf( stderr, "a5droid: dropped %ld frames of input\n", d - told_d ); told_d = d; }
    if( sv != told_s ) { asp_core_edge( c, 1, sv - told_s ); fprintf( stderr, "a5droid: starved %ld frames of output\n", sv - told_s ); told_s = sv; }
    asp_core_tick( c, in, out );
    if( ! drain && ! atomic_load( &flowing ) )
      { static int32_t cushion[2 * 4800];     /* 100 ms of silence in front of the first tick: the two device clocks drift, and a starve makes the core hunt the sync again */
        ring_put( &outr, cushion, 4800 ); }
    if( ! drain ) { if( RING_FRAMES - ring_used( &outr ) >= ASP_TICK ) ring_put( &outr, out, ASP_TICK ); atomic_store( &flowing, 1 ); }
    if( ! drain && ticks > 0 && ticks % 300 == 0 )
      {                                     /* a minute: how fast did each device run against the monotonic clock, in Hz (48000 is exact) */
      struct timespec t1; clock_gettime( CLOCK_MONOTONIC, &t1 );
      static double pel = 0; static long pcap = 0, pplay = 0;      /* the minute just gone, not the whole run: the start-up of each stream is not a rate */
      double el = ( t1.tv_sec - t0.tv_sec ) + ( t1.tv_nsec - t0.tv_nsec ) / 1e9;
      long cf = atomic_load( &capframes ), pf = atomic_load( &playframes );
      double cr = ( cf - pcap ) / ( el - pel ), pr = ( pf - pplay ) / ( el - pel );
      pel = el; pcap = cf; pplay = pf;
      fprintf( stderr, "a5droid: DRIFT at %.0fs, last minute: capture %.2f Hz play %.2f Hz difference %.1f ppm, output ring %llu frames, drops %ld starves %ld\n", el, cr, pr, ( cr - pr ) / 48000.0 * 1e6,
               (unsigned long long)ring_used( &outr ), atomic_load( &drops ), atomic_load( &starves ) );
      }
    if( ++ticks % 5 == 0 && ticks % 50 == 0 )
      {
      AspCoreStatus ps; asp_core_status( c, &ps );
      fprintf( stderr, "a5droid: %3ds %s in %4.0f dB out %4.0f dB takes %d%s load %.0f%% round trip %lld frames%s\n", ticks / 5, ps.phase ? "play" : "sync", ps.in_db[0], ps.out_db[0], ps.loop_takes, ps.take_open ? " (recording)" : "", ps.load, (long long)ps.latency, ps.phase ? "" : " (hunting: tone level shift below the loudest shown next)" );
      { float pp = atomic_exchange( &playpeak, 0 ), cp = atomic_exchange( &cappeak, 0 );
        fprintf( stderr, "a5droid:     devices: speaker side peak %.1f dB, mic side peak %.1f dB%s\n", 20 * log10f( pp + 1e-9f ), 20 * log10f( cp + 1e-9f ), ps.phase ? "" : ", hunting" ); }
      if( ! ps.phase ) fprintf( stderr, "a5droid:     sync tone shifted %d bits down\n", ps.sync_shift );
      }
    }
  AAudioStream_requestStop( pi ); AAudioStream_requestStop( po );
  AAudioStream_close( pi ); AAudioStream_close( po );
  asp_core_close( c ); asp_tank_close( t ); asp_close( s );
  fprintf( stderr, "a5droid: done\n" );
  return 0;
  }
