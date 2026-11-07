/* sync: finds the round-trip latency by sending a coded tone out and reading it back.
   one second at a time: sync_out gives the second to play, sync_in takes the second that was
   recorded while it played. the shell/edge does the playing, and the mixer work (loopback on,
   monitor muted) around it; see docs/EDGE.md. */
#ifndef ASPSYNC_H
#define ASPSYNC_H
#include <stdint.h>

#define ASP_SYNC_FRAMES 48000                    /* one second, stereo int32 */

typedef struct
  {
  int    start_shift; /* first output: asp4's sync tone right-shifted this many bits (one bit is about 6 dB). default 8 */
  int    loudest;     /* never shifted less than this (0 = asp4's level). default 0 */
  int    step_shift;  /* bits it comes up each second that is not heard. default 1 */
  int    target_shift;/* wanted input peak once heard, in bits below full scale. default 2 */
  int    need;        /* readings whose median is the answer. default 3 */
  int    tol;         /* widest spread among them, in samples. default 2 */
  int    give_up;     /* seconds before giving up, 0 never. default 60 */
  int    output_muted;/* the shell has muted what is heard: no need to start quietly */
  } AspSyncCfg;

typedef struct
  {
  int64_t frames;     /* round trip, output to input, in frames (may exceed one second) */
  int     inverted;   /* the path inverts the signal */
  int     channel;    /* 0 left, 1 right: the input channel read */
  int     out_shift;  /* output shift it settled at, bits below asp4's */
  int     in_peak_shift; /* input peak, bits below full scale, at that level */
  int     seconds;    /* seconds taken */
  int     spread;     /* widest difference among the readings used */
  } AspSyncResult;

typedef struct AspSync AspSync;
void     asp_sync_defaults( AspSyncCfg *c );
AspSync *asp_sync_new( const AspSyncCfg *c );          /* NULL for the defaults */
void     asp_sync_free( AspSync *s );
/* the second to play now: ASP_SYNC_FRAMES stereo frames */
void     asp_sync_out( AspSync *s, int32_t *out );
/* the second just recorded. 0 still looking, 1 found (see result), -1 gave up */
int      asp_sync_in( AspSync *s, const int32_t *in );
const AspSyncResult *asp_sync_result( const AspSync *s );
int      asp_sync_level( const AspSync *s );           /* output shift now, bits below asp4's */

/* asp4's own routines, for the differential test: out_asp4 is asp4's putSync at full level; find_asp4
   is findSync with asp4's alternating polarity (inverted on odd seconds) */
void     asp_sync_out_asp4( int32_t *out, int second );
int64_t  asp_sync_find_asp4( const int32_t *in, int second );
#endif
