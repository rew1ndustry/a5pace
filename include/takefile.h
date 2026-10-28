/* takefile: one take on disk. a fixed header of facts about the recording, then the codec's bytes (codec.h), nothing else.
   written as it is recorded, never read whole unless a caller asks; the header is patched once, at the end, so a take
   that never ended is told by finished == 0 and still decodes as far as it got.
   the header holds metrics, not verdicts: whether a take is mono, and which channel, is worked out from them
   (asp_hdr_mono) by whoever opens it. all numbers little endian.
     0 "A5TK"  4 u16 version  6 u16 header size  8 codec[4]  12 u8 channels  13 u8 input  14 u16 flags
     16 u32 rate  20 u32 bits  24 u64 frames  32 artist[16]  48 u32 peak[8]  80 u32 clips[8]  112 u32 active[8]  144 reserved */
#ifndef TAKEFILE_H
#define TAKEFILE_H
#include <stdint.h>

#define ASP_HDR_SIZE 160
#define ASP_ARTIST   16

typedef struct
  {
  char     codec[5];
  int      channels, input, rate, bits;        /* input: how the card was told to be heard: 0 whatever comes, 1 left, 2 right, 3 both */
  int      finished, damaged, ident;           /* ident: the first two channels were equal throughout */
  uint64_t frames;
  unsigned char artist[ASP_ARTIST];
  uint32_t peak[8];                            /* largest |sample| per channel */
  uint32_t clips[8];                           /* samples at or beyond the clip level */
  uint32_t active[8];                          /* ticks the gate heard the channel */
  } AspHdr;

/* the one channel a take is to be heard from, or -1 for all as stored. input told: that channel (or both). otherwise,
   unless detect is off or the take was damaged: equal channels give 0; one channel active at least twenty times as
   often as the other gives that one */
int asp_hdr_mono( const AspHdr *h, int detect );

typedef struct AspTakeW AspTakeW;
AspTakeW *asp_tw_open( const char *path, const unsigned char artist[ASP_ARTIST], int channels, int rate, int input );
int  asp_tw_put( AspTakeW *w, const int32_t *pcm, unsigned frames );    /* interleaved; 0 ok */
void asp_tw_active( AspTakeW *w, unsigned mask );                       /* this tick, these channels (bit n) were heard */
int  asp_tw_close( AspTakeW *w, int damaged, AspHdr *out );             /* ends the codec, patches the header, fdatasync; 0 ok */
void asp_tw_abort( AspTakeW *w );                                       /* closes the file as it stands, header unfinished */

typedef struct AspTakeR AspTakeR;
AspTakeR *asp_tr_open( const char *path );                              /* NULL: not a take, or written with another codec */
const AspHdr *asp_tr_hdr( const AspTakeR *r );
int  asp_tr_get( AspTakeR *r, int32_t *pcm, unsigned max );             /* frames as stored; 0 at the end; -1 failed */
int  asp_tr_errors( const AspTakeR *r );
void asp_tr_close( AspTakeR *r );

/* a take through a view: src -1 gives the channels as stored, src n gives channel n alone. cb gets blocks; non zero aborts.
   -1 if the file is not a take, a block was damaged, or a finished take has not the frames its header says */
typedef int (*AspTakeFrames)( const int32_t *pcm, unsigned frames, int channels, void *user );
int asp_take_frames( const char *path, int src, AspTakeFrames cb, void *user );
int asp_take_load( const char *path, int src, int32_t **pcm, int64_t *frames, int *channels );   /* all of it, in memory */
int asp_take_header( const char *path, AspHdr *h );
#endif
