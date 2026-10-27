#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspengine.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

#define LOOP 25000
static int64_t model[LOOP * 2];

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

/* the model: the same sum done the slow plain way. sign +1 puts a range in, -1 takes it out */
static void mput( const int32_t *p, int frames, int64_t start, int lo, int hi, int sign )
  {
  for( int i = lo; i < ( hi < 0 ? frames : hi ); i++ )
    {
    int64_t at = ( start + i ) % LOOP;
    model[2*at] += sign * (int64_t)p[2*i]; model[2*at+1] += sign * (int64_t)p[2*i+1];
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
  AspTank *t = asp_tank_create( p, LOOP, 48000, e, sizeof e );
  CHECK( s && t && asp_tank_frames( t ) == LOOP );

  char fa[300], fb[300];
  snprintf( fa, sizeof fa, "%s/a.d32", dir ); snprintf( fb, sizeof fb, "%s/b.d32", dir );
  int32_t *A = make( 30000, 11, fa ), *B = make( 7000, 22, fb );   /* A is longer than the loop */
  AspTake ta, tb;
  CHECK( asp_take_add_d32( s, 5, 0, fa, 2, 48000, "off", 0, &ta ) == 0 );
  CHECK( asp_take_add_d32( s, 5, 1, fb, 2, 48000, "off", 0, &tb ) == 0 );   /* 1 s in = 23000 into a 25000 loop */

  AspPlace OFF = ASP_PLACE_OFF, ST = ASP_PLACE_STEREO;
  CHECK( asp_move_take( t, s, ta.id, 0, -1, OFF, ST ) == 0 );   mput( A, 30000, 0, 0, -1, 1 );        CHECK( same( t ) );
  CHECK( asp_move_take( t, s, tb.id, 0, -1, OFF, ST ) == 0 );   mput( B, 7000, 48000, 0, -1, 1 );     CHECK( same( t ) );   /* wraps past the loop end */
  CHECK( asp_move_take( t, s, tb.id, 100, 5000, ST, OFF ) == 0 ); mput( B, 7000, 48000, 100, 5000, -1 ); CHECK( same( t ) );   /* a ranged removal */
  CHECK( asp_move_take( t, s, tb.id, 0, 100, ST, OFF ) == 0 );   mput( B, 7000, 48000, 0, 100, -1 );
  CHECK( asp_move_take( t, s, tb.id, 5000, -1, ST, OFF ) == 0 ); mput( B, 7000, 48000, 5000, -1, -1 ); CHECK( same( t ) );
  CHECK( asp_move_take( t, s, ta.id, 0, -1, ST, OFF ) == 0 );    mput( A, 30000, 0, 0, -1, -1 );
  { static int64_t z[LOOP * 2]; CHECK( ! memcmp( model, z, sizeof model ) && same( t ) ); }   /* exactly empty */

  /* asp_tank_add on its own, across the loop end */
  CHECK( asp_tank_add( t, LOOP - 100, B, 300 ) == 0 );
  for( int i = 0; i < 300; i++ ) { int64_t at = ( LOOP - 100 + i ) % LOOP; model[2*at] += B[2*i]; model[2*at+1] += B[2*i+1]; }
  CHECK( same( t ) );
  CHECK( asp_tank_add( t, LOOP - 100, B, 300 ) == 0 );
  for( int i = 0; i < 300; i++ ) { int64_t at = ( LOOP - 100 + i ) % LOOP; model[2*at] += B[2*i]; model[2*at+1] += B[2*i+1]; }
  CHECK( same( t ) );

  /* states in the log, rebuild, and rebuild at an earlier log point */
  memset( model, 0, sizeof model );
  CHECK( asp_rebuild( t, s, -1 ) == 0 && same( t ) );                      /* nothing in yet */
  int64_t q0 = asp_log_last( s );
  asp_take_set_state( s, ta.id, "{\"status\":\"mix\",\"out\":\"lr\",\"gone\":0}", "t", 1 );
  int64_t q1 = asp_take_set_state( s, tb.id, "{\"status\":\"mix\",\"out\":\"lr\",\"gone\":1000}", "t", 2 );
  asp_take_set_state( s, 0, "{\"gain_steps\":2}", "t", 3 );               /* take 0 is the loop gain: not a take, never rebuilt */
  mput( A, 30000, 0, 0, -1, 1 ); mput( B, 7000, 48000, 1000, -1, 1 );
  CHECK( asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  memset( model, 0, sizeof model );
  mput( A, 30000, 0, 0, -1, 1 );
  AspTakeState st; asp_parse_state( "{\"status\":\"mix\",\"out\":\"r\",\"flip\":1}", &st );
  CHECK( st.l == 0 && st.r == 1 && st.flip == 1 );
  asp_parse_state( "{\"status\":\"mix\",\"lev\":[\"2\",\"-\"]}", &st );   /* asp4's levels: on or off, nothing between */
  CHECK( st.l == 1 && st.r == 0 );
  asp_take_set_state( s, tb.id, "{\"status\":\"off\"}", "t", 4 );
  CHECK( asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  int64_t r = asp_revert( s, q1, "t" );                                  /* back to B in */
  mput( B, 7000, 48000, 1000, -1, 1 );
  CHECK( r > 0 && asp_rebuild( t, s, -1 ) == 0 && same( t ) );
  memset( model, 0, sizeof model );
  CHECK( asp_rebuild( t, s, q0 ) == 0 && same( t ) );                    /* before any state: empty */

  CHECK( asp_tank_set_head( t, 193, 12345 ) == 0 );
  CHECK( asp_tank_bind( t, asp_session_id( s ) ) == 0 && asp_tank_set_seq( t, 77 ) == 0 && asp_tank_set_dirty( t, 1 ) == 0 );
  asp_tank_close( t );
  t = asp_tank_open( p, e, sizeof e );
  CHECK( t && asp_tank_iter( t ) == 193 && asp_tank_peak( t ) == 12345 && asp_tank_frames( t ) == LOOP );
  CHECK( t && ! asp_tank_legacy( t ) && asp_tank_seq( t ) == 77 && asp_tank_dirty( t ) && asp_tank_rate( t ) == 48000 );
  CHECK( t && asp_tank_bound( t, asp_session_id( s ) ) && ! asp_tank_bound( t, "0000000000000000000000000000000000000000000000000000000000000001" ) );
  CHECK( t && same( t ) );                                   /* the data is untouched by header writes */
  asp_tank_set_dirty( t, 0 );
  { /* an asp4 file opens as legacy with the same data layout */
    char lp[300]; snprintf( lp, sizeof lp, "%s/old.d64", dir );
    FILE *o = fopen( lp, "wb" ); unsigned char h[16] = { 193 }; fwrite( h, 1, 16, o );
    for( int i = 0; i < 100; i++ ) { unsigned char fr[16] = { (unsigned char)i, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; fwrite( fr, 1, 16, o ); }
    fclose( o );
    AspTank *l = asp_tank_open( lp, e, sizeof e ); int64_t m[8];
    CHECK( l && asp_tank_legacy( l ) && asp_tank_frames( l ) == 100 && asp_tank_iter( l ) == 193 );
    CHECK( l && asp_tank_get( l, 3, 2, m ) == 0 && m[0] == 3 && m[1] == -1 && m[2] == 4 );
    CHECK( l && asp_tank_set_seq( l, 1 ) != 0 );
    asp_tank_close( l );
  }
  asp_tank_close( t ); asp_close( s ); free( A ); free( B );
  snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); if( system( cmd ) ) {}
  printf( fails ? "%d FAILED\n" : "all tank tests passed\n", fails );
  return fails != 0;
  }
