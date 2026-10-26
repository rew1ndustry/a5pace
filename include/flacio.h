/* flacio: pcm <-> flac through libFLAC only. pcm is interleaved signed 32 bit. */
#ifndef FLACIO_H
#define FLACIO_H
#include <stdint.h>

typedef struct AspFlacEnc AspFlacEnc;

/* 32 bit flac is outside the flac subset; the encoder is told so */
AspFlacEnc *asp_flac_enc_open( const char *path, int channels, int rate );
int  asp_flac_enc_write( AspFlacEnc *e, const int32_t *pcm, unsigned frames );
int  asp_flac_enc_close( AspFlacEnc *e );   /* 0 ok; non zero if verify failed */

int  asp_flac_encode_file( const char *d32, const char *flac, int channels, int rate );
/* decode, calling cb for each block of interleaved frames; non zero from cb aborts */
typedef int (*AspFlacFrames)( const int32_t *pcm, unsigned frames, int channels, void *user );
int  asp_flac_decode_frames( const char *flac, AspFlacFrames cb, void *user );
/* decode to a raw .d32 (or "-" for stdout); checks the stored md5 */
int  asp_flac_decode_file( const char *flac, const char *d32 );
/* streaminfo: md5 of the decoded pcm bytes (16) and frames */
int  asp_flac_info( const char *flac, unsigned char md5[16], uint64_t *frames,
                    int *channels, int *rate, int *bits );
#endif
