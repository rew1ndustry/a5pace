/* codec: pcm <-> bytes, the only place that knows what the bytes are. aspace's takes and astream's wire both go
   through this; the build chooses the implementation (make CODEC=flac, the default, or CODEC=raw), and nothing
   outside src/codec_*.c includes a codec's own header. pcm is interleaved signed 32 bit, any channel count 1..8.

   the contract any codec keeps:
   - a put of one block (the block size given to asp_enc_new) makes exactly one sink call with samples > 0;
     a put of less is held until the block fills, and asp_enc_end sends what is held. so a tick put whole is a frame.
   - bytes the sink gets with samples == 0 are header: a decoder may do without them (astream does).
   - the decoder asks for bytes through the source and gives back one block at a time, in order. a damaged block
     is counted, skipped, and the next one decoded; asp_dec_errors says how many.
   world-readable files (flac as other programs write it) are not this module's business: see flacio.h. */
#ifndef CODEC_H
#define CODEC_H
#include <stddef.h>
#include <stdint.h>

#define ASP_CODEC_MAXCH 8

/* bytes out. samples: pcm frames in these bytes, 0 for header. frame: the codec's block number. non zero return aborts */
typedef int (*AspSink)( const unsigned char *bytes, size_t n, unsigned samples, uint32_t frame, void *user );
/* bytes in. fill up to *n bytes and set *n to what was given; 0 given is the end of the stream. non zero return is an error */
typedef int (*AspSource)( unsigned char *bytes, size_t *n, void *user );

typedef struct AspEnc AspEnc;
typedef struct AspDec AspDec;

const char *asp_codec_name( void );                 /* "flac", "raw": four letters at most, stored in the take header */
int         asp_codec_detects( void );              /* 1 if a damaged block is noticed (flac: crc), 0 if it cannot be (raw) */

/* block: pcm frames per block. verify: check each block by decoding it again (slower; for storage, not the wire) */
AspEnc *asp_enc_new( int channels, int rate, int block, int verify, AspSink sink, void *user );
int     asp_enc_put( AspEnc *e, const int32_t *pcm, unsigned frames );    /* 0 ok */
int     asp_enc_end( AspEnc *e );                                         /* flush, free; 0 ok, -1 if anything failed (verify too) */

AspDec *asp_dec_new( int channels, AspSource source, void *user );
/* up to max frames of the current block into pcm; 0 at the end of the stream; -1 if the source or the stream failed.
   a block is handed out over as many calls as max needs. */
int     asp_dec_get( AspDec *d, int32_t *pcm, unsigned max );
int     asp_dec_errors( const AspDec *d );                 /* damaged blocks met so far */
void    asp_dec_free( AspDec *d );
#endif
