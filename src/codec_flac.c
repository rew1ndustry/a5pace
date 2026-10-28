#include <stdlib.h>
#include <string.h>
#include "FLAC/stream_encoder.h"
#include "FLAC/stream_decoder.h"
#include "codec.h"

/* flac through libFLAC's stream interface only, as astream uses it: no file, no seek, no tell. 32 bit flac is outside the
   flac subset, so the encoder is told so. without a seek callback the header's length and md5 stay zero: a take's length
   is the caller's to keep. the stream is a legal flac stream all the same, and libFLAC reads it back with or without
   its header */

#define LEVEL 5

struct AspEnc { FLAC__StreamEncoder *f; AspSink sink; void *user; int failed; };

struct AspDec
  {
  FLAC__StreamDecoder *f;
  AspSource source; void *user;
  int ch, errors, failed;
  int32_t *buf; size_t cap, have, pos;                 /* the current block, interleaved */
  };

const char *asp_codec_name( void ) { return "flac"; }
int asp_codec_detects( void ) { return 1; }

static FLAC__StreamEncoderWriteStatus enc_write( const FLAC__StreamEncoder *f, const FLAC__byte *b, size_t n,
                                                 uint32_t samples, uint32_t frame, void *u )
  {
  AspEnc *e = u;
  (void)f;
  if( e->sink( b, n, samples, frame, e->user ) ) { e->failed = 1; return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR; }
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }

AspEnc *asp_enc_new( int channels, int rate, int block, int verify, AspSink sink, void *user )
  {
  if( channels < 1 || channels > ASP_CODEC_MAXCH || block < 16 || block > 65535 ) return NULL;
  AspEnc *e = calloc( 1, sizeof *e );
  if( ! e ) return NULL;
  e->sink = sink; e->user = user;
  e->f = FLAC__stream_encoder_new();
  int ok = e->f
    && FLAC__stream_encoder_set_channels( e->f, channels )
    && FLAC__stream_encoder_set_bits_per_sample( e->f, 32 )
    && FLAC__stream_encoder_set_sample_rate( e->f, rate )
    && FLAC__stream_encoder_set_compression_level( e->f, LEVEL )
    && FLAC__stream_encoder_set_blocksize( e->f, block )
    && FLAC__stream_encoder_set_streamable_subset( e->f, 0 )
    && FLAC__stream_encoder_set_verify( e->f, verify != 0 );
  if( ! ok || FLAC__stream_encoder_init_stream( e->f, enc_write, NULL, NULL, NULL, e ) != FLAC__STREAM_ENCODER_INIT_STATUS_OK )
    {
    if( e->f ) FLAC__stream_encoder_delete( e->f );
    free( e );
    return NULL;
    }
  return e;
  }

int asp_enc_put( AspEnc *e, const int32_t *pcm, unsigned frames )
  { return FLAC__stream_encoder_process_interleaved( e->f, (const FLAC__int32 *)pcm, frames ) && ! e->failed ? 0 : -1; }

int asp_enc_end( AspEnc *e )
  {
  int ok = FLAC__stream_encoder_finish( e->f ) && ! e->failed;      /* false if verify found a difference */
  FLAC__stream_encoder_delete( e->f );
  free( e );
  return ok ? 0 : -1;
  }

static FLAC__StreamDecoderReadStatus dec_read( const FLAC__StreamDecoder *f, FLAC__byte *b, size_t *n, void *u )
  {
  AspDec *d = u;
  (void)f;
  if( d->source( b, n, d->user ) ) { d->failed = 1; return FLAC__STREAM_DECODER_READ_STATUS_ABORT; }
  return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }

static FLAC__StreamDecoderWriteStatus dec_write( const FLAC__StreamDecoder *f, const FLAC__Frame *fr,
                                                 const FLAC__int32 *const b[], void *u )
  {
  AspDec *d = u;
  unsigned ch = fr->header.channels, n = fr->header.blocksize;
  (void)f;
  if( (int)ch != d->ch || fr->header.bits_per_sample != 32 ) { d->errors++; return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE; }
  if( (size_t)n * ch > d->cap )
    {
    int32_t *p = realloc( d->buf, (size_t)n * ch * 4 );
    if( ! p ) { d->failed = 1; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    d->buf = p; d->cap = (size_t)n * ch;
    }
  for( unsigned i = 0; i < n; i++ )
    for( unsigned k = 0; k < ch; k++ ) d->buf[(size_t)i * ch + k] = b[k][i];
  d->have = n; d->pos = 0;
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

static void dec_error( const FLAC__StreamDecoder *f, FLAC__StreamDecoderErrorStatus s, void *u )
  {
  AspDec *d = u;
  d->errors++;
  (void)f;
  if( s == FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC ) FLAC__stream_decoder_flush( d->f );
  }

AspDec *asp_dec_new( int channels, AspSource source, void *user )
  {
  if( channels < 1 || channels > ASP_CODEC_MAXCH ) return NULL;
  AspDec *d = calloc( 1, sizeof *d );
  if( ! d ) return NULL;
  d->ch = channels; d->source = source; d->user = user;
  d->f = FLAC__stream_decoder_new();
  if( ! d->f || FLAC__stream_decoder_init_stream( d->f, dec_read, NULL, NULL, NULL, NULL, dec_write, NULL, dec_error, d )
                != FLAC__STREAM_DECODER_INIT_STATUS_OK )
    {
    if( d->f ) FLAC__stream_decoder_delete( d->f );
    free( d );
    return NULL;
    }
  return d;
  }

int asp_dec_get( AspDec *d, int32_t *pcm, unsigned max )
  {
  while( d->pos == d->have )
    {
    d->have = d->pos = 0;
    if( d->failed ) return -1;
    if( FLAC__stream_decoder_get_state( d->f ) == FLAC__STREAM_DECODER_END_OF_STREAM ) return 0;
    if( ! FLAC__stream_decoder_process_single( d->f ) && FLAC__stream_decoder_get_state( d->f ) != FLAC__STREAM_DECODER_END_OF_STREAM )
      return -1;                                       /* a fatal read, write or memory error; a bad block is not that, it is counted */
    }
  unsigned k = (unsigned)( d->have - d->pos );
  if( k > max ) k = max;
  memcpy( pcm, d->buf + d->pos * d->ch, (size_t)k * d->ch * 4 );
  d->pos += k;
  return (int)k;
  }

int asp_dec_errors( const AspDec *d ) { return d->errors; }

void asp_dec_free( AspDec *d )
  {
  if( ! d ) return;
  FLAC__stream_decoder_delete( d->f );
  free( d->buf ); free( d );
  }
