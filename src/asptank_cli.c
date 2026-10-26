/* asptank rebuild SESSION_DIR OUT.d64 FRAMES [SEQ]   rebuild the loop from the store
   asptank diff A.d64 B.d64                           compare two loops */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspengine.h"

int main( int argc, char **argv )
  {
  char err[ASP_ERR];
  if( argc >= 5 && ! strcmp( argv[1], "rebuild" ) )
    {
    AspStore *s = asp_open( argv[2], 1, err );
    if( ! s ) { fprintf( stderr, "%s\n", err ); return 1; }
    AspTank *t = asp_tank_create( argv[3], atoll( argv[4] ), err, sizeof err );
    if( ! t ) { fprintf( stderr, "%s\n", err ); return 1; }
    int rc = asp_rebuild( t, s, argc > 5 ? atoll( argv[5] ) : -1 );
    asp_tank_close( t ); asp_close( s );
    return rc ? 1 : 0;
    }
  if( argc == 4 && ! strcmp( argv[1], "diff" ) )
    {
    AspTank *a = asp_tank_open( argv[2], err, sizeof err ), *b = asp_tank_open( argv[3], err, sizeof err );
    if( ! a || ! b ) { fprintf( stderr, "%s\n", err ); return 1; }
    int64_t n = asp_tank_frames( a ), bad = 0, first = -1, maxd = 0, pos;
    if( n != asp_tank_frames( b ) ) { printf( "lengths differ %lld %lld\n", (long long)n, (long long)asp_tank_frames( b ) ); return 1; }
    for( pos = 0; pos < n; pos += ASP_TICK )
      {
      int64_t x[ASP_TICK * 2], y[ASP_TICK * 2], k = n - pos < ASP_TICK ? n - pos : ASP_TICK, i;
      asp_tank_get( a, pos, k, x ); asp_tank_get( b, pos, k, y );
      for( i = 0; i < 2 * k; i++ )
        if( x[i] != y[i] )
          { int64_t d = x[i] > y[i] ? x[i] - y[i] : y[i] - x[i]; if( d > maxd ) maxd = d; if( first < 0 ) first = pos + i / 2; bad++; }
      }
    printf( "%lld of %lld samples differ, first at frame %lld, max diff %lld\n", (long long)bad, (long long)n * 2, (long long)first, (long long)maxd );
    return bad != 0;
    }
  fprintf( stderr, "usage: asptank rebuild DIR OUT.d64 FRAMES [SEQ] | diff A.d64 B.d64\n" );
  return 2;
  }
