#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gate.h"
#include "asptank.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

static unsigned rs = 5;
static double rnd( void ) { rs = rs * 1664525u + 1013904223u; return ( (int)( rs >> 8 ) & 0xffff ) / 32768.0 - 1; }

/* learn a noise floor, then a burst of a sine; does the gate open? */
static int opens( double hz, double dbfs, double lowcut, int strict )
  {
  AspGate *g = asp_gate_new( NULL, NULL );
  asp_gate_set_lowcut( g, lowcut ); asp_gate_set_strict( g, strict );
  int32_t b[2 * ASP_TICK];
  int64_t t = 0;
  int found = 0;
  double amp = 2147483648.0 * pow( 10, dbfs / 20 ), nz = 2147483648.0 * pow( 10, -95.0 / 20 );
  for( int tick = 0; tick < 12; tick++ )
    {
    for( int i = 0; i < ASP_TICK; i++, t++ )
      {
      /* a smooth burst, 0.4 s: abrupt edges would be broadband clicks whatever the pitch */
      double w = tick >= 6 && tick < 8 ? 0.5 - 0.5 * cos( 2 * M_PI * ( t - 6 * ASP_TICK ) / ( 2.0 * ASP_TICK ) ) : 0;
      double burst = w * amp * sin( 2 * M_PI * hz * t / 48000.0 );
      for( int ch = 0; ch < 2; ch++ ) b[2*i+ch] = (int32_t)( rnd() * nz + burst );
      }
    if( asp_gate_scan( g, b, ASP_TICK ) < ASP_TICK && tick >= 6 ) found = 1;
    }
  asp_gate_free( g );
  return found;
  }

int main( void )
  {
  CHECK( opens( 440, -50, 30, 0 ) && opens( 440, -50, 190, 1 ) );          /* a note opens it, with or without the options */
  CHECK( opens( 80, -50, 30, 0 ) );                                         /* a bump at 80 Hz opens the old gate ... */
  CHECK( ! opens( 80, -50, 190, 0 ) && ! opens( 80, -50, 190, 1 ) );        /* ... and not one told the lowest note is 190 Hz */
  CHECK( opens( 80, -35, 30, 0 ) && ! opens( 80, -35, 190, 1 ) && ! opens( 120, -30, 190, 1 ) );   /* a loud bump: mostly below the range, so no */
  CHECK( opens( 440, -62, 190, 1 ) );                                       /* a quiet note still does */
  CHECK( opens( 2000, -55, 190, 1 ) );                                      /* and a high one */
  CHECK( ! opens( 440, -200, 190, 1 ) );                                    /* silence does not */
  printf( fails ? "%d FAILED\n" : "all gate tests passed\n", fails );
  return fails != 0;
  }
