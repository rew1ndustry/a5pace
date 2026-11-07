#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "a5mixer.h"

int a5mix_parse( const char *s, MixCmd *c )
  {
  const char *p = s; int ops = 0;
  memset( c, 0, sizeof *c );
  if( isdigit( (unsigned char)*p ) )
    {
    c->has_iter = 1; c->iter = strtol( p, (char **)&p, 10 );
    if( *p == '.' ) { p++; if( isdigit( (unsigned char)*p ) ) { c->has_take = 1; c->take = strtol( p, (char **)&p, 10 ); } }
    }
  for( ; ; )
    {
    if( ! *p ) return MP_WAIT;
    if( *p == '+' || *p == '-' || *p == '/' || *p == '\\' )
      {
      char o = *p++; int have = isdigit( (unsigned char)*p ); long v = have ? strtol( p, (char **)&p, 10 ) : 0;
      if( ! *p ) return MP_WAIT;                                  /* the number may not be finished */
      if( o == '+' || o == '-' ) { c->set_gain = have ? 1 : 2; c->gdb = (int)( o == '-' ? -v : v ); }
      else { c->set_pan = have ? 1 : 2; c->pan = (int)( o == '\\' ? -v : v ); }
      ops++;
      continue;
      }
    if( ! strchr( ".!mMpP", *p ) || p[1] ) return MP_BAD;
    c->now = *p == '!' || *p == 'M' || *p == 'P';
    c->act = *p == 'm' || *p == 'M' ? 1 : *p == 'p' || *p == 'P' ? 2 : ops ? 0 : 3;
    return MP_DONE;
    }
  }

/* the control line for one take: MIX iter off|* [gN] [pN] [MUTE|PLAY|TOGGLE] [NOW]. off < 0 is every take of the iteration */
int a5mix_line( char *b, size_t n, long iter, long off, const MixCmd *c )
  {
  size_t o = (size_t)snprintf( b, n, "MIX %ld ", iter );
  #define ADD( ... ) do { if( o < n ) o += (size_t)snprintf( b + o, n - o, __VA_ARGS__ ); } while( 0 )
  if( off < 0 ) ADD( "*" ); else ADD( "%ld", off );
  if( c->set_gain ) { if( c->set_gain == 2 ) ADD( " g" ); else ADD( " g%d", c->gdb ); }
  if( c->set_pan ) { if( c->set_pan == 2 ) ADD( " p" ); else ADD( " p%d", c->pan ); }
  if( c->act ) ADD( c->act == 1 ? " MUTE" : c->act == 2 ? " PLAY" : " TOGGLE" );
  if( c->now ) ADD( " NOW" );
  #undef ADD
  return o < n ? (int)o : (int)n - 1;
  }
