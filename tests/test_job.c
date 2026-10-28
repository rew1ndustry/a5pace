#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include "aspengine.h"
#include "takefile.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

#define LOOP 20011
static int64_t model[LOOP * 2];

static void wr( const char *path, const int32_t *p, int64_t n, int ch )
  {
  FILE *f = fopen( path, "wb" );
  for( int64_t i = 0; i < n * ch; i++ ) { unsigned char b[4]; uint32_t v = (uint32_t)p[i]; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; fwrite( b, 1, 4, f ); }
  fclose( f );
  }
static int32_t *mk( int64_t n, int ch, unsigned seed )
  {
  int32_t *p = malloc( (size_t)n * ch * 4 ); unsigned x = seed;
  for( int64_t i = 0; i < n * ch; i++ ) { x = x * 1664525u + 1013904223u; p[i] = (int32_t)( x ^ ( x << 7 ) ); }
  return p;
  }
static int64_t lv( int64_t x, int lev ) { return lev == 99 ? 0 : lev >= 0 ? x >> lev : x * ( 1 << -lev ); }
static void mput( const int32_t *p, int64_t n, int ch, int64_t pos0, int sign, AspPlace pl )
  {
  for( int64_t i = 0; i < n; i++ )
    {
    int64_t a = p[i*ch], b = ch > 1 ? p[i*ch+1] : p[i*ch], s0 = pl.flip ? b : a, s1 = pl.flip ? a : b, at = ( pos0 + i ) % LOOP;
    model[2*at] += sign * lv( s0, pl.l ); model[2*at+1] += sign * lv( s1, pl.r );
    }
  }
static int32_t hv( int64_t n )                            /* a sample that can be had again from its number, so a long take needs no buffer */
  { uint64_t x = (uint64_t)n * 0x9E3779B97F4A7C15ull; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32; return (int32_t)x; }
static int same( AspTank *t )
  { static int64_t g[LOOP * 2]; return ! asp_tank_get( t, 0, LOOP, g ) && ! memcmp( g, model, sizeof model ); }

int main( void )
  {
  char dir[] = "/tmp/aspjob_testXXXXXX", p[300], e[ASP_ERR], cmd[600], f1[300], f2[300];
  if( ! mkdtemp( dir ) ) return 1;
  { char x[300]; snprintf( x, sizeof x, "%s/cfg", dir ); setenv( "XDG_CONFIG_HOME", x, 1 ); }
  snprintf( p, sizeof p, "%s/s", dir );
  AspStore *s = asp_create( p, "{\"name\":\"j\"}", e );
  snprintf( p, sizeof p, "%s/l.d64", dir );
  AspTank *t = asp_tank_create( p, LOOP, 48000, e, sizeof e );
  snprintf( f1, sizeof f1, "%s/a.d32", dir ); snprintf( f2, sizeof f2, "%s/b.d32", dir );
  int32_t *S = mk( 30000, 2, 5 );              /* a stereo take, longer than the loop */
  wr( f1, S, 30000, 2 );
  AspTake a; CHECK( asp_take_add_d32( s, 1, 0, f1, 2, 48000, "off", 0, &a ) == 0 );
  int32_t *M = mk( 9000, 1, 6 );               /* a mono take */
  wr( f2, M, 9000, 1 );
  AspTake b; CHECK( asp_take_add_d32( s, 1, 1, f2, 1, 48000, "off", 0, &b ) == 0 );
  CHECK( b.channels == 1 );

  AspPlace OFF = ASP_PLACE_OFF, st00 = { 0, 0, 0 }, sw = { 2, 1, 1 }, c11 = { 1, 1, 0 }, hl = { 0, ASP_LEV_OFF, 0 }, hr = { ASP_LEV_OFF, 0, 0 };

  CHECK( asp_move_take( t, s, a.id, -1, 0, -1, OFF, st00 ) == 0 );   mput( S, 30000, 2, 0, 1, st00 );     CHECK( same( t ) );
  CHECK( asp_move_take( t, s, a.id, -1, 0, -1, st00, sw ) == 0 );    mput( S, 30000, 2, 0, -1, st00 ); mput( S, 30000, 2, 0, 1, sw );   CHECK( same( t ) );   /* swapped channels, new levels */
  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, OFF, c11 ) == 0 );    mput( M, 9000, 1, 48000, 1, c11 );   CHECK( same( t ) );   /* mono in the middle */
  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, c11, hl ) == 0 );     mput( M, 9000, 1, 48000, -1, c11 ); mput( M, 9000, 1, 48000, 1, hl ); CHECK( same( t ) );
  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, hl, hr ) == 0 );      mput( M, 9000, 1, 48000, -1, hl ); mput( M, 9000, 1, 48000, 1, hr ); CHECK( same( t ) );

  /* a conversion: the stereo take was fed live as stereo, and is replaced by its left channel as mono, in slices */
  {
  AspJob *j = asp_job_take( s, a.id, 0, -1, sw, -1, c11, 0 );
  int steps = 0, rc;
  while( ! ( rc = asp_job_step( j, t, 7000 ) ) ) steps++;
  CHECK( rc == 1 && steps >= 2 );                                      /* it took several slices */
  asp_job_free( j );
  mput( S, 30000, 2, 0, -1, sw );
  int32_t *ml = malloc( 30000 * 4 ); for( int i = 0; i < 30000; i++ ) ml[i] = S[2*i];
  mput( ml, 30000, 1, 0, 1, c11 ); free( ml );
  CHECK( same( t ) );
  }

  /* an explicit mix: gain and pan in whole dB / percent, exact to take out again; a stereo take's pan does nothing */
  {
  AspPlace mx = { 0, 0, 0, -6, 50, 1 }, mx2 = { 0, 0, 0, 3, -100, 1 };
  int64_t ml, mr, ms[2];
  asp_place_mult( (AspPlace){ 0, 0, 0, 0, 0, 1 }, 1, &ml, &mr );  CHECK( ml == ( 1 << 23 ) && mr == ml );          /* centre = half each = level shift 1 */
  asp_place_mult( (AspPlace){ 0, 0, 0, 0, -100, 1 }, 1, &ml, &mr ); CHECK( ml == ( 1 << 24 ) && mr == 0 );
  asp_place_mult( (AspPlace){ 0, 0, 0, 0, 80, 1 }, 0, &ml, &mr );   CHECK( ml == ( 1 << 24 ) && mr == ml );
  { int g, p; asp_place_to_mix( hl, 1, &g, &p ); CHECK( g == 0 && p == -100 ); asp_place_to_mix( c11, 1, &g, &p ); CHECK( g == 0 && p == 0 ); }
  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, hr, mx ) == 0 );
  int32_t *bm = malloc( 30000 * 4 ); for( int i = 0; i < 30000; i++ ) bm[i] = S[2*i];     /* a is, since the conversion, its left channel as mono */
  memset( model, 0, sizeof model ); mput( bm, 30000, 1, 0, 1, c11 );
  asp_place_mult( mx, 1, &ms[0], &ms[1] );
  for( int i = 0; i < 9000; i++ ) { int64_t at = ( 48000 + i ) % LOOP; model[2*at] += asp_gain( M[i], ms[0] ); model[2*at+1] += asp_gain( M[i], ms[1] ); }
  CHECK( same( t ) );
  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, mx, mx2 ) == 0 );  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, mx2, ASP_PLACE_OFF ) == 0 );
  memset( model, 0, sizeof model ); mput( bm, 30000, 1, 0, 1, c11 );  CHECK( same( t ) );
  CHECK( asp_move_take( t, s, b.id, -1, 0, -1, ASP_PLACE_OFF, hr ) == 0 ); free( bm );
  }


  /* limits: a job stopped part way, moved on; a part of a take out again */
  {
  AspPlace st = { 0, 0, 0 };
  asp_tank_clear( t ); memset( model, 0, sizeof model );
  AspJob *j = asp_job_take( s, a.id, 0, -1, OFF, -1, st, -1 );
  CHECK( j && asp_job_retarget( j, st ) == 0 );                       /* not begun: may still be sent elsewhere */
  CHECK( asp_job_step_to( j, t, INT64_MAX, 10000 ) == 0 && asp_job_cursor( j ) == 10000 );
  mput( S, 10000, 2, 0, 1, st );  CHECK( same( t ) );                 /* exactly the first 10000 frames */
  CHECK( asp_job_retarget( j, st ) == -1 );                           /* begun: no */
  int rc; while( ! ( rc = asp_job_step( j, t, 3333 ) ) ) {}
  CHECK( rc == 1 && asp_job_cursor( j ) == 30000 );
  asp_job_free( j );
  memset( model, 0, sizeof model ); mput( S, 30000, 2, 0, 1, st );  CHECK( same( t ) );
  CHECK( asp_move_take( t, s, a.id, -1, 100, 5000, st, OFF ) == 0 );  /* frames 100 .. 5000 only */
  mput( S + 200, 4900, 2, 100, -1, st );  CHECK( same( t ) );
  CHECK( asp_move_take( t, s, a.id, -1, 100, 5000, OFF, st ) == 0 );
  mput( S + 200, 4900, 2, 100, 1, st );  CHECK( same( t ) );
  asp_move_take( t, s, a.id, -1, 0, -1, st, OFF );
  memset( model, 0, sizeof model ); CHECK( same( t ) );
  }

  /* what cannot be moved says so: a channel that is not there, a take that never ended, one cut short, one damaged */
  {
  AspPlace st = { 0, 0, 0 };
  char part[300], tp[300]; AspTake c; unsigned char art[ASP_ARTIST];
  asp_artist_local( art, 1 );
  CHECK( asp_job_take( s, a.id, 0, -1, OFF, 2, st, -1 ) == NULL );
  snprintf( part, sizeof part, "%s/unfinished.part", dir );
  AspTakeW *w = asp_tw_open( part, art, 2, 48000, 0 ); asp_tw_put( w, S, 9600 ); asp_tw_abort( w );
  CHECK( asp_job_open( part, 0, 0, -1, OFF, -1, st, -1 ) == NULL );
  int32_t *C = mk( 30000, 2, 9 ); wr( f1, C, 30000, 2 );
  CHECK( asp_take_add_d32( s, 3, 0, f1, 2, 48000, "off", 0, &c ) == 0 && asp_take_path( s, &c, tp, sizeof tp ) == 0 );
  { char sz[300]; snprintf( sz, sizeof sz, "%s.cut", tp ); snprintf( cmd, sizeof cmd, "cp %s %s && truncate -s 60000 %s", tp, sz, sz );
    if( system( cmd ) ) {}
    AspJob *j = asp_job_open( sz, 0, 0, -1, OFF, -1, st, -1 );
    CHECK( j && asp_job_step( j, t, INT64_MAX ) == -1 ); asp_job_free( j ); }
  { char sz[300]; snprintf( sz, sizeof sz, "%s.bad", tp ); snprintf( cmd, sizeof cmd, "cp %s %s && printf 'XXXXXXXX' | dd of=%s bs=1 seek=40000 conv=notrunc 2>/dev/null", tp, sz, sz );
    if( system( cmd ) ) {}
    AspJob *j = asp_job_open( sz, 0, 0, -1, OFF, -1, st, -1 );
    CHECK( j && asp_job_step( j, t, INT64_MAX ) == -1 ); asp_job_free( j ); }
  free( C );
  asp_tank_clear( t );
  }

  /* a take of three minutes goes through without being held: the memory it took is a block, not the take */
  {
  AspPlace st = { 0, 0, 0 };
  const int64_t N = 48000LL * 180;
  char big[300]; snprintf( big, sizeof big, "%s/big.d32", dir );
  FILE *f = fopen( big, "wb" );
  for( int64_t i = 0; i < N; i += 4096 )
    { unsigned char b[4096 * 8]; int64_t k = N - i < 4096 ? N - i : 4096;
      for( int64_t m = 0; m < k * 2; m++ ) { uint32_t v = (uint32_t)hv( i * 2 + m ); b[4*m] = v; b[4*m+1] = v >> 8; b[4*m+2] = v >> 16; b[4*m+3] = v >> 24; }
      fwrite( b, 8, k, f ); }
  fclose( f );
  AspTake bg; CHECK( asp_take_add_d32( s, 4, 0, big, 2, 48000, "off", 0, &bg ) == 0 );
  unlink( big );
  struct rusage ru; getrusage( RUSAGE_SELF, &ru ); long before = ru.ru_maxrss;
  asp_tank_clear( t ); memset( model, 0, sizeof model );
  CHECK( asp_move_take( t, s, bg.id, -1, 0, -1, OFF, st ) == 0 );
  getrusage( RUSAGE_SELF, &ru );
  CHECK( ru.ru_maxrss - before < 16 * 1024 );                         /* kb: the take is 66 MB decoded */
  for( int64_t i = 0; i < N; i++ ) { int64_t at = i % LOOP; model[2*at] += hv( i * 2 ); model[2*at+1] += hv( i * 2 + 1 ); }
  CHECK( same( t ) );                                                 /* and it came out exact, nine thousand times over the loop */
  CHECK( asp_move_take( t, s, bg.id, -1, 0, -1, st, OFF ) == 0 );
  memset( model, 0, sizeof model ); CHECK( same( t ) );              /* and out again, to nothing */
  }

  /* rebuild from state: flip and mono follow the log */
  asp_take_set_state( s, a.id, "{\"status\":\"mix\",\"lev\":[\"2\",\"1\"],\"flip\":1,\"gone\":0}", "t", 1 );
  asp_take_set_state( s, b.id, "{\"status\":\"mix\",\"lev\":[\"1\",\"1\"],\"mono\":1}", "t", 2 );
  memset( model, 0, sizeof model );
  mput( S, 30000, 2, 0, 1, sw ); mput( M, 9000, 1, 48000, 1, c11 );
  CHECK( asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  int64_t pk; CHECK( asp_take_peak( s, b.id, -1, &pk ) == 0 && pk > 0 );
  AspTakeState ps; asp_parse_state( "{\"status\":\"mix\",\"lev\":[\"1\",\"-\"],\"flip\":1,\"peak\":777,\"len\":55}", &ps );
  CHECK( ps.l == 1 && ps.r == ASP_LEV_OFF && ps.flip == 1 && ps.peak == 777 && ps.len == 55 );

  asp_tank_close( t ); asp_close( s ); free( S ); free( M );
  snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); if( system( cmd ) ) {}
  printf( fails ? "%d FAILED\n" : "all job tests passed\n", fails );
  return fails != 0;
  }
