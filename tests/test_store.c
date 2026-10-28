#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aspstore.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

static int count;
static char last_state[8][128];
static void cb( int64_t id, const char *st, void *u )
  { (void)u; if( id < 8 ) snprintf( last_state[id], 128, "%s", st ); count++; }

static void write_d32( const char *path, int frames, int seed )
  {
  FILE *f = fopen( path, "wb" );
  unsigned x = seed;
  for( int i = 0; i < frames * 2; i++ )
    { x = x * 1664525u + 1013904223u; int32_t v = (int32_t)( x ^ ( x << 11 ) ); if( i % 7 == 0 ) v = 0; fwrite( &v, 4, 1, f ); }
  fclose( f );
  }

int main( void )
  {
  char dir[] = "/tmp/aspstore_testXXXXXX", p[256], e[ASP_ERR];
  mkdtemp( dir );
  { char x[300]; snprintf( x, sizeof x, "%s/cfg", dir ); setenv( "XDG_CONFIG_HOME", x, 1 ); }       /* the artist is made in the test, not in the user's config */
  snprintf( p, sizeof p, "%s/s", dir );
  AspStore *s = asp_create( p, "{\"name\":\"t\"}", e );
  CHECK( s );
  if( ! s ) { fprintf( stderr, "%s\n", e ); return 1; }
  CHECK( asp_create( p, "{\"name\":\"t\"}", e ) == NULL );              /* no second session in the dir */

  char pcm[300], back[300];
  snprintf( pcm, sizeof pcm, "%s/a.d32", dir );  snprintf( back, sizeof back, "%s/b.d32", dir );
  write_d32( pcm, 50000, 1 );
  AspTake t1, t2, tmp;
  CHECK( asp_take_add_d32( s, 5, 18, pcm, 2, 48000, "mix", 100, &t1 ) == 0 );
  CHECK( t1.frames == 50000 && t1.bits == 32 && t1.channels == 2 && strlen( t1.artist ) == 32 && ! strcmp( t1.artist, asp_artist( s ) ) );
  CHECK( asp_take_add_d32( s, 5, 18, pcm, 2, 48000, "mix", 101, &tmp ) != 0 );   /* never overwritten */
  write_d32( pcm, 1234, 2 );
  CHECK( asp_take_add_d32( s, 5, 19, pcm, 2, 48000, "mix", 102, &t2 ) == 0 );
  CHECK( asp_take_find( s, 5, 19, &tmp ) == 0 && tmp.id == t2.id );

  char sha[65], bp[300], want[300];
  CHECK( asp_take_path( s, &t1, bp, sizeof bp ) == 0 );
  snprintf( want, sizeof want, "%s/takes.%.12s/5/18.a5t", p, asp_artist( s ) );
  CHECK( ! strcmp( bp, want ) && access( bp, F_OK ) == 0 );                        /* named by who, which iteration, where */
  { AspHdr h; CHECK( asp_take_header( bp, &h ) == 0 && h.finished && h.frames == 50000 && ! memcmp( h.artist, asp_artist_id( s ), ASP_ARTIST ) ); }
  { char f[300]; snprintf( f, sizeof f, "%s/cfg/aspace/artist", dir ); CHECK( access( f, F_OK ) == 0 ); }   /* made on first use, kept */
  asp_sha256_buf( "abc", 3, sha );
  CHECK( ! strcmp( sha, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ) );

  write_d32( pcm, 50000, 1 );                                                       /* the first take again */
  CHECK( asp_take_export_d32( s, t1.id, back ) == 0 );
  char cmd[700]; snprintf( cmd, sizeof cmd, "cmp -s %s %s", pcm, back );
  CHECK( system( cmd ) == 0 );                                                      /* bit exact round trip */

  /* state, log, revert */
  int64_t a = asp_log_last( s );
  int64_t q1 = asp_take_set_state( s, t1.id, "{\"status\":\"off\",\"lev\":[\"1\",\"1\"]}", "t", 200 );
  int64_t q2 = asp_take_set_state( s, t2.id, "{\"status\":\"trk\"}", "t", 201 );
  CHECK( q1 == a + 1 && q2 == a + 2 );
  CHECK( asp_mix_name( s, "before-edit", a ) == 0 && asp_mix_seq( s, "before-edit" ) == a );
  CHECK( asp_mix_seq( s, "nope" ) == -1 );
  count = 0; CHECK( asp_fold( s, -1, cb, NULL ) == 0 );
  CHECK( count == 2 && strstr( last_state[t1.id], "off" ) && strstr( last_state[t2.id], "trk" ) );
  count = 0; CHECK( asp_fold( s, a, cb, NULL ) == 0 );
  CHECK( strstr( last_state[t1.id], "mix" ) && count == 2 );

  int64_t r = asp_revert( s, a, "t" );
  CHECK( r > q2 );
  count = 0; asp_fold( s, -1, cb, NULL );
  CHECK( strstr( last_state[t1.id], "mix" ) && strstr( last_state[t2.id], "mix" ) );   /* back to a */
  int64_t r2 = asp_revert( s, q2, "t" );                                                 /* undo the revert */
  asp_fold( s, -1, cb, NULL );
  CHECK( r2 > r && strstr( last_state[t1.id], "off" ) && strstr( last_state[t2.id], "trk" ) );
  CHECK( asp_revert( s, 99999, "t" ) < 0 );

  /* reopen, readonly refuses writes */
  asp_close( s );
  s = asp_open( p, 1, e );
  CHECK( s && asp_log( s, "x", "y", "{}", 0 ) < 0 );
  CHECK( s && asp_take_add_d32( s, 9, 9, pcm, 2, 48000, "mix", 0, NULL ) != 0 );
  asp_close( s );
  s = asp_open( p, 0, e );
  CHECK( s && asp_log_last( s ) == r2 );
  asp_close( s );

  snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); system( cmd );
  printf( fails ? "%d FAILED\n" : "all store tests passed\n", fails );
  return fails != 0;
  }
