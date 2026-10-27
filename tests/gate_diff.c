#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gate.h"
#include "asptank.h"
#include "plat.h"

void ref_reset( double gate, double ring );
int64_t ref_scan( int32_t *p, int n );
void ref_switch( int32_t *p, int32_t *prev, int n, int armed );

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
  static const double cfg[][2] = { { 18, 7.5 }, { 12, 6 }, { 30, 9 } };
  long ticks = 0, hits = 0, bad = 0;
  for( int a = 1; a < argc; a++ )
    {
    int64_t n;
    int32_t *T = load( argv[a], &n );
    if( ! T ) { perror( argv[a] ); return 1; }
    for( unsigned c = 0; c < sizeof cfg / sizeof cfg[0]; c++ )
      {
      AspGate *g = asp_gate_new( NULL, NULL );
      asp_gate_set( g, cfg[c][0], cfg[c][1] );
      ref_reset( cfg[c][0], cfg[c][1] );
      int32_t pa[ASP_TICK * 2], pb[ASP_TICK * 2], qa[ASP_TICK * 2], qb[ASP_TICK * 2];
      int have = 0;
      for( int64_t s = 0, k = 0; s < n; s += ASP_TICK, k++ )
        {
        int64_t m = n - s < ASP_TICK ? n - s : ASP_TICK;
        memset( pa, 0, sizeof pa );
        memcpy( pa, T + 2 * s, (size_t)m * 8 );
        memcpy( pb, pa, sizeof pa );
        int armed = ( k % 3 ) != 2;
        asp_gate_switch( g, pa, have ? qa : NULL, ASP_TICK, armed );
        ref_switch( pb, have ? qb : NULL, ASP_TICK, armed );
        int64_t ra = asp_gate_scan( g, pa, ASP_TICK ), rb = ref_scan( pb, ASP_TICK );
        ticks++;
        if( ra < ASP_TICK ) hits++;
        if( ra != rb || memcmp( pa, pb, sizeof pa ) )
          { if( bad++ < 5 ) printf( "DIFF %s cfg %u tick %lld: %lld vs ref %lld\n", argv[a], c, (long long)k, (long long)ra, (long long)rb ); }
        memcpy( qa, pa, sizeof pa ); memcpy( qb, pb, sizeof pb ); have = 1;
        }
      asp_gate_free( g );
      }
    free( T );
    }
  printf( "%ld ticks compared (%ld with sound found), %ld differences\n", ticks, hits, bad );
  return bad != 0;
  }
