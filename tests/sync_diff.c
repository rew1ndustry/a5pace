#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sync.h"

void ref_put( char *d, int sec );
int64_t ref_find( char *d, int sec );

static unsigned rs = 12345;
static int rnd( void ) { rs = rs * 1664525u + 1013904223u; return (int)( rs >> 8 ) & 0xffff; }

static long bad, cases, found;

static void same( const int32_t *in, int sec, const char *what )
  {
  static int32_t copy[2 * ASP_SYNC_FRAMES];
  memcpy( copy, in, sizeof copy );
  int64_t a = asp_sync_find_asp4( in, sec ), b = ref_find( (char *)copy, sec );
  cases++; if( a ) found++;
  if( a != b ) { bad++; if( bad < 6 ) printf( "DIFF %s second %d: %lld vs ref %lld\n", what, sec, (long long)a, (long long)b ); }
  }

int main( int argc, char **argv )
  {
  static int32_t o[2 * ASP_SYNC_FRAMES], r[2 * ASP_SYNC_FRAMES], in[2 * ASP_SYNC_FRAMES];
  for( int s = 0; s < 8; s++ )
    {
    asp_sync_out_asp4( o, s ); ref_put( (char *)r, s );
    if( memcmp( o, r, sizeof o ) ) { bad++; printf( "DIFF output second %d\n", s ); }
    cases++;
    }
  static const int delay[] = { 0, 3, 100, 5000, 20000, 47000 };
  static const double gain[] = { 0.5, 0.05, 0.003 };
  for( int s = 0; s < 8; s++ )
    {
    asp_sync_out_asp4( o, s );
    for( unsigned d = 0; d < sizeof delay / sizeof *delay; d++ )
      for( unsigned g = 0; g < sizeof gain / sizeof *gain; g++ )
        for( int inv = 0; inv < 2; inv++ )
          for( int nz = 0; nz < 2; nz++ )
            {
            memset( in, 0, sizeof in );
            for( int n = 0; n + delay[d] < ASP_SYNC_FRAMES; n++ )
              for( int c = 0; c < 2; c++ )
                {
                double v = o[2*n+c] * gain[g] * ( inv ? -1 : 1 );
                if( nz ) v += ( rnd() - 32768 ) * 2000.0;
                in[2*( n + delay[d] ) + c] = (int32_t)v;
                }
            same( in, s + 1, "synthetic" );
            }
    }
  for( int a = 1; a < argc; a++ )
    {
    FILE *f = fopen( argv[a], "rb" );
    if( ! f ) { perror( argv[a] ); return 1; }
    memset( in, 0, sizeof in );
    size_t n = fread( in, 4, 2 * ASP_SYNC_FRAMES, f ); fclose( f );
    if( n < 2 * ASP_SYNC_FRAMES ) printf( "%s: short (%zu samples), padded\n", argv[a], n );
    for( int s = 0; s < 8; s++ ) same( in, s, argv[a] );
    }
  printf( "%ld cases compared (%ld decoded), %ld differences\n", cases, found, bad );
  return bad != 0;
  }
