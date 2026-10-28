/* tickbench: where does a tick go? times the parts of a tick on this machine (made for the cubieboard).
     tickbench [ticks]      each part over many ticks of noise; then a whole core with takes coming and going */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "aspcore.h"
#include "gate.h"
#include "asptank.h"

static double now_ms( void ) { struct timespec t; clock_gettime( CLOCK_MONOTONIC, &t ); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static unsigned rnd = 12345;
static int32_t noise( int amp ) { rnd = rnd * 1664525u + 1013904223u; return (int32_t)( ( (int64_t)( rnd >> 8 ) - 8388608 ) * amp / 8388608 ); }

typedef struct { double sum, max; int n; } Stat;
static void add( Stat *s, double ms ) { s->sum += ms; s->n++; if( ms > s->max ) s->max = ms; }
static void show( const char *what, const Stat *s ) { printf( "  %-34s avg %7.2f ms   max %7.2f ms   (%d)\n", what, s->n ? s->sum / s->n : 0, s->max, s->n ); }

int main( int argc, char **argv )
  {
  int ticks = argc > 1 ? atoi( argv[1] ) : 100;
  static int32_t pcm[2 * ASP_TICK], prev[2 * ASP_TICK], out[2 * ASP_TICK];
  static int64_t mix[2 * ASP_TICK];
  char err[256];
  printf( "one tick is %.0f ms of audio\n", ASP_TICK * 1000.0 / 48000 );

  AspGate *g = asp_gate_new( NULL, NULL );
  asp_gate_set( g, 18, 7.5 );
  Stat sw = { 0 }, sc = { 0 };
  for( int t = 0; t < ticks; t++ )
    {
    for( int i = 0; i < 2 * ASP_TICK; i++ ) pcm[i] = noise( 40000 );
    double a = now_ms(); asp_gate_switch( g, pcm, prev, ASP_TICK, 1 ); double b = now_ms(); asp_gate_scan( g, pcm, ASP_TICK ); double c = now_ms();
    add( &sw, b - a ); add( &sc, c - b );
    memcpy( prev, pcm, sizeof prev );
    }
  show( "gate switch (dc filter, per sample)", &sw );
  show( "gate scan (frames, fft)", &sc );

  char path[] = "/dev/shm/tickbench.d64";
  unlink( path );
  AspTank *tk = asp_tank_create( path, 48000 * 20, 48000, err, sizeof err );
  Stat tg = { 0 }, tp = { 0 }, mo = { 0 };
  memset( mix, 0, sizeof mix );
  for( int t = 0; t < ticks; t++ )
    {
    double a = now_ms(); asp_tank_get( tk, (int64_t)t * ASP_TICK, ASP_TICK, mix ); double b = now_ms();
    asp_tank_put( tk, (int64_t)t * ASP_TICK, ASP_TICK, mix ); double c = now_ms();
    int sh = 0; asp_mix_out( 1 << 20, out, ASP_TICK, mix, 0, &sh ); double d = now_ms();
    add( &tg, b - a ); add( &tp, c - b ); add( &mo, d - c );
    }
  show( "tank get (one tick)", &tg );
  show( "tank put (one tick)", &tp );
  show( "mix to output (one tick)", &mo );
  asp_tank_close( tk );
  unlink( path );
  return 0;
  }
