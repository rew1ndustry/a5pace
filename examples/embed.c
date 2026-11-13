/* embed: the smallest host of the core, linked against libaspace.a only (no runner, no control port, no audio device).
   it is what a phone app's worker thread does: open a space, tick it with audio, drain, close. here the audio is a
   generated tone with a loud burst, played for N seconds, and the round trip is declared (no sync tone to hunt).
     embed DIR [SECONDS]      DIR is made if it is not a space yet. prints the status once a second.
   in a real host `in` comes from a ring the capture callback fills and `out` goes to a ring the playback callback empties. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aspace.h"

int main( int argc, char **argv )
  {
  if( argc < 2 ) { fprintf( stderr, "usage: embed DIR [SECONDS]\n" ); return 2; }
  const char *dir = argv[1];
  int secs = argc > 2 ? atoi( argv[2] ) : 3;
  char err[ASP_ERR], tp[1100];
  snprintf( tp, sizeof tp, "%s/loop.d64", dir );
  struct stat st;
  int fresh = stat( dir, &st ) != 0;
  AspStore *s = fresh ? asp_create( dir, "{\"name\":\"embed\"}", err ) : asp_open( dir, 0, err );
  if( ! s ) { fprintf( stderr, "embed: %s\n", err ); return 1; }
  AspTank *t = fresh ? asp_tank_create( tp, 48000 * 10, 48000, err, sizeof err ) : asp_tank_open( tp, err, sizeof err );
  if( ! t ) { fprintf( stderr, "embed: %s\n", err ); return 1; }
  if( fresh ) asp_tank_bind( t, asp_session_id( s ) );
  AspCoreCfg cfg; asp_core_defaults( &cfg );
  cfg.declared_latency = 4800;                       /* a host that knows its own round trip skips the sync hunt */
  AspCore *c = asp_core_open( s, t, &cfg, err, sizeof err );
  if( ! c ) { fprintf( stderr, "embed: %s\n", err ); return 1; }

  static int32_t in[2 * ASP_TICK], out[2 * ASP_TICK];
  for( int tick = 0; tick < secs * 5; tick++ )
    {
    for( int i = 0; i < ASP_TICK; i++ )
      {
      long n = (long)tick * ASP_TICK + i;
      double v = tick >= 10 && tick < 15 ? 0.2 * sin( 2 * M_PI * 440 * n / 48000.0 ) : 0.0;   /* a second of tone, at 2 s */
      in[2 * i] = in[2 * i + 1] = (int32_t)( v * 2147483647.0 );
      }
    asp_core_tick( c, in, out );                   /* a real host does this on its worker thread, paced by the audio */
    if( tick % 5 == 4 )
      {
      AspCoreStatus ps; asp_core_status( c, &ps );
      printf( "t=%ds phase=%s takes in loop=%d in=%.0f dB out=%.0f dB\n", tick / 5 + 1, ps.phase ? "play" : "sync", ps.loop_takes, ps.in_db[0], ps.out_db[0] );
      }
    }
  asp_core_drain( c );
  while( asp_core_moves( c ) > 0 ) asp_core_tick( c, in, out );
  asp_core_close( c ); asp_tank_close( t ); asp_close( s );
  return 0;
  }
