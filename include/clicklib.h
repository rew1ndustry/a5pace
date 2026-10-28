/* clicklib: time signatures, click tracks, and audio files in. a click track is a take like any other: generated to
   fit a loop's length exactly and inserted into a session that is not running. */
#ifndef ASPCLICK_H
#define ASPCLICK_H
#include <stdint.h>
#include "aspstore.h"
#include "asptank.h"

typedef struct { int n, d; double bpm; int bars; } AspMeter;     /* bpm counts quarter notes a minute */
typedef struct { int64_t at; int strength; } AspClickEv;         /* 0 accent, 1 beat, 2 subdivision */

int    asp_meter_parse( const char *sig, AspMeter *m );          /* "4/4", "6/8", "7/8": 0 ok */
double asp_meter_quarters( const AspMeter *m );                  /* quarter notes in all the bars */
double asp_meter_seconds( const AspMeter *m );                   /* how long they last at m->bpm */
int    asp_meter_beats( const AspMeter *m );                     /* clicks in a bar: 4/4 4, 6/8 2 (dotted quarters), 7/8 7 */
/* the events of a style over a loop of loop_frames, the bars spread evenly across it (so the tempo is whatever fits) */
int    asp_click_events( const AspMeter *m, const char *style, int64_t loop_frames, AspClickEv *ev, int max );
extern const char *asp_click_styles;                             /* "beats bars eighths triplets sixteenths offbeat" */

/* the three sounds: synthesised, or from a mono mix of an audio file */
typedef struct { int32_t *pcm[3]; int64_t len[3]; } AspClickSounds;
void   asp_click_sounds_default( AspClickSounds *s );
int    asp_click_sound_file( AspClickSounds *s, int which, const char *path, char *err, int errn );
void   asp_click_sounds_free( AspClickSounds *s );
/* mono pcm for the loop (malloc'd, loop_frames long); sounds mix where they overlap */
int32_t *asp_click_render( const AspClickEv *ev, int nev, int64_t loop_frames, const AspClickSounds *s );

/* an audio file (wav: pcm 16/24/32 or float 32; flac): interleaved int32 left justified, channels 1 or 2, rate 48000 */
int    asp_audio_read( const char *path, int32_t **pcm, int64_t *frames, int *channels, int *rate, char *err, int errn );

/* make a mono click take in a session that is not running, at lev l r (shifts; 99 = off). iter is in the 90000 series (asp_gen_next_iter). in puts it into
   the loop as well; otherwise it is only kept (state off) until someone plays it. returns the take id or -1 */
int64_t asp_click_insert( AspStore *s, AspTank *t, int64_t iter, int32_t *mono, int64_t frames, const char *style, int lev_l, int lev_r, const char *meta, int in );
/* for a session that is running: the click as a flac under DIR/blobs/tmp, name returned; the running aspace adopts it (CLICK ADD) */
int64_t asp_import_insert( AspStore *s, AspTank *t, int64_t iter, const int32_t *pcm, int64_t frames, int channels, int lev_l, int lev_r, int in );
int    asp_pcm_write_part( const char *dir, const int32_t *pcm, int64_t frames, int channels, char *name, size_t n );
int64_t asp_gen_next_iter( AspStore *s );
int    asp_click_write_part( const char *dir, const int32_t *mono, int64_t frames, char *name, size_t n );
/* take every generated click out of the loop (state off) */
int    asp_click_clear( AspStore *s, AspTank *t );
#endif
