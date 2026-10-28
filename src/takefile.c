#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "takefile.h"
#include "codec.h"
#include "asptank.h"
#include "plat.h"

#define VERSION   1
#define CLIP      0x7FFFFF00u         /* a sample this loud is clipping */
#define SYNC_EVERY (1 << 20)          /* bytes written before the kernel is asked to start writing them out */

int asp_hdr_mono( const AspHdr *h, int detect )
  {
  if( h->channels < 2 || h->channels > 2 ) return -1;
  if( h->input == 1 ) return 0;
  if( h->input == 2 ) return 1;
  if( h->input == 3 || ! detect || h->damaged ) return -1;
  uint64_t a = h->active[0], b = h->active[1];
  if( h->ident ) return 0;
  if( a > 0 && b * 20 <= a ) return 0;
  if( b > 0 && a * 20 <= b ) return 1;
  return -1;
  }

static void hdr_put( unsigned char *b, const AspHdr *h )
  {
  memset( b, 0, ASP_HDR_SIZE );
  memcpy( b, "A5TK", 4 ); b[4] = VERSION; b[6] = ASP_HDR_SIZE;
  memcpy( b + 8, h->codec, 4 );
  b[12] = (unsigned char)h->channels; b[13] = (unsigned char)h->input;
  b[14] = (unsigned char)( ( h->finished ? 1 : 0 ) | ( h->damaged ? 2 : 0 ) | ( h->ident ? 4 : 0 ) );
  asp_st32( b + 16, (uint32_t)h->rate ); asp_st32( b + 20, (uint32_t)h->bits ); asp_st64( b + 24, h->frames );
  memcpy( b + 32, h->artist, ASP_ARTIST );
  for( int i = 0; i < 8; i++ )
    { asp_st32( b + 48 + 4 * i, h->peak[i] ); asp_st32( b + 80 + 4 * i, h->clips[i] ); asp_st32( b + 112 + 4 * i, h->active[i] ); }
  }

static int hdr_get( const unsigned char *b, AspHdr *h )
  {
  memset( h, 0, sizeof *h );
  if( memcmp( b, "A5TK", 4 ) || b[4] != VERSION || b[6] != ASP_HDR_SIZE || b[12] < 1 || b[12] > ASP_CODEC_MAXCH ) return -1;
  memcpy( h->codec, b + 8, 4 );
  h->channels = b[12]; h->input = b[13];
  h->finished = b[14] & 1; h->damaged = ( b[14] >> 1 ) & 1; h->ident = ( b[14] >> 2 ) & 1;
  h->rate = (int)asp_ld32( b + 16 ); h->bits = (int)asp_ld32( b + 20 ); h->frames = asp_ld64( b + 24 );
  memcpy( h->artist, b + 32, ASP_ARTIST );
  for( int i = 0; i < 8; i++ )
    { h->peak[i] = asp_ld32( b + 48 + 4 * i ); h->clips[i] = asp_ld32( b + 80 + 4 * i ); h->active[i] = asp_ld32( b + 112 + 4 * i ); }
  return 0;
  }

/* --- writing --- */

struct AspTakeW { int fd, failed; AspEnc *enc; AspHdr h; off_t at, synced; };

static int tw_sink( const unsigned char *b, size_t n, unsigned samples, uint32_t frame, void *u )
  {
  AspTakeW *w = u;
  (void)samples; (void)frame;
  while( n )
    {
    ssize_t k = write( w->fd, b, n );
    if( k < 0 && errno == EINTR ) continue;
    if( k <= 0 ) { w->failed = 1; return -1; }
    b += k; n -= (size_t)k; w->at += k;
    }
#ifdef __linux__
  if( w->at - w->synced >= SYNC_EVERY )                /* start the write-out now, so the last sync has little left to do */
    { sync_file_range( w->fd, w->synced, w->at - w->synced, SYNC_FILE_RANGE_WRITE ); w->synced = w->at; }
#endif
  return 0;
  }

AspTakeW *asp_tw_open( const char *path, const unsigned char artist[ASP_ARTIST], int channels, int rate, int input )
  {
  if( channels < 1 || channels > ASP_CODEC_MAXCH ) return NULL;
  AspTakeW *w = calloc( 1, sizeof *w );
  if( ! w ) return NULL;
  snprintf( w->h.codec, sizeof w->h.codec, "%.4s", asp_codec_name() );
  w->h.channels = channels; w->h.rate = rate; w->h.bits = 32; w->h.input = input; w->h.ident = channels > 1;
  memcpy( w->h.artist, artist, ASP_ARTIST );
  /* a file that is not there is made; one that is there must be empty (a spare the keeper made ahead of time): never over a take */
  struct stat sb;
  w->fd = open( path, O_WRONLY | O_CREAT, 0644 );
  if( w->fd >= 0 && ( fstat( w->fd, &sb ) || sb.st_size != 0 ) ) { close( w->fd ); free( w ); return NULL; }
  unsigned char b[ASP_HDR_SIZE];
  hdr_put( b, &w->h );
  if( w->fd < 0 || tw_sink( b, ASP_HDR_SIZE, 0, 0, w ) || ! ( w->enc = asp_enc_new( channels, rate, ASP_TICK, 1, tw_sink, w ) ) )
    { if( w->fd >= 0 ) { close( w->fd ); unlink( path ); } free( w ); return NULL; }
  return w;
  }

int asp_tw_put( AspTakeW *w, const int32_t *pcm, unsigned frames )
  {
  int ch = w->h.channels;
  for( size_t i = 0; i < (size_t)frames * ch; i++ )
    {
    uint32_t v = pcm[i] < 0 ? 0u - (uint32_t)pcm[i] : (uint32_t)pcm[i];     /* |INT32_MIN| is 2^31, which fits */
    int k = (int)( i % ch );
    if( v > w->h.peak[k] ) w->h.peak[k] = v;
    if( v >= CLIP ) w->h.clips[k]++;
    }
  if( w->h.ident )
    for( unsigned i = 0; i < frames; i++ ) if( pcm[(size_t)i * ch] != pcm[(size_t)i * ch + 1] ) { w->h.ident = 0; break; }
  w->h.frames += frames;
  return asp_enc_put( w->enc, pcm, frames ) || w->failed ? -1 : 0;
  }

void asp_tw_active( AspTakeW *w, unsigned mask )
  { for( int k = 0; k < w->h.channels; k++ ) if( mask >> k & 1 ) w->h.active[k]++; }

int asp_tw_close( AspTakeW *w, int damaged, AspHdr *out )
  {
  int bad = asp_enc_end( w->enc ) || w->failed;
  unsigned char b[ASP_HDR_SIZE];
  w->h.finished = ! bad; w->h.damaged = damaged;
  hdr_put( b, &w->h );
  if( pwrite( w->fd, b, ASP_HDR_SIZE, 0 ) != ASP_HDR_SIZE ) bad = 1;              /* not synced: whoever adopts the take settles it */
  if( close( w->fd ) ) bad = 1;
  if( out ) *out = w->h;
  free( w );
  return bad ? -1 : 0;
  }

void asp_tw_abort( AspTakeW *w )
  {
  asp_enc_end( w->enc );
  close( w->fd );
  free( w );
  }

/* --- reading --- */

struct AspTakeR { int fd; AspDec *dec; AspHdr h; };

static int tr_source( unsigned char *b, size_t *n, void *u )
  {
  AspTakeR *r = u;
  ssize_t k;
  do k = read( r->fd, b, *n ); while( k < 0 && errno == EINTR );
  if( k < 0 ) return -1;
  *n = (size_t)k;
  return 0;
  }

AspTakeR *asp_tr_open( const char *path )
  {
  AspTakeR *r = calloc( 1, sizeof *r );
  unsigned char b[ASP_HDR_SIZE];
  if( ! r ) return NULL;
  r->fd = open( path, O_RDONLY );
#ifdef POSIX_FADV_SEQUENTIAL
  if( r->fd >= 0 ) posix_fadvise( r->fd, 0, 0, POSIX_FADV_SEQUENTIAL );     /* read ahead: a slow card is asked early, not waited for */
#endif
  if( r->fd < 0 || read( r->fd, b, ASP_HDR_SIZE ) != ASP_HDR_SIZE || hdr_get( b, &r->h ) || strncmp( r->h.codec, asp_codec_name(), 4 )
      || ! ( r->dec = asp_dec_new( r->h.channels, tr_source, r ) ) )
    { if( r->fd >= 0 ) close( r->fd ); free( r ); return NULL; }
  return r;
  }

const AspHdr *asp_tr_hdr( const AspTakeR *r ) { return &r->h; }
int asp_tr_get( AspTakeR *r, int32_t *pcm, unsigned max ) { return asp_dec_get( r->dec, pcm, max ); }
int asp_tr_errors( const AspTakeR *r ) { return asp_dec_errors( r->dec ); }
void asp_tr_close( AspTakeR *r ) { if( r ) { asp_dec_free( r->dec ); close( r->fd ); free( r ); } }

int asp_take_header( const char *path, AspHdr *h )
  {
  AspTakeR *r = asp_tr_open( path );
  if( ! r ) return -1;
  *h = r->h;
  asp_tr_close( r );
  return 0;
  }

int asp_take_frames( const char *path, int src, AspTakeFrames cb, void *user )
  {
  AspTakeR *r = asp_tr_open( path );
  if( ! r ) return -1;
  int ch = r->h.channels, out = src >= 0 ? 1 : ch, n = 0, rc = 0;
  uint64_t total = 0;
  int32_t *in = malloc( (size_t)ASP_TICK * ch * 4 ), *one = src >= 0 ? malloc( (size_t)ASP_TICK * 4 ) : NULL;
  if( ! in || ( src >= 0 && ( ! one || src >= ch ) ) ) rc = -1;
  while( ! rc && ( n = asp_tr_get( r, in, ASP_TICK ) ) > 0 )
    {
    const int32_t *p = in;
    if( src >= 0 ) { for( int i = 0; i < n; i++ ) one[i] = in[(size_t)i * ch + src]; p = one; }
    if( cb( p, (unsigned)n, out, user ) ) rc = -1;
    total += (uint64_t)n;
    }
  if( n < 0 || asp_tr_errors( r ) || ( r->h.finished && total != r->h.frames ) ) rc = -1;
  free( in ); free( one );
  asp_tr_close( r );
  return rc;
  }

typedef struct { int32_t *p; int64_t n, cap; int ch; } Pcm;

static int pcm_add( const int32_t *pcm, unsigned frames, int ch, void *u )
  {
  Pcm *b = u;
  b->ch = ch;
  if( b->n + frames > b->cap )
    {
    int64_t cap = b->cap ? b->cap * 2 : 1 << 16;
    while( cap < b->n + frames ) cap *= 2;
    int32_t *q = realloc( b->p, (size_t)cap * ch * 4 );
    if( ! q ) return -1;
    b->p = q; b->cap = cap;
    }
  memcpy( b->p + (size_t)b->n * ch, pcm, (size_t)frames * ch * 4 );
  b->n += frames;
  return 0;
  }

int asp_take_load( const char *path, int src, int32_t **pcm, int64_t *frames, int *channels )
  {
  Pcm b = { NULL, 0, 0, 0 };
  AspHdr h;
  if( asp_take_header( path, &h ) ) return -1;
  if( asp_take_frames( path, src, pcm_add, &b ) ) { free( b.p ); return -1; }
  *pcm = b.p; *frames = b.n; *channels = src >= 0 ? 1 : h.channels;
  return 0;
  }
