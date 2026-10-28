#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "takefile.h"
#include "codec.h"
#include "asptank.h"

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

static const unsigned char ART[ASP_ARTIST] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static int32_t *make( int ch, long frames, int quiet )     /* channel 0 loud and clipping now and then; the others quiet, or silent */
  {
  int32_t *p = malloc( (size_t)frames * ch * 4 );
  unsigned x = 777;
  for( long i = 0; i < frames; i++ )
    for( int k = 0; k < ch; k++ )
      {
      x = x * 1664525u + 1013904223u;
      int32_t v = (int32_t)( x ^ ( x << 11 ) );
      p[i * ch + k] = k == 0 ? ( i == 5 ? 2147483647 : v ) : quiet ? ( v >> 20 ) : 0;
      }
  return p;
  }

static int write_take( const char *path, int ch, const int32_t *pcm, long frames, int input, int ticks, int damaged, AspHdr *h )
  {
  AspTakeW *w = asp_tw_open( path, ART, ch, 48000, input );
  if( ! w ) return -1;
  for( long i = 0; i < frames; i += 4000 )
    if( asp_tw_put( w, pcm + i * ch, (unsigned)( frames - i < 4000 ? frames - i : 4000 ) ) ) return -1;
  for( int t = 0; t < ticks; t++ ) asp_tw_active( w, ch > 1 ? 1 : 1 );
  return asp_tw_close( w, damaged, h );
  }

typedef struct { long n; int ch, calls; } Count;
static int count_cb( const int32_t *pcm, unsigned frames, int ch, void *u ) { Count *c = u; (void)pcm; c->n += frames; c->ch = ch; c->calls++; return 0; }
static int stop_cb( const int32_t *pcm, unsigned frames, int ch, void *u ) { (void)pcm; (void)frames; (void)ch; (void)u; return 1; }

int main( void )
  {
  char dir[] = "/tmp/takefile_testXXXXXX", p[256];
  mkdtemp( dir );
  snprintf( p, sizeof p, "%s/a.a5t", dir );
  long frames = ASP_TICK * 4 + 321;
  int32_t *pcm = make( 2, frames, 0 );
  AspHdr h, g;

  CHECK( write_take( p, 2, pcm, frames, 0, 30, 0, &h ) == 0 );
  CHECK( h.finished && h.frames == (uint64_t)frames && h.channels == 2 && h.rate == 48000 && h.bits == 32 );
  CHECK( ! strcmp( h.codec, asp_codec_name() ) && ! memcmp( h.artist, ART, ASP_ARTIST ) );
  CHECK( h.peak[0] == 2147483647u && h.peak[1] == 0 && h.clips[0] >= 1 && h.clips[1] == 0 );
  CHECK( h.active[0] == 30 && h.active[1] == 0 && ! h.ident );
  CHECK( asp_take_header( p, &g ) == 0 && g.frames == h.frames && g.active[0] == 30 && g.peak[0] == h.peak[0] && g.finished );
  CHECK( asp_hdr_mono( &g, 1 ) == 0 );                    /* one channel never heard: mono, channel 0 */
  CHECK( asp_hdr_mono( &g, 0 ) == -1 );                   /* detection off */

  {                                                       /* the views */
  int32_t *all, *one; int64_t n; int ch;
  CHECK( asp_take_load( p, -1, &all, &n, &ch ) == 0 && n == frames && ch == 2 && ! memcmp( all, pcm, (size_t)frames * 8 ) );
  CHECK( asp_take_load( p, 0, &one, &n, &ch ) == 0 && n == frames && ch == 1 );
  for( long i = 0; i < frames; i++ ) if( one[i] != pcm[2 * i] ) { CHECK( 0 ); break; }
  free( one );
  CHECK( asp_take_load( p, 2, &one, &n, &ch ) != 0 );     /* no such channel */
  free( all );
  Count c = { 0, 0, 0 };
  CHECK( asp_take_frames( p, -1, count_cb, &c ) == 0 && c.n == frames && c.ch == 2 );
  CHECK( asp_take_frames( p, -1, stop_cb, NULL ) != 0 );  /* a callback can abort */
  }

  {                                                       /* an interface told what to hear wins over what was heard */
  snprintf( p, sizeof p, "%s/b.a5t", dir );
  CHECK( write_take( p, 2, pcm, frames, 2, 30, 0, &h ) == 0 );
  CHECK( asp_hdr_mono( &h, 1 ) == 1 );
  snprintf( p, sizeof p, "%s/c.a5t", dir );
  CHECK( write_take( p, 2, pcm, frames, 3, 30, 0, &h ) == 0 );
  CHECK( asp_hdr_mono( &h, 1 ) == -1 );
  }

  {                                                       /* damaged takes are not guessed at; equal channels are mono */
  snprintf( p, sizeof p, "%s/d.a5t", dir );
  CHECK( write_take( p, 2, pcm, frames, 0, 30, 1, &h ) == 0 && h.damaged && asp_hdr_mono( &h, 1 ) == -1 );
  int32_t *eq = make( 2, frames, 0 );
  for( long i = 0; i < frames; i++ ) eq[2 * i + 1] = eq[2 * i];
  snprintf( p, sizeof p, "%s/e.a5t", dir );
  CHECK( write_take( p, 2, eq, frames, 0, 5, 0, &h ) == 0 && h.ident && asp_hdr_mono( &h, 1 ) == 0 );
  free( eq );
  }

  {                                                       /* both heard about equally: stereo; one 20 times as often: that one */
  AspTakeW *w;
  snprintf( p, sizeof p, "%s/f.a5t", dir );
  w = asp_tw_open( p, ART, 2, 48000, 0 );
  asp_tw_put( w, pcm, 100 );
  for( int i = 0; i < 40; i++ ) asp_tw_active( w, 3 );
  CHECK( asp_tw_close( w, 0, &h ) == 0 && asp_hdr_mono( &h, 1 ) == -1 );
  snprintf( p, sizeof p, "%s/g.a5t", dir );
  w = asp_tw_open( p, ART, 2, 48000, 0 );
  asp_tw_put( w, pcm, 100 );
  for( int i = 0; i < 40; i++ ) asp_tw_active( w, 2 );
  asp_tw_active( w, 1 ); asp_tw_active( w, 1 );
  CHECK( asp_tw_close( w, 0, &h ) == 0 && asp_hdr_mono( &h, 1 ) == 1 );
  }

  {                                                       /* a take that never ended: unfinished, and decodes what was written */
  snprintf( p, sizeof p, "%s/h.a5t", dir );
  AspTakeW *w = asp_tw_open( p, ART, 2, 48000, 0 );
  asp_tw_put( w, pcm, ASP_TICK * 3 + 5 );
  asp_tw_abort( w );
  CHECK( asp_take_header( p, &g ) == 0 && ! g.finished );
  Count c = { 0, 0, 0 };
  asp_take_frames( p, -1, count_cb, &c );
  CHECK( c.n >= ASP_TICK * 2 && c.n <= ASP_TICK * 3 + 5 );
  }

  {                                                       /* not a take; a take cut short; a damaged block */
  FILE *f; long size;
  snprintf( p, sizeof p, "%s/i.a5t", dir );
  f = fopen( p, "wb" ); fputs( "nothing to see here", f ); fclose( f );
  CHECK( asp_take_header( p, &g ) != 0 );
  snprintf( p, sizeof p, "%s/nope.a5t", dir );
  CHECK( asp_take_header( p, &g ) != 0 );

  snprintf( p, sizeof p, "%s/j.a5t", dir );
  CHECK( write_take( p, 2, pcm, frames, 0, 3, 0, &h ) == 0 );
  f = fopen( p, "rb" ); fseek( f, 0, SEEK_END ); size = ftell( f ); fclose( f );
  CHECK( truncate( p, size - size / 5 ) == 0 );
  Count c = { 0, 0, 0 };
  CHECK( asp_take_frames( p, -1, count_cb, &c ) != 0 );   /* finished says more frames than there are */

  if( asp_codec_detects() )
    {
    snprintf( p, sizeof p, "%s/k.a5t", dir );
    CHECK( write_take( p, 2, pcm, frames, 0, 3, 0, &h ) == 0 );
    f = fopen( p, "r+b" ); fseek( f, ASP_HDR_SIZE + ( size - ASP_HDR_SIZE ) / 2, SEEK_SET ); fputc( 0x5a, f ); fputc( 0xa5, f ); fclose( f );
    CHECK( asp_take_frames( p, -1, count_cb, &c ) != 0 );
    }
  }

  {                                                       /* one channel, and four */
  int32_t *m = make( 1, frames, 0 ), *q = make( 4, frames, 1 ); int32_t *back; int64_t n; int ch;
  snprintf( p, sizeof p, "%s/l.a5t", dir );
  CHECK( write_take( p, 1, m, frames, 0, 2, 0, &h ) == 0 && h.channels == 1 && ! h.ident && asp_hdr_mono( &h, 1 ) == -1 );
  CHECK( asp_take_load( p, -1, &back, &n, &ch ) == 0 && ch == 1 && ! memcmp( back, m, (size_t)frames * 4 ) );
  free( back );
  snprintf( p, sizeof p, "%s/m.a5t", dir );
  CHECK( write_take( p, 4, q, frames, 0, 2, 0, &h ) == 0 && h.channels == 4 && h.peak[3] > 0 );
  CHECK( asp_take_load( p, -1, &back, &n, &ch ) == 0 && ch == 4 && ! memcmp( back, q, (size_t)frames * 16 ) );
  free( back ); free( m ); free( q );
  }

  CHECK( asp_tw_open( "/nonexistent/dir/x.a5t", ART, 2, 48000, 0 ) == NULL );
  snprintf( p, sizeof p, "%s/a.a5t", dir );
  CHECK( asp_tw_open( p, ART, 2, 48000, 0 ) == NULL );    /* never over an existing take */

  free( pcm );
  { char cmd[300]; snprintf( cmd, sizeof cmd, "rm -rf %s", dir ); if( system( cmd ) ) {} }
  if( fails ) { fprintf( stderr, "%d failures\n", fails ); return 1; }
  printf( "all takefile tests passed (%s)\n", asp_codec_name() );
  return 0;
  }
