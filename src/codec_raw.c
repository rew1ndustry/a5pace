#include <stdlib.h>
#include <string.h>
#include "codec.h"
#include "plat.h"

/* the codec with nothing in it: little endian 32 bit, no header. also the proof that the seam is clean */

#define CHUNK 4096

struct AspEnc { int ch, failed; unsigned block, held; uint32_t frame; AspSink sink; void *user; unsigned char *buf; };
struct AspDec { int ch; AspSource source; void *user; unsigned char *buf; size_t have, pos; };

const char *asp_codec_name( void ) { return "raw"; }
int asp_codec_detects( void ) { return 0; }

AspEnc *asp_enc_new( int channels, int rate, int block, int verify, AspSink sink, void *user )
  {
  (void)rate; (void)verify;
  if( channels < 1 || channels > ASP_CODEC_MAXCH || block < 1 ) return NULL;
  AspEnc *e = calloc( 1, sizeof *e );
  if( ! e ) return NULL;
  e->buf = malloc( (size_t)block * channels * 4 );
  if( ! e->buf ) { free( e ); return NULL; }
  e->ch = channels; e->block = (unsigned)block; e->sink = sink; e->user = user;
  return e;
  }

static void send_block( AspEnc *e )
  {
  if( e->held && e->sink( e->buf, (size_t)e->held * e->ch * 4, e->held, e->frame++, e->user ) ) e->failed = 1;
  e->held = 0;
  }

int asp_enc_put( AspEnc *e, const int32_t *pcm, unsigned frames )
  {
  while( frames && ! e->failed )
    {
    unsigned k = e->block - e->held < frames ? e->block - e->held : frames;
    for( size_t i = 0; i < (size_t)k * e->ch; i++ ) asp_st32( e->buf + ( (size_t)e->held * e->ch + i ) * 4, (uint32_t)pcm[i] );
    e->held += k; pcm += (size_t)k * e->ch; frames -= k;
    if( e->held == e->block ) send_block( e );
    }
  return e->failed ? -1 : 0;
  }

int asp_enc_end( AspEnc *e )
  {
  send_block( e );
  int rc = e->failed ? -1 : 0;
  free( e->buf ); free( e );
  return rc;
  }

AspDec *asp_dec_new( int channels, AspSource source, void *user )
  {
  if( channels < 1 || channels > ASP_CODEC_MAXCH ) return NULL;
  AspDec *d = calloc( 1, sizeof *d );
  if( ! d ) return NULL;
  d->buf = malloc( (size_t)CHUNK * channels * 4 );
  if( ! d->buf ) { free( d ); return NULL; }
  d->ch = channels; d->source = source; d->user = user;
  return d;
  }

int asp_dec_get( AspDec *d, int32_t *pcm, unsigned max )
  {
  size_t fb = (size_t)d->ch * 4;
  if( d->pos == d->have )                              /* nothing left over: ask for more, and keep only whole frames */
    {
    size_t got = 0;
    d->have = d->pos = 0;
    do
      {
      size_t k = (size_t)CHUNK * fb - got;
      if( d->source( d->buf + got, &k, d->user ) ) return -1;
      if( ! k ) return got ? -1 : 0;                   /* the end; bytes before it that make no frame are damage */
      got += k;
      }
    while( got < fb );
    while( got % fb )                                  /* a source may give less than asked: finish the frame */
      {
      size_t k = fb - got % fb;
      if( d->source( d->buf + got, &k, d->user ) || ! k ) return -1;
      got += k;
      }
    d->have = got;
    }
  unsigned k = (unsigned)( ( d->have - d->pos ) / fb );
  if( k > max ) k = max;
  for( size_t i = 0; i < (size_t)k * d->ch; i++ ) pcm[i] = (int32_t)asp_ld32( d->buf + d->pos + i * 4 );
  d->pos += (size_t)k * fb;
  return (int)k;
  }

int asp_dec_errors( const AspDec *d ) { (void)d; return 0; }
void asp_dec_free( AspDec *d ) { if( d ) { free( d->buf ); free( d ); } }
