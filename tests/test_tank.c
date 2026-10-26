#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspengine.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

#define LOOP 25000
static int64_t model[LOOP * 2];

static int64_t lv( int64_t x, int lev ) { return lev == 99 ? 0 : lev >= 0 ? x >> lev : x * ( 1 << -lev ); }

static int32_t *make( int frames, unsigned seed, const char *path )
  {
  int32_t *p = malloc( 8 * frames );
  unsigned x = seed;
  for( int i = 0; i < frames * 2; i++ ) { x = x * 1664525u + 1013904223u; p[i] = (int32_t)( x ^ ( x << 9 ) ); }
  FILE *f = fopen( path, "wb" );
  for( int i = 0; i < frames * 2; i++ ) { unsigned char b[4]; uint32_t v = (uint32_t)p[i]; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; fwrite( b, 1, 4, f ); }
  fclose( f );
  return p;
  }

/* the model: the same arithmetic done the slow plain way */
static void mput( const int32_t *p, int frames, int64_t start, int lo, int hi, int ol, int orr, int nl, int nr )
  {
  for( int i = lo; i < ( hi < 0 ? frames : hi ); i++ )
    {
    int64_t at = ( start + i ) % LOOP;
    model[2*at]   += lv( p[2*i], nl ) - lv( p[2*i], ol );
    model[2*at+1] += lv( p[2*i+1], nr ) - lv( p[2*i+1], orr );
    }
  }

static int same( AspTank *t )
  {
  static int64_t got[LOOP * 2];
  if( asp_tank_get( t, 0, LOOP, got ) ) return 0;
  return ! memcmp( got, model, sizeof model );
  }

int main( void )
  {
  char dir[] = "/tmp/asptank_testXXXXXX", p[300], e[ASP_ERR], cmd[600];
  if( ! mkdtemp( dir ) ) return 1;
  snprintf( p, sizeof p, "%s/s", dir );
  AspStore *s = asp_create( p, "{\"name\":\"t\"}", e );
  snprintf( p, sizeof p, "%s/loop.d64", dir );
  AspTank *t = asp_tank_create( p, LOOP, e, sizeof e );
  CHECK( s && t && asp_tank_frames( t ) == LOOP );

  CHECK( asp_level( -3, 1 ) == -2 && asp_level( 5, -2 ) == 20 && asp_level( 7, ASP_LEV_OFF ) == 0 );

  char fa[300], fb[300];
  snprintf( fa, sizeof fa, "%s/a.d32", dir ); snprintf( fb, sizeof fb, "%s/b.d32", dir );
  int32_t *A = make( 30000, 11, fa ), *B = make( 7000, 22, fb );   /* A is longer than the loop */
  AspTake ta, tb;
  CHECK( asp_take_add_d32( s, 5, 0, fa, 2, 48000, "off", 0, &ta ) == 0 );
  CHECK( asp_take_add_d32( s, 5, 1, fb, 2, 48000, "off", 0, &tb ) == 0 );   /* 1 s in = 23000 into a 25000 loop */

  const int OFF = ASP_LEV_OFF;
  CHECK( asp_apply_take( t, s, ta.id, 0, -1, OFF, OFF, 0, 0 ) == 0 );   mput( A, 30000, 0, 0, -1, OFF, OFF, 0, 0 );
  CHECK( same( t ) );
  CHECK( asp_apply_take( t, s, tb.id, 0, -1, OFF, OFF, 2, OFF ) == 0 ); mput( B, 7000, 48000, 0, -1, OFF, OFF, 2, OFF );
  CHECK( same( t ) );                                                   /* wraps past the loop end */
  CHECK( asp_apply_take( t, s, ta.id, 0, -1, 0, 0, 1, -1 ) == 0 );      mput( A, 30000, 0, 0, -1, 0, 0, 1, -1 );
  CHECK( same( t ) );                                                   /* change of level */
  CHECK( asp_apply_take( t, s, tb.id, 100, 5000, 2, OFF, OFF, OFF ) == 0 ); mput( B, 7000, 48000, 100, 5000, 2, OFF, OFF, OFF );
  CHECK( same( t ) );                                                   /* ranged removal */
  CHECK( asp_apply_take( t, s, tb.id, 0, 100, 2, OFF, OFF, OFF ) == 0 ); mput( B, 7000, 48000, 0, 100, 2, OFF, OFF, OFF );
  CHECK( asp_apply_take( t, s, tb.id, 5000, -1, 2, OFF, OFF, OFF ) == 0 ); mput( B, 7000, 48000, 5000, -1, 2, OFF, OFF, OFF );
  CHECK( same( t ) );
  CHECK( asp_apply_take( t, s, ta.id, 0, -1, 1, -1, OFF, OFF ) == 0 );  mput( A, 30000, 0, 0, -1, 1, -1, OFF, OFF );
  { static int64_t z[LOOP * 2]; CHECK( ! memcmp( model, z, sizeof model ) && same( t ) ); }   /* exactly empty */

  /* states in the log, rebuild, and rebuild at an earlier log point */
  int64_t q0 = asp_log_last( s );
  asp_take_set_state( s, ta.id, "{\"status\":\"mix\",\"lev\":[\"1\",\"1\"],\"gone\":0}", "t", 1 );
  int64_t q1 = asp_take_set_state( s, tb.id, "{\"status\":\"mix\",\"lev\":[\"0\",\"-\"],\"gone\":1000}", "t", 2 );
  memset( model, 0, sizeof model );
  mput( A, 30000, 0, 0, -1, OFF, OFF, 1, 1 );
  mput( B, 7000, 48000, 1000, -1, OFF, OFF, 0, OFF );
  CHECK( asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  memset( model, 0, sizeof model );
  mput( A, 30000, 0, 0, -1, OFF, OFF, 1, 1 );
  AspTakeState st; asp_parse_state( "{\"status\":\"mix\",\"lev\":[null,2]}", &st );
  CHECK( st.l == OFF && st.r == 2 );
  asp_take_set_state( s, tb.id, "{\"status\":\"off\"}", "t", 3 );
  CHECK( asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  int64_t r = asp_revert( s, q1, "t" );                                 /* back to B in */
  memset( model, 0, sizeof model );
  mput( A, 30000, 0, 0, -1, OFF, OFF, 1, 1 );
  mput( B, 7000, 48000, 1000, -1, OFF, OFF, 0, OFF );
  CHECK( r > 0 && asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  memset( model, 0, sizeof model );
  CHECK( asp_rebuild( t, s, q0 ) == 0 && same( t ) );                   /* before any state: empty */

  CHECK( asp_tank_set_head( t, 193, 12345 ) == 0 );
  asp_tank_close( t );
  t = asp_tank_open( p, e, sizeof e );
  CHECK( t && asp_tank_iter( t ) == 193 && asp_tank_peak( t ) == 12345 && asp_tank_frames( t ) == LOOP );
  asp_tank_close( t ); asp_close( s ); free( A ); free( B );
  snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); if( system( cmd ) ) {}
  printf( fails ? "%d FAILED\n" : "all tank tests passed\n", fails );
  return fails != 0;
  }
