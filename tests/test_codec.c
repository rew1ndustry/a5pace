#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codec.h"

/* the same test for every codec: make CODEC=flac / CODEC=raw */

static int fails;
#define CHECK(x) do { if( ! (x) ) { fprintf( stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x ); fails++; } } while(0)

#define BLOCK 9600

typedef struct { unsigned char *b; size_t n, cap, hdr; int calls, hcalls, bad_order; unsigned first_samples; uint32_t next; } Sink;

static int sink( const unsigned char *b, size_t n, unsigned samples, uint32_t frame, void *u )
  {
  Sink *s = u;
  if( s->n + n > s->cap ) { s->cap = ( s->n + n ) * 2; s->b = realloc( s->b, s->cap ); }
  memcpy( s->b + s->n, b, n ); s->n += n;
  if( ! samples ) { s->hcalls++; s->hdr += n; if( s->calls ) s->bad_order = 1; }
  else { if( ! s->calls ) s->first_samples = samples; s->calls++; (void)frame; }
  return 0;
  }

typedef struct { const unsigned char *b; size_t n, pos, step; } Src;
static int source( unsigned char *b, size_t *n, void *u )
  {
  Src *s = u;
  size_t k = s->n - s->pos;
  if( k > *n ) k = *n;
  if( s->step && k > s->step ) k = s->step;          /* a source that gives little at a time */
  memcpy( b, s->b + s->pos, k ); s->pos += k; *n = k;
  return 0;
  }
static int fail_source( unsigned char *b, size_t *n, void *u ) { (void)b; (void)n; (void)u; return -1; }

static int32_t *make( int ch, long frames )
  {
  int32_t *p = malloc( (size_t)frames * ch * 4 );
  unsigned x = 12345;
  for( long i = 0; i < frames; i++ )
    for( int k = 0; k < ch; k++ )
      {
      x = x * 1664525u + 1013904223u;
      p[i * ch + k] = k == 1 ? 0 : (int32_t)( x ^ ( x << 11 ) ) / ( 1 << ( 4 * k ) );   /* full range, a silent channel, quieter ones */
      }
  return p;
  }

static int encode( Sink *s, int ch, const int32_t *pcm, long frames, long put, int verify )
  {
  AspEnc *e = asp_enc_new( ch, 48000, BLOCK, verify, sink, s );
  if( ! e ) return -1;
  int rc = 0;
  for( long i = 0; i < frames && ! rc; i += put ) rc = asp_enc_put( e, pcm + i * ch, (unsigned)( frames - i < put ? frames - i : put ) );
  return asp_enc_end( e ) || rc ? -1 : 0;
  }

/* decode all of b[from..n) through a source giving at most step bytes at a time, in reads of at most max frames */
static long decode( const unsigned char *b, size_t from, size_t n, int ch, size_t step, unsigned max, int32_t *out, int *errors )
  {
  Src src = { b + from, n - from, 0, step };
  AspDec *d = asp_dec_new( ch, source, &src );
  long got = 0;
  int k;
  if( ! d ) return -1;
  while( ( k = asp_dec_get( d, out + got * ch, max ) ) > 0 ) got += k;
  if( errors ) *errors = asp_dec_errors( d );
  asp_dec_free( d );
  return k < 0 ? -1 : got;
  }

static void roundtrip( int ch, long frames, long put, int verify )
  {
  Sink s; memset( &s, 0, sizeof s );
  int32_t *pcm = make( ch, frames ), *out = malloc( (size_t)frames * ch * 4 + 4 );
  int errors = -1;
  CHECK( encode( &s, ch, pcm, frames, put, verify ) == 0 );
  CHECK( s.bad_order == 0 );
  long blocks = ( frames + BLOCK - 1 ) / BLOCK;
  if( put % BLOCK == 0 || put >= frames ) CHECK( s.calls == blocks );        /* a block put whole is one sink call */
  if( frames >= BLOCK ) CHECK( s.first_samples == BLOCK );
  CHECK( decode( s.b, 0, s.n, ch, 0, 4096, out, &errors ) == frames );
  CHECK( memcmp( out, pcm, (size_t)frames * ch * 4 ) == 0 );
  CHECK( errors == 0 );
  CHECK( decode( s.b, s.hdr, s.n, ch, 0, BLOCK, out, &errors ) == frames );  /* without the header bytes: astream does */
  CHECK( memcmp( out, pcm, (size_t)frames * ch * 4 ) == 0 );
  CHECK( decode( s.b, 0, s.n, ch, 7, 1000, out, NULL ) == frames );          /* a source giving 7 bytes at a time */
  CHECK( memcmp( out, pcm, (size_t)frames * ch * 4 ) == 0 );
  free( pcm ); free( out ); free( s.b );
  }

int main( void )
  {
  printf( "codec %s\n", asp_codec_name() );
  roundtrip( 2, BLOCK * 5, BLOCK, 0 );                 /* whole blocks, as a tick is put */
  roundtrip( 2, BLOCK * 5, BLOCK, 1 );
  roundtrip( 2, BLOCK * 5 + 1234, 4096, 0 );           /* put in slices, a short last block */
  roundtrip( 1, BLOCK * 3, BLOCK, 0 );
  roundtrip( 4, BLOCK * 3 + 17, BLOCK, 0 );
  roundtrip( 2, 100, 100, 0 );                         /* less than one block */

  {                                                    /* a stream cut short decodes up to the last whole block */
  Sink s; memset( &s, 0, sizeof s );
  int32_t *pcm = make( 2, BLOCK * 6 ), *out = malloc( (size_t)BLOCK * 6 * 8 );
  CHECK( encode( &s, 2, pcm, BLOCK * 6, BLOCK, 0 ) == 0 );
  long got = decode( s.b, 0, s.n - s.n / 12, 2, 0, 4096, out, NULL );
  CHECK( got >= BLOCK * 4 && got < BLOCK * 6 );
  if( asp_codec_detects() ) CHECK( got % BLOCK == 0 );      /* a codec with blocks of its own gives whole ones */
  CHECK( got < 0 || memcmp( out, pcm, (size_t)got * 8 ) == 0 );
  free( pcm ); free( out ); free( s.b );
  }

  if( asp_codec_detects() )                            /* a damaged block is counted and skipped; the rest decode */
    {
    Sink s; memset( &s, 0, sizeof s );
    int32_t *pcm = make( 2, BLOCK * 6 ), *out = malloc( (size_t)BLOCK * 6 * 8 );
    int errors = 0;
    CHECK( encode( &s, 2, pcm, BLOCK * 6, BLOCK, 0 ) == 0 );
    s.b[s.hdr + ( s.n - s.hdr ) / 2] ^= 0x10;
    long got = decode( s.b, 0, s.n, 2, 0, 4096, out, &errors );
    CHECK( errors > 0 );
    CHECK( got > 0 && got <= BLOCK * 6 );
    free( pcm ); free( out ); free( s.b );
    }

  {                                                    /* a failing source and a failing sink are errors, not hangs */
  AspDec *d = asp_dec_new( 2, fail_source, NULL );
  int32_t buf[64];
  CHECK( d && asp_dec_get( d, buf, 32 ) < 0 );
  asp_dec_free( d );
  }

  CHECK( asp_enc_new( 0, 48000, BLOCK, 0, sink, NULL ) == NULL );
  CHECK( asp_enc_new( 9, 48000, BLOCK, 0, sink, NULL ) == NULL );
  CHECK( asp_dec_new( 0, source, NULL ) == NULL );

  if( fails ) { fprintf( stderr, "%d failures\n", fails ); return 1; }
  printf( "all codec tests passed (%s)\n", asp_codec_name() );
  return 0;
  }
