#include <stdio.h>
#include <string.h>
#include "a5mixer.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

int main( void )
  {
  MixCmd c;
  CHECK( a5mix_parse( "123", &c ) == MP_WAIT );
  CHECK( a5mix_parse( "123.", &c ) == MP_WAIT );                         /* the first dot is the separator: it waits */
  CHECK( a5mix_parse( "123.15", &c ) == MP_WAIT );
  CHECK( a5mix_parse( "123..", &c ) == MP_DONE && c.has_iter && c.iter == 123 && ! c.has_take && c.act == 3 && ! c.now );
  CHECK( a5mix_parse( "123.16.", &c ) == MP_DONE && c.has_take && c.take == 16 && c.act == 3 );
  CHECK( a5mix_parse( "123.16+3.", &c ) == MP_DONE && c.set_gain == 1 && c.gdb == 3 && c.act == 0 && ! c.now );
  CHECK( a5mix_parse( "123.16-2/20.", &c ) == MP_DONE && c.gdb == -2 && c.set_pan == 1 && c.pan == 20 );
  { MixCmd d; CHECK( a5mix_parse( "123.16/20-2.", &d ) == MP_DONE && d.gdb == c.gdb && d.pan == c.pan && d.take == c.take ); }
  CHECK( a5mix_parse( "123.16\\90!", &c ) == MP_DONE && c.pan == -90 && c.now && c.act == 0 );
  CHECK( a5mix_parse( "123m", &c ) == MP_DONE && c.iter == 123 && ! c.has_take && c.act == 1 && ! c.now );
  CHECK( a5mix_parse( "123.15m", &c ) == MP_DONE && c.take == 15 && c.act == 1 );
  CHECK( a5mix_parse( "123.17P", &c ) == MP_DONE && c.act == 2 && c.now );
  CHECK( a5mix_parse( "123.17+3M", &c ) == MP_DONE && c.act == 1 && c.now && c.gdb == 3 );     /* settings and a mute together */
  CHECK( a5mix_parse( "123-3.", &c ) == MP_DONE && ! c.has_take && c.gdb == -3 );               /* a whole iteration, no separator */
  CHECK( a5mix_parse( "+0.", &c ) == MP_DONE && ! c.has_iter && c.set_gain == 1 && c.gdb == 0 );
  CHECK( a5mix_parse( "+.", &c ) == MP_DONE && c.set_gain == 2 );                               /* keep the gain, as a mix of its own */
  CHECK( a5mix_parse( "/.", &c ) == MP_DONE && c.set_pan == 2 );
  CHECK( a5mix_parse( ".", &c ) == MP_DONE && ! c.has_iter && c.act == 3 );
  CHECK( a5mix_parse( "!", &c ) == MP_DONE && c.act == 3 && c.now );
  CHECK( a5mix_parse( "m", &c ) == MP_DONE && c.act == 1 );
  CHECK( a5mix_parse( "+", &c ) == MP_WAIT && a5mix_parse( "+3", &c ) == MP_WAIT && a5mix_parse( "12.3/", &c ) == MP_WAIT );
  CHECK( a5mix_parse( "123x", &c ) == MP_BAD );
  CHECK( a5mix_parse( "123.15.5", &c ) == MP_BAD );
  printf( fails ? "%d FAILED\n" : "all mix parser tests passed\n", fails );
  return fails != 0;
  }
