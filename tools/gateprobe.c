/* gateprobe SESSION [-g dB] [-r dB] [-l Hz] [takeid...]  run takes from a session through the gate and say which rule opens it.
   the floor is learnt from the quietest 0.6 s of the take itself (its tail, after the event), then the take is
   replayed from its first sound. prints, per take: peak dBFS, when the gate first opens (ms from the first sound),
   by which rules, and how far above the learnt floor the opening tick's peak is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "aspengine.h"
#include "gate.h"

int main( int argc, char **argv )
  {
  char err[ASP_ERR];
  double gm = 18, rg = 7.5, lc = 30; int strict = 0;
  int a = 1;
  if( argc < 2 ) { fprintf( stderr, "usage: gateprobe SESSION [-g dB] [-r dB] [-l Hz] [takeid...]\n" ); return 2; }
  AspStore *s = asp_open( argv[a++], 1, err );
  if( ! s ) { fprintf( stderr, "%s\n", err ); return 1; }
  for( ; a + 1 < argc && argv[a][0] == '-'; a += 2 ) { if( argv[a][1] == 'g' ) gm = atof( argv[a+1] ); else if( argv[a][1] == 'r' ) rg = atof( argv[a+1] ); else if( argv[a][1] == 'l' ) lc = atof( argv[a+1] ); else if( argv[a][1] == 's' ) strict = atoi( argv[a+1] ); }
  for( int64_t id = 1; ; id++ )
    {
    AspTake k; char path[1200]; int32_t *p; int64_t n; int ch;
    if( asp_take_get( s, id, &k ) ) break;
    if( a < argc ) { int want = 0; for( int i = a; i < argc; i++ ) if( atoll( argv[i] ) == id ) want = 1; if( ! want ) continue; }
    asp_take_path( s, &k, path, sizeof path );
    if( asp_pcm_load( path, &p, &n, &ch ) ) continue;
    int32_t *st = malloc( (size_t)n * 8 );                    /* stereo from whatever it is */
    for( int64_t i = 0; i < n; i++ ) { st[2*i] = p[i*ch]; st[2*i+1] = p[i*ch+(ch>1)]; }
    free( p );
    int64_t first = 0; while( first < n && ! st[2*first] && ! st[2*first+1] ) first++;
    if( first >= n ) { free( st ); continue; }
    /* quietest 0.6 s window after the first sound */
    const int64_t W = 28800; double best = 1e300; int64_t at = first;
    for( int64_t t = first; t + W <= n; t += 4800 )
      { double e = 0; for( int64_t i = 0; i < 2 * W; i += 5 ) e += (double)st[2*t+i] * st[2*t+i]; if( e < best ) { best = e; at = t; } }
    if( n - first < 2 * W ) { free( st ); continue; }
    AspGate *g = asp_gate_new( NULL, NULL );
    asp_gate_set( g, gm, rg ); asp_gate_set_lowcut( g, lc ); asp_gate_set_strict( g, strict );
    for( int64_t t = 0; t + ASP_TICK <= W; t += ASP_TICK ) { int32_t b[ASP_TICK * 2]; memcpy( b, st + 2 * ( at + t ), sizeof b ); asp_gate_scan( g, b, ASP_TICK ); }
    asp_gate_why( g );
    double fl = asp_gate_floor( g, 0 ), fr = asp_gate_floor( g, 1 );
    int64_t pk = 0; for( int64_t i = 0; i < 2 * n; i++ ) { int64_t v = llabs( (int64_t)st[i] ); if( v > pk ) pk = v; }
    int64_t open = -1; int why = 0; double tickpk = 0;
    for( int64_t t = first; t + ASP_TICK <= n && open < 0; t += ASP_TICK )
      {
      int32_t b[ASP_TICK * 2]; memcpy( b, st + 2 * t, sizeof b );
      int64_t r = asp_gate_scan( g, b, ASP_TICK );
      if( r < ASP_TICK ) { open = t + r - first; why = asp_gate_why( g ); int64_t m = 0; for( int i = 0; i < 2 * ASP_TICK; i++ ) { int64_t v = llabs( (int64_t)b[i] ); if( v > m ) m = v; } tickpk = m; }
      }
    printf( "take %3lld  %5.1fs  peak %6.1f dBFS  floor rms %6.1f/%6.1f  ", (long long)id, (n - first) / 48000.0, 20 * log10( pk / 2147483648.0 + 1e-12 ),
            20 * log10( fl / 2147483648.0 + 1e-12 ), 20 * log10( fr / 2147483648.0 + 1e-12 ) );
    if( open < 0 ) printf( "gate does not open\n" );
    else printf( "opens at +%6.0f ms  rules %c%c%c%c  tick peak %6.1f dBFS (%+.0f dB over floor rms)\n", open * 1000.0 / 48000,
                 why & 1 ? 'C' : '-', why & 2 ? 'R' : '-', why & 4 ? 'B' : '-', why & 8 ? 'b' : '-',
                 20 * log10( tickpk / 2147483648.0 + 1e-12 ), 20 * log10( tickpk / ( ( fl > fr ? fl : fr ) + 1e-9 ) ) );
    asp_gate_free( g ); free( st );
    }
  asp_close( s );
  return 0;
  }
