#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "aspstore.h"
#include "keeper.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)
static double now_ms( void ) { struct timespec t; clock_gettime( CLOCK_MONOTONIC, &t ); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main( void )
  {
  char dir[] = "/tmp/keeper_testXXXXXX", p[300], e[ASP_ERR];
  if( ! mkdtemp( dir ) ) return 1;
  { char x[300]; snprintf( x, sizeof x, "%s/cfg", dir ); setenv( "XDG_CONFIG_HOME", x, 1 ); }

  /* the servant alone: settle many files, get them back as done, in order, without the caller waiting */
  AspKeeper *k = asp_keeper_start( NULL, 0 );
  CHECK( k );
  for( int i = 0; i < 20; i++ ) { snprintf( p, sizeof p, "%s/f%d", dir, i ); FILE *f = fopen( p, "w" ); fputs( "x", f ); fclose( f ); }
  double t0 = now_ms();
  for( int i = 0; i < 20; i++ ) { snprintf( p, sizeof p, "%s/f%d", dir, i ); asp_keeper_settle( k, 100 + i, p ); }
  CHECK( now_ms() - t0 < 50 );                                         /* queued, not done */
  int64_t tags[32]; int got = 0, spins = 0;
  while( got < 20 && spins++ < 500 ) { got += asp_keeper_done( k, tags + got, 32 - got ); if( got < 20 ) usleep( 10000 ); }
  CHECK( got == 20 );
  for( int i = 0; i < got; i++ ) CHECK( tags[i] == 100 + i );
  CHECK( asp_keeper_done( k, tags, 32 ) == 0 );
  asp_keeper_settle( k, 7, "/nonexistent/file" );                      /* a file that is gone is done all the same */
  asp_keeper_stop( k );                                                /* stopping lets it finish: no crash, no leak of the queue */

  /* in a store: adopt returns at once, the row appears when the keeper has the file in its place and safe (on poll) */
  snprintf( p, sizeof p, "%s/s", dir );
  AspStore *s = asp_create( p, "{\"name\":\"k\"}", e );
  CHECK( s && asp_store_keeper( s ) == 0 && asp_store_keeper( s ) != 0 );
  char d32[300]; snprintf( d32, sizeof d32, "%s/a.d32", dir );
  FILE *f = fopen( d32, "wb" ); for( int i = 0; i < 40000 * 2; i++ ) { unsigned char b[4] = { (unsigned char)i, (unsigned char)( i * 7 ), 0, 0 }; fwrite( b, 1, 4, f ); } fclose( f );
  AspTake t;
  t0 = now_ms();
  CHECK( asp_take_add_d32( s, 1, 0, d32, 2, 48000, "off", 0, &t ) == 0 );
  CHECK( now_ms() - t0 < 200 );
  char tp[400]; CHECK( asp_take_path( s, &t, tp, sizeof tp ) == 0 );
  CHECK( access( tp, F_OK ) == 0 );                                    /* wherever it is just now (its part, or its place), it is readable */
  int n = 0; spins = 0;
  while( ! n && spins++ < 500 ) { n = asp_store_poll( s ); if( ! n ) usleep( 10000 ); }
  CHECK( n == 1 );
  CHECK( asp_take_path( s, &t, tp, sizeof tp ) == 0 && strstr( tp, "/takes." ) && access( tp, F_OK ) == 0 );   /* and now it is in its place */
  asp_close( s );
  s = asp_open( p, 1, e );
  AspTake back;
  CHECK( asp_take_find( s, 1, 0, &back ) == 0 && back.id == t.id && back.frames == 40000 );   /* the row is there once the file is */
  { char bp[400]; CHECK( asp_take_path( s, &back, bp, sizeof bp ) == 0 && access( bp, F_OK ) == 0 ); }
  asp_close( s );

  /* the checkpoints: the keeper folds the log into the database on a connection of its own, now and then */
  snprintf( p, sizeof p, "%s/s2", dir );
  s = asp_create( p, "{\"name\":\"k2\"}", e );
  char db[400]; snprintf( db, sizeof db, "%s/session.db", p );
  k = asp_keeper_start( db, 100 );
  for( int i = 0; i < 50; i++ ) asp_log( s, "t", "x", "{}", i );
  usleep( 500000 );
  CHECK( asp_keeper_checkpoints( k ) >= 2 );
  asp_log( s, "t", "x", "{}", 99 );                                    /* the core's writes go on beside it */
  asp_keeper_stop( k );
  asp_close( s );
  { char cmd[400]; snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); if( system( cmd ) ) {} }
  printf( fails ? "%d FAILED\n" : "all keeper tests passed\n", fails );
  return fails != 0;
  }
