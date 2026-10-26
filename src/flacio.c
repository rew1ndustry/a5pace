#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FLAC/stream_encoder.h"
#include "FLAC/stream_decoder.h"
#include "FLAC/metadata.h"
#include "flacio.h"

#define CHUNK 65536

struct AspFlacEnc { FLAC__StreamEncoder *e; int ch; };

AspFlacEnc *asp_flac_enc_open( const char *path, int channels, int rate )
  {
  AspFlacEnc *x = calloc( 1, sizeof *x );
  if( ! x ) return NULL;
  x->ch = channels;
  x->e = FLAC__stream_encoder_new();
  int ok = x->e
    && FLAC__stream_encoder_set_channels( x->e, channels )
    && FLAC__stream_encoder_set_bits_per_sample( x->e, 32 )
    && FLAC__stream_encoder_set_sample_rate( x->e, rate )
    && FLAC__stream_encoder_set_compression_level( x->e, 5 )
    && FLAC__stream_encoder_set_streamable_subset( x->e, 0 )
    && FLAC__stream_encoder_set_verify( x->e, 1 );
  if( ! ok || FLAC__stream_encoder_init_file( x->e, path, NULL, NULL ) != FLAC__STREAM_ENCODER_INIT_STATUS_OK )
    {
    if( x->e ) FLAC__stream_encoder_delete( x->e );
    free( x );
    return NULL;
    }
  return x;
  }

int asp_flac_enc_write( AspFlacEnc *x, const int32_t *pcm, unsigned frames )
  { return FLAC__stream_encoder_process_interleaved( x->e, (const FLAC__int32 *)pcm, frames ) ? 0 : -1; }

int asp_flac_enc_close( AspFlacEnc *x )
  {
  int ok = FLAC__stream_encoder_finish( x->e );
  FLAC__stream_encoder_delete( x->e );
  free( x );
  return ok ? 0 : -1;
  }

int asp_flac_encode_file( const char *d32, const char *flac, int channels, int rate )
  {
  FILE *f = fopen( d32, "rb" );
  if( ! f ) return -1;
  AspFlacEnc *e = asp_flac_enc_open( flac, channels, rate );
  if( ! e ) { fclose( f ); return -1; }
  int32_t *buf = malloc( sizeof( int32_t ) * channels * CHUNK );
  size_t got;
  int rc = 0;
  while( ! rc && ( got = fread( buf, 4 * channels, CHUNK, f ) ) > 0 )
    {
    unsigned char *b = (unsigned char *)buf;
    size_t i;
    for( i = 0; i < got * channels; i++ )                  /* d32 is little endian on disk */
      buf[i] = (int32_t)( (uint32_t)b[4*i] | (uint32_t)b[4*i+1] << 8 | (uint32_t)b[4*i+2] << 16 | (uint32_t)b[4*i+3] << 24 );
    rc = asp_flac_enc_write( e, buf, (unsigned)got );
    }
  free( buf );
  fclose( f );
  return asp_flac_enc_close( e ) || rc ? -1 : 0;
  }

typedef struct { AspFlacFrames cb; void *user; int bad; } Dec;

static FLAC__StreamDecoderWriteStatus wr( const FLAC__StreamDecoder *d, const FLAC__Frame *fr,
                                          const FLAC__int32 *const b[], void *cd )
  {
  Dec *c = cd;
  unsigned n, k, ch = fr->header.channels, bs = fr->header.blocksize;
  static FLAC__int32 o[8 * CHUNK];                    /* a flac block is at most 65535 frames */
  (void)d;
  if( ch > 8 || bs > CHUNK ) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  for( n = 0; n < bs; n++ )
    for( k = 0; k < ch; k++ ) o[n * ch + k] = b[k][n];
  if( c->cb( (const int32_t *)o, bs, (int)ch, c->user ) ) { c->bad = 1; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }
static void er( const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *cd )
  { (void)d; (void)cd; fprintf( stderr, "flac decode: %s\n", FLAC__StreamDecoderErrorStatusString[s] ); }

int asp_flac_decode_frames( const char *flac, AspFlacFrames cb, void *user )
  {
  Dec c = { cb, user, 0 };
  FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
  FLAC__stream_decoder_set_md5_checking( d, 1 );
  int ok = FLAC__stream_decoder_init_file( d, flac, wr, NULL, er, &c ) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
  if( ok ) ok = FLAC__stream_decoder_process_until_end_of_stream( d );
  ok = FLAC__stream_decoder_finish( d ) && ok;      /* false on md5 mismatch */
  FLAC__stream_decoder_delete( d );
  return ok && ! c.bad ? 0 : -1;
  }

static int to_file( const int32_t *pcm, unsigned frames, int ch, void *u )
  {
  /* little endian on disk whatever the host */
  FILE *f = u;
  unsigned char b[4 * 8 * 256];
  unsigned n = 0, total = frames * ch;
  while( n < total )
    {
    unsigned k = total - n < 8 * 256 ? total - n : 8 * 256, i;
    for( i = 0; i < k; i++ )
      { uint32_t v = (uint32_t)pcm[n + i]; b[4*i] = v; b[4*i+1] = v >> 8; b[4*i+2] = v >> 16; b[4*i+3] = v >> 24; }
    if( fwrite( b, 4, k, f ) != k ) return -1;
    n += k;
    }
  return 0;
  }

int asp_flac_decode_file( const char *flac, const char *d32 )
  {
  FILE *f = strcmp( d32, "-" ) ? fopen( d32, "wb" ) : stdout;
  if( ! f ) return -1;
  int rc = asp_flac_decode_frames( flac, to_file, f );
  if( f == stdout ) fflush( stdout ); else if( fclose( f ) ) rc = -1;
  return rc;
  }

int asp_flac_info( const char *flac, unsigned char md5[16], uint64_t *frames,
                   int *channels, int *rate, int *bits )
  {
  FLAC__StreamMetadata sm;
  if( ! FLAC__metadata_get_streaminfo( flac, &sm ) ) return -1;
  if( md5 ) memcpy( md5, sm.data.stream_info.md5sum, 16 );
  if( frames ) *frames = sm.data.stream_info.total_samples;
  if( channels ) *channels = sm.data.stream_info.channels;
  if( rate ) *rate = sm.data.stream_info.sample_rate;
  if( bits ) *bits = sm.data.stream_info.bits_per_sample;
  return 0;
  }
