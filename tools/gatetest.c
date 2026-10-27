/* gatetest [-g margin] [-r ring] LEARN.d32 TAKE.d32...
   learns the floor from the quietest 0.6 s of LEARN, then feeds each take tick by tick and says
   whether the gate finds sound in it. used to hold the ported gate to asp4's labelled takes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gate.h"
#include "asptank.h"
#include "plat.h"

static int32_t *load( const char *path, int64_t *frames )
  {
  FILE *f = fopen( path, "rb" );
  if( ! f ) return NULL;
  ASP_FSEEK( f, 0, SEEK_END );
  int64_t n = ASP_FTELL( f ) / 8;
  ASP_FSEEK( f, 0, SEEK_SET );
  unsigned char *b = malloc( (size_t)n * 8 );
  int32_t *p = malloc( (size_t)n * 8 );
  if( fread( b, 8, (size_t)n, f ) != (size_t)n ) { fclose( f ); return NULL; }
  fclose( f );
  for( int64_t i = 0; i < 2 * n; i++ ) p[i] = (int32_t)asp_ld32( b + 4 * i );
  free( b );
  *frames = n;
  return p;
  }

int main( int argc, char **argv )
  {
  double gm = 18, rg = 7.5, lc = 30; int strict = 0;
  int a = 1;
  for( ; a < argc && argv[a][0] == '-'; a += 2 )
    { if( argv[a][1] == 'g' ) gm = atof( argv[a+1] ); else if( argv[a][1] == 'r' ) rg = atof( argv[a+1] ); else if( argv[a][1] == 'l' ) lc = atof( argv[a+1] ); else if( argv[a][1] == 's' ) strict = atoi( argv[a+1] ); }
  if( argc - a < 2 ) { fprintf( stderr, "usage: gatetest [-g dB] [-r dB] LEARN.d32 TAKE.d32...\n" ); return 2; }
  int64_t ln;
  int32_t *L = load( argv[a], &ln );
  if( ! L ) { perror( argv[a] ); return 1; }
  const int64_t W = 28800;                       /* 0.6 s */
  double best = 1e300; int64_t at = 0;
  for( int64_t s = 0; s + W <= ln; s += 4800 )
    {
    double e = 0;
    for( int64_t i = 0; i < 2 * W; i += 7 ) e += (double)L[2*s+i] * L[2*s+i];
    if( e < best ) { best = e; at = s; }
    }
  for( a++; a < argc; a++ )
    {
    int64_t n; int32_t *T = load( argv[a], &n );
    if( ! T ) { perror( argv[a] ); continue; }
    AspGate *g = asp_gate_new( NULL, NULL );
    asp_gate_set( g, gm, rg ); asp_gate_set_lowcut( g, lc ); asp_gate_set_strict( g, strict );
    for( int64_t s = 0; s + ASP_TICK <= W; s += ASP_TICK )
      { int32_t buf[ASP_TICK * 2]; memcpy( buf, L + 2 * ( at + s ), sizeof buf ); asp_gate_scan( g, buf, ASP_TICK ); }
    int found = 0; int64_t first = -1;
    for( int64_t s = 0; s < n; s += ASP_TICK )
      {
      int32_t buf[ASP_TICK * 2] = { 0 };
      int64_t k = n - s < ASP_TICK ? n - s : ASP_TICK;
      memcpy( buf, T + 2 * s, (size_t)k * 8 );
      int64_t r = asp_gate_scan( g, buf, ASP_TICK );
      if( r < ASP_TICK ) { found = 1; if( first < 0 ) first = s + r; }
      }
    const char *base = strrchr( argv[a], '/' );
    printf( "%-14s %s%s\n", base ? base + 1 : argv[a], found ? "SOUND" : "quiet", found ? "" : "" );
    asp_gate_free( g ); free( T );
    }
  return 0;
  }
