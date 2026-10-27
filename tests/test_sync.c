#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sync.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

#define SECS 40
static int32_t *outs;                       /* everything played, SECS seconds */
static unsigned rs = 7;
static double rnd( void ) { rs = rs * 1664525u + 1013904223u; return ( (int)( rs >> 8 ) & 0xffff ) / 32768.0 - 1; }

typedef struct { int delay; double gain_db; int inv; double noise_db; int jitter; double clip_db; } Chan;

/* the loopback: input second n = output delayed, scaled, maybe inverted, with noise */
static int run( const Chan *c, AspSyncCfg *cfg, AspSyncResult *res, int verbose )
  {
  AspSync *s = asp_sync_new( cfg );
  static int32_t in[2 * ASP_SYNC_FRAMES];
  double g = pow( 10, c->gain_db / 20 ), nz = pow( 10, c->noise_db / 20 ) * 2147483648.0;
  double clip = c->clip_db ? pow( 10, c->clip_db / 20 ) * 2147483648.0 : 0;
  int rc = 0;
  for( int n = 0; n < SECS && rc == 0; n++ )
    {
    int32_t *o = outs + (size_t)n * 2 * ASP_SYNC_FRAMES;
    asp_sync_out( s, o );
    int d = c->delay + ( c->jitter ? ( n % 2 ? c->jitter : -c->jitter ) : 0 );
    for( int i = 0; i < ASP_SYNC_FRAMES; i++ )
      for( int ch = 0; ch < 2; ch++ )
        {
        int64_t t = (int64_t)n * ASP_SYNC_FRAMES + i - d;
        double v = 0;
        if( t >= 0 ) v = (double)outs[2 * t + ch] * g * ( c->inv ? -1 : 1 );
        v += rnd() * nz;
        if( clip && v > clip ) v = clip; else if( clip && v < -clip ) v = -clip;
        in[2*i+ch] = (int32_t)v;
        }
    rc = asp_sync_in( s, in );
    if( verbose ) printf( "  second %d level %.0f dB -> %d\n", n, asp_sync_level( s ), rc );
    }
  if( rc == 1 ) *res = *asp_sync_result( s );
  else { *res = *asp_sync_result( s ); }
  asp_sync_free( s );
  return rc;
  }

int main( void )
  {
  outs = malloc( (size_t)SECS * 2 * ASP_SYNC_FRAMES * 4 );
  AspSyncCfg cfg;
  AspSyncResult r;
  asp_sync_defaults( &cfg );

  /* clean path, assorted latencies, including more than a second and more than two */
  static const int delays[] = { 1, 5000, 47999, 60000, 130000 };   /* 0 is asp4's 'not found' */
  for( unsigned i = 0; i < sizeof delays / sizeof *delays; i++ )
    {
    Chan c = { delays[i], -20, 0, -300, 0, 0 };
    int rc = run( &c, &cfg, &r, 0 );
    CHECK( rc == 1 && r.frames == delays[i] );
    if( rc != 1 || r.frames != delays[i] ) printf( "delay %d -> rc %d frames %lld\n", delays[i], rc, (long long)r.frames );
    }

  /* inverted path, either channel */
  { Chan c = { 7777, -20, 1, -300, 0, 0 }; int rc = run( &c, &cfg, &r, 0 );
    CHECK( rc == 1 && r.frames == 7777 && r.inverted == 1 ); }
  { Chan c = { 7777, -20, 0, -300, 0, 0 }; int rc = run( &c, &cfg, &r, 0 );
    CHECK( rc == 1 && r.frames == 7777 && r.inverted == 0 ); }

  /* quiet returns: the output rises until it is heard, then trims to the target */
  for( double gdb = -70; gdb <= 0; gdb += 10 )
    {
    Chan c = { 12345, gdb, 0, -300, 0, 0 };
    int rc = run( &c, &cfg, &r, 0 );
    CHECK( rc == 1 && r.frames == 12345 );
    if( rc == 1 && gdb > -48 ) CHECK( fabs( r.in_peak_db - cfg.target_db ) < 3.5 || r.out_db >= cfg.max_db );
    if( rc != 1 ) printf( "gain %.0f dB: rc %d\n", gdb, rc );
    }

  /* a hot return that clips: the level comes down, not up */
  { Chan c = { 4000, 6, 0, -300, 0, -0.5 }; int rc = run( &c, &cfg, &r, 0 );
    CHECK( rc == 1 && abs( (int)r.frames - 4000 ) <= 1 && r.in_peak_db < -6 ); }

  /* noisy path */
  { Chan c = { 9000, -30, 0, -60, 0, 0 }; int rc = run( &c, &cfg, &r, 0 );
    if( rc != 1 || abs( (int)r.frames - 9000 ) > 1 ) printf( "noisy: rc %d frames %lld spread %d level %.1f peak %.1f\n", rc, (long long)r.frames, r.spread, r.out_db, r.in_peak_db );
    CHECK( rc == 1 && abs( (int)r.frames - 9000 ) <= 1 ); }      /* noise moves a crossing by a sample at most */

  /* jitter of one sample is steady enough (tol 2), five is not */
  { Chan c = { 9000, -20, 0, -90, 1, 0 }; int rc = run( &c, &cfg, &r, 0 ); CHECK( rc == 1 && abs( (int)r.frames - 9000 ) <= 1 ); }
  { Chan c = { 9000, -20, 0, -90, 5, 0 }; AspSyncCfg g = cfg; g.give_up = 25; int rc = run( &c, &g, &r, 0 ); CHECK( rc == -1 && r.spread > cfg.tol ); }

  /* nothing comes back */
  { Chan c = { 9000, -200, 0, -200, 0, 0 }; AspSyncCfg g = cfg; g.give_up = 20; int rc = run( &c, &g, &r, 0 ); CHECK( rc == -1 ); }

  /* output muted by the shell: starts at the top level, no ramp */
  { AspSyncCfg m = cfg; m.output_muted = 1; AspSync *s = asp_sync_new( &m );
    CHECK( asp_sync_level( s ) == m.max_db ); asp_sync_free( s );
    AspSync *q = asp_sync_new( &cfg ); CHECK( asp_sync_level( q ) == cfg.start_db ); asp_sync_free( q ); }

  /* it never exceeds max_db, whatever the path */
  { AspSyncCfg lo = cfg; lo.max_db = -24; Chan c = { 500, -40, 0, -100, 0, 0 };
    AspSync *s = asp_sync_new( &lo ); int32_t *o = outs;
    for( int n = 0; n < 20; n++ ) { asp_sync_out( s, o ); static int32_t z[2 * ASP_SYNC_FRAMES]; asp_sync_in( s, z ); CHECK( asp_sync_level( s ) <= -24 ); }
    asp_sync_free( s ); (void)c; }

  free( outs );
  printf( fails ? "%d FAILED\n" : "all sync tests passed\n", fails );
  return fails != 0;
  }
