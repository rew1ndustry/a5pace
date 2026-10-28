#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspengine.h"

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
  int32_t *old = malloc( 30000 * 8 ), *nw = malloc( 30000 * 4 );
  memcpy( old, S, 30000 * 8 );
  for( int i = 0; i < 30000; i++ ) nw[i] = S[2*i];
  AspJob *j = asp_job_new( 0, 0, -1, old, 30000, 2, sw, nw, 30000, 1, c11 );
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
