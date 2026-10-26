/* aspflac: raw 32-bit stereo 48k pcm (.d32) <-> flac, with libFLAC only.
     aspflac enc IN.d32 OUT.flac [channels [rate]]
     aspflac dec IN.flac OUT.d32
     aspflac md5 IN.flac        prints the md5 of the decoded pcm bytes (little endian s32)
   32 bit flac is outside the flac subset, so the encoder is told so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FLAC/stream_encoder.h"
#include "FLAC/stream_decoder.h"
#include "FLAC/metadata.h"

#define CHUNK 65536

static int enc( const char *in, const char *out, int ch, int rate )
  {
  FILE *f = fopen( in, "rb" );
  if( ! f ) { perror( in ); return 1; }
  FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
  int ok = FLAC__stream_encoder_set_channels( e, ch )
    && FLAC__stream_encoder_set_bits_per_sample( e, 32 )
    && FLAC__stream_encoder_set_sample_rate( e, rate )
    && FLAC__stream_encoder_set_compression_level( e, 5 )
    && FLAC__stream_encoder_set_streamable_subset( e, 0 )
    && FLAC__stream_encoder_set_verify( e, 1 );
  if( ! ok ) { fprintf( stderr, "encoder setup failed\n" ); return 1; }
  FLAC__StreamEncoderInitStatus st = FLAC__stream_encoder_init_file( e, out, NULL, NULL );
  if( st != FLAC__STREAM_ENCODER_INIT_STATUS_OK )
    { fprintf( stderr, "%s: %s\n", out, FLAC__StreamEncoderInitStatusString[st] ); return 1; }
  static FLAC__int32 buf[CHUNK * 8];
  size_t got;
  while( ( got = fread( buf, 4 * ch, CHUNK, f ) ) > 0 )
    if( ! FLAC__stream_encoder_process_interleaved( e, buf, (unsigned)got ) )
      { fprintf( stderr, "encode: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state( e )] ); return 1; }
  ok = FLAC__stream_encoder_finish( e );
  FLAC__stream_encoder_delete( e );
  fclose( f );
  if( ! ok ) { fprintf( stderr, "finish failed (verify mismatch?)\n" ); return 1; }
  return 0;
  }

typedef struct { FILE *out; unsigned long long frames; int ch; } Dec;

static FLAC__StreamDecoderWriteStatus wr( const FLAC__StreamDecoder *d, const FLAC__Frame *fr,
                                          const FLAC__int32 *const b[], void *cd )
  {
  Dec *c = cd;
  unsigned n, k;
  static FLAC__int32 o[CHUNK * 8];
  (void)d;
  for( n = 0; n < fr->header.blocksize; n++ )
    for( k = 0; k < fr->header.channels; k++ )
      o[n * fr->header.channels + k] = b[k][n];
  if( fwrite( o, 4 * fr->header.channels, fr->header.blocksize, c->out ) != fr->header.blocksize )
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  c->frames += fr->header.blocksize;
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }
static void er( const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *cd )
  { (void)d; (void)cd; fprintf( stderr, "decode: %s\n", FLAC__StreamDecoderErrorStatusString[s] ); }

static int dec( const char *in, const char *out )
  {
  Dec c = { stdout, 0, 0 };
  if( strcmp( out, "-" ) && ! ( c.out = fopen( out, "wb" ) ) ) { perror( out ); return 1; }
  FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
  FLAC__stream_decoder_set_md5_checking( d, 1 );
  if( FLAC__stream_decoder_init_file( d, in, wr, NULL, er, &c ) != FLAC__STREAM_DECODER_INIT_STATUS_OK )
    { fprintf( stderr, "%s: cannot open\n", in ); return 1; }
  int ok = FLAC__stream_decoder_process_until_end_of_stream( d );
  ok = FLAC__stream_decoder_finish( d ) && ok;   /* false on md5 mismatch */
  FLAC__stream_decoder_delete( d );
  if( c.out != stdout ) fclose( c.out );
  if( ! ok ) { fprintf( stderr, "%s: decode or md5 check failed\n", in ); return 1; }
  return 0;
  }

static int md5( const char *in )
  {
  FLAC__StreamMetadata sm;
  if( ! FLAC__metadata_get_streaminfo( in, &sm ) ) { fprintf( stderr, "%s: no streaminfo\n", in ); return 1; }
  for( int i = 0; i < 16; i++ ) printf( "%02x", sm.data.stream_info.md5sum[i] );
  printf( " %llu\n", (unsigned long long)sm.data.stream_info.total_samples );
  return 0;
  }

int main( int argc, char **argv )
  {
  if( argc >= 4 && ! strcmp( argv[1], "enc" ) )
    return enc( argv[2], argv[3], argc > 4 ? atoi( argv[4] ) : 2, argc > 5 ? atoi( argv[5] ) : 48000 );
  if( argc == 4 && ! strcmp( argv[1], "dec" ) ) return dec( argv[2], argv[3] );
  if( argc == 3 && ! strcmp( argv[1], "md5" ) ) return md5( argv[2] );
  fprintf( stderr, "usage: aspflac enc IN.d32 OUT.flac [ch [rate]] | dec IN.flac OUT.d32 | md5 IN.flac\n" );
  return 2;
  }
