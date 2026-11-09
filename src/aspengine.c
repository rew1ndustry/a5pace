#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "aspengine.h"
#include "flacio.h"
#include "takefile.h"

/* --- pcm in memory --- */

typedef struct { int32_t *p; int64_t n, cap; int ch; } Pcm;

static int pcm_add( const int32_t *pcm, unsigned frames, int ch, void *u )
  {
  Pcm *b = u;
  if( ! b->ch ) b->ch = ch;
  if( ch != b->ch ) return -1;
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

int asp_pcm_load( const char *flac, int32_t **pcm, int64_t *frames, int *channels )
  {
  Pcm b = { NULL, 0, 0, 0 };
  if( asp_flac_decode_frames( flac, pcm_add, &b ) ) { free( b.p ); return -1; }
  *pcm = b.p; *frames = b.n; *channels = b.ch ? b.ch : 2;
  return 0;
  }

/* largest |sample| of a take: of the channel src, or of all. the recording kept it in the header */
int asp_take_peak( AspStore *s, int64_t id, int src, int64_t *peak )
  {
  AspTake k; char path[1200]; AspHdr h; uint32_t pk = 0;
  if( asp_take_get( s, id, &k ) || asp_take_path( s, &k, path, sizeof path ) || asp_take_header( path, &h ) || src >= h.channels ) return -1;
  for( int i = 0; i < h.channels; i++ ) if( ( src < 0 || src == i ) && h.peak[i] > pk ) pk = h.peak[i];
  *peak = pk;
  return 0;
  }

/* --- jobs --- */

/* a job reads its take as a stream, a block at a time, and never holds more than one block: a take may be hours long.
   it visits the take's frames in order; frame f lies at tank position pos0 + f (the tank wraps). a take longer than
   the loop therefore reaches each position more than once, a loop-length apart, and a change of level arrives at
   those a revolution at a time rather than all at once: the sum is the same, exactly. */
struct AspJob
  {
  AspTakeR *r;
  int64_t pos0, lo, hi, cursor;   /* cursor: the next frame of the take to move (lo .. hi) */
  int64_t base, have, at;         /* frame index of the block's first frame; frames in it; how many are used */
  int32_t *buf;                   /* the block, as stored */
  int64_t *offs; int64_t noff, capoff;   /* the stream offset of each tick met so far, stacked as the take is streamed: tick k starts at offs[k] */
  int ch;                         /* channels as stored */
  int osrc, nsrc;                 /* a side that is one channel of a take stored with more: which, or -1 for all as they are */
  AspPlace from, to;
  };

AspJob *asp_job_open( const char *path, int64_t pos0, int64_t lo, int64_t hi, AspPlace from, int osrc, AspPlace to, int nsrc )
  {
  AspTakeR *r = asp_tr_open( path );
  if( ! r ) return NULL;
  const AspHdr *h = asp_tr_hdr( r );
  if( ! h->finished || osrc >= h->channels || nsrc >= h->channels ) { asp_tr_close( r ); return NULL; }
  AspJob *j = calloc( 1, sizeof *j );
  j->buf = malloc( (size_t)ASP_TICK * h->channels * 4 );
  if( ! j || ! j->buf ) { free( j ? j->buf : NULL ); free( j ); asp_tr_close( r ); return NULL; }
  j->r = r; j->ch = h->channels; j->pos0 = pos0;
  j->lo = lo < 0 ? 0 : lo;
  j->hi = hi < 0 || hi > (int64_t)h->frames ? (int64_t)h->frames : hi;
  j->cursor = j->lo;
  j->from = from; j->to = to; j->osrc = osrc; j->nsrc = nsrc;
  return j;
  }

/* a take of the store, through a view: src is the one channel of it that is heard (mono), or -1 for the channels as stored;
   each side has its own */
AspJob *asp_job_take( AspStore *s, int64_t id, int64_t lo, int64_t hi, AspPlace from, int osrc, AspPlace to, int nsrc )
  {
  AspTake k; char path[1200]; AspJob *j = NULL;
  for( int tries = 0; tries < 2 && ! j; tries++ )          /* a take on its way to its place may move between asking where it is and opening it */
    {
    if( asp_take_get( s, id, &k ) || asp_take_path( s, &k, path, sizeof path ) ) return NULL;
    j = asp_job_open( path, k.off * k.rate, lo, hi, from, osrc, to, nsrc );
    }
  return j;
  }

void asp_job_free( AspJob *j ) { if( j ) { asp_tr_close( j->r ); free( j->buf ); free( j->offs ); free( j ); } }
int64_t asp_job_span( const AspJob *j ) { return j->hi - j->lo; }
int64_t asp_job_pos0( const AspJob *j ) { return j->pos0; }

int asp_place_eq( AspPlace a, AspPlace b )
  {
  if( a.mixed != b.mixed ) return 0;
  if( a.mixed ) return a.gsh == b.gsh && a.pan == b.pan;
  return a.l == b.l && a.r == b.r && a.flip == b.flip;
  }

/* a mixed placement is two shifts: gsh is the gain in bits (+N: every sample left-shifted N bits, louder, saturating
   in the tank; -N: right-shifted N bits, quieter), pan N steps: +N right (the left side N bits quieter and the right side N bits louder), -N left (the mirror). a step is balanced, one shift in each channel; gain and pan are summed per channel, then applied once. no multiplies, no dB. a stereo take's
   pan is kept but does nothing yet. */
static int clampsh( int v ) { return v < ASP_LEV_MIN ? ASP_LEV_MIN : v > ASP_LEV_MAX ? ASP_LEV_MAX : v; }

void asp_place_shifts( AspPlace p, int mono, int *sl, int *sr )
  {
  int pan = mono ? p.pan : 0;
  *sl = clampsh( -p.gsh + pan );                              /* the tank's levels are right shifts: louder is negative */
  *sr = clampsh( -p.gsh - pan );
  }

void asp_place_to_mix( AspPlace p, int mono, int *gsh, int *pan )
  {
  if( p.mixed ) { *gsh = p.gsh; *pan = p.pan; return; }
  int l = p.l == ASP_LEV_OFF ? ASP_LEV_MAX : p.l, r = p.r == ASP_LEV_OFF ? ASP_LEV_MAX : p.r;
  if( ! mono ) { *gsh = -( l < r ? l : r ); *pan = 0; return; }
  *pan = ( l - r ) / 2; *gsh = *pan - l;                        /* gain is the mean of the two, pan half the difference (an odd difference is kept to within a bit) */
  }

static inline void contrib( const int32_t *p, int ch, int src, AspPlace pl, int sign, int64_t *m )
  {
  int one = ch == 1 || src >= 0;                          /* a single channel is heard as mono */
  int64_t a = p[src >= 0 ? src : 0], b = one ? a : p[1];
  if( pl.mixed )
    {
    int sl, sr;
    asp_place_shifts( pl, one, &sl, &sr );
    m[0] += sign * asp_level( a, sl ); m[1] += sign * asp_level( b, sr );
    return;
    }
  int64_t s0 = pl.flip ? b : a, s1 = pl.flip ? a : b;
  m[0] += sign * asp_level( s0, pl.l );
  m[1] += sign * asp_level( s1, pl.r );
  }

int64_t asp_job_cursor( const AspJob *j ) { return j->cursor; }

int asp_job_retarget( AspJob *j, AspPlace to )
  {
  if( j->cursor > j->lo ) return -1;
  j->to = to;
  return asp_place_eq( j->from, j->to ) && j->osrc == j->nsrc ? 1 : 0;
  }

/* move up to budget frames (decoding the ones below lo counts), not past frame lo + limit of the take (limit < 0: no limit).
   1 done, 0 more to do, -1 failed */
int asp_job_step_to( AspJob *j, AspTank *t, int64_t budget, int64_t limit )
  {
  int64_t end = j->hi;
  if( limit >= 0 && j->lo + limit < end ) end = j->lo + limit;
  while( j->cursor < j->hi && budget > 0 )
    {
    if( j->at == j->have )                                     /* the block is used up: the next */
      {
      if( j->cursor >= end ) break;
      if( ( j->base + j->have ) / ASP_TICK == j->noff )          /* a tick not met before: where it starts, to come back to */
        {
        int64_t at = asp_tr_tell( j->r );
        if( at >= 0 )
          {
          if( j->noff == j->capoff )
            {
            int64_t *o = realloc( j->offs, (size_t)( j->capoff ? 2 * j->capoff : 256 ) * sizeof *o );
            if( ! o ) return -1;
            j->offs = o; j->capoff = j->capoff ? 2 * j->capoff : 256;
            }
          j->offs[j->noff++] = at;
          }
        }
      int n = asp_tr_get( j->r, j->buf, ASP_TICK );
      if( n < 0 ) return -1;
      if( n == 0 ) return -1;                                  /* it ended before the frames its header counts */
      j->base += j->have; j->have = n; j->at = 0;
      }
    int64_t f = j->base + j->at, k = j->have - j->at;
    if( f < j->lo ) { if( k > j->lo - f ) k = j->lo - f; j->at += k; budget -= k; continue; }       /* before the part asked for */
    if( f >= end ) break;
    if( k > end - f ) k = end - f;
    if( k > budget ) k = budget;
    int64_t mix[2 * ASP_TICK];
    if( asp_tank_get( t, j->pos0 + f, k, mix ) ) return -1;
    for( int64_t i = 0; i < k; i++ )
      {
      const int32_t *p = j->buf + ( j->at + i ) * j->ch;
      contrib( p, j->ch, j->osrc, j->from, -1, mix + 2 * i );
      contrib( p, j->ch, j->nsrc, j->to, 1, mix + 2 * i );
      }
    if( asp_tank_put( t, j->pos0 + f, k, mix ) ) return -1;
    j->at += k; j->cursor = f + k; budget -= k;
    }
  if( j->cursor < j->hi ) return 0;
  return asp_tr_errors( j->r ) ? -1 : 1;                       /* a damaged block is a failed move */
  }

/* go on from the start of tick k of the take, from the stacked offset, with no decoding of what lies before. 1 if the job has not
   streamed that far yet (go on forward: it stacks as it goes), -1 if the tick is before the part it was asked for or after the end */
int asp_job_goto( AspJob *j, int64_t k )
  {
  if( k < 0 || k * ASP_TICK < j->lo || k * ASP_TICK >= j->hi ) return -1;
  if( k >= j->noff ) return 1;
  if( asp_tr_seek( j->r, j->offs[k] ) ) return -1;
  j->base = k * ASP_TICK; j->have = j->at = 0; j->cursor = k * ASP_TICK;
  return 0;
  }

int asp_job_step( AspJob *j, AspTank *t, int64_t budget ) { return asp_job_step_to( j, t, budget, -1 ); }

int asp_move_take( AspTank *t, AspStore *s, int64_t id, int src, int64_t lo, int64_t hi, AspPlace from, AspPlace to )
  {
  AspJob *j = asp_job_take( s, id, lo, hi, from, src, to, src );
  if( ! j ) return -1;
  int rc = asp_job_step( j, t, INT64_MAX );
  asp_job_free( j );
  return rc < 0 ? -1 : 0;
  }

int asp_apply_take( AspTank *t, AspStore *s, int64_t id, int64_t lo, int64_t hi, int ol, int orr, int nl, int nr )
  { return asp_move_take( t, s, id, -1, lo, hi, (AspPlace){ ol, orr, 0 }, (AspPlace){ nl, nr, 0 } ); }

int asp_tank_clear( AspTank *t )
  {
  int64_t z[ASP_TICK * 2] = { 0 }, n = asp_tank_frames( t ), pos = 0;
  for( ; n > 0; n -= ASP_TICK, pos += ASP_TICK )
    if( asp_tank_put( t, pos, n < ASP_TICK ? n : ASP_TICK, z ) ) return -1;
  return asp_tank_set_head( t, asp_tank_iter( t ), 0 );
  }

static int lev_of( const char *s, const char *end )
  {
  char b[16]; int n = 0;
  for( ; s < end && n < 15; s++ ) if( ! isspace( (unsigned char)*s ) && *s != '"' ) b[n++] = *s;
  b[n] = 0;
  if( ! n || ! strcmp( b, "-" ) || ! strcmp( b, "null" ) || ! strcmp( b, "off" ) ) return ASP_LEV_OFF;
  return atoi( b );
  }

static int64_t num_after( const char *j, const char *key, int64_t dflt )
  {
  const char *p = strstr( j, key );
  if( ! p ) return dflt;
  p += strlen( key );
  if( *p == '"' ) p++;
  return ( isdigit( (unsigned char)*p ) || *p == '-' ) ? atoll( p ) : dflt;
  }

void asp_parse_state( const char *j, AspTakeState *st )
  {
  const char *p;
  strcpy( st->status, "legacy" ); st->l = st->r = 0; st->flip = st->mono = st->src = 0; st->gone = 0; st->peak = -1; st->len = -1; st->gsh = st->pan = st->mixed = 0;
  if( ( p = strstr( j, "\"status\":\"" ) ) )
    {
    int n = 0; p += 10;
    while( *p && *p != '"' && n < 7 ) st->status[n++] = *p++;
    st->status[n] = 0;
    }
  if( ( p = strstr( j, "\"lev\":[" ) ) )
    {
    const char *c = strchr( p, ',' ), *e = strchr( p, ']' );
    if( c && e && c < e ) { st->l = lev_of( p + 7, c ); st->r = lev_of( c + 1, e ); }
    }
  st->gone = num_after( j, "\"gone\":", 0 );
  st->flip = (int)num_after( j, "\"flip\":", 0 );
  st->mono = (int)num_after( j, "\"mono\":", 0 ); st->src = (int)num_after( j, "\"src\":", 0 );
  st->peak = num_after( j, "\"peak\":", -1 );
  st->len = num_after( j, "\"len\":", -1 );
  st->mixed = (int)num_after( j, "\"mixed\":", 0 ); st->gsh = (int)num_after( j, "\"gsh\":", 0 ); st->pan = (int)num_after( j, "\"pan\":", 0 );
  }

typedef struct { int64_t id; AspTakeState st; } Ent;
typedef struct { Ent *e; int n, cap; } List;

static void collect( int64_t id, const char *json, void *u )
  {
  List *l = u;
  if( l->n == l->cap ) { l->cap = l->cap ? 2 * l->cap : 64; l->e = realloc( l->e, l->cap * sizeof *l->e ); }
  l->e[l->n].id = id;
  asp_parse_state( json, &l->e[l->n].st );
  l->n++;
  }

int asp_rebuild( AspTank *t, AspStore *s, int64_t upto )
  {
  List l = { 0 };
  int rc = asp_fold( s, upto, collect, &l );
  if( ! rc ) rc = asp_tank_clear( t );
  for( int i = 0; ! rc && i < l.n; i++ )
    {
    AspTakeState *st = &l.e[i].st;
    if( l.e[i].id == 0 || ! strcmp( st->status, "off" ) ) continue;   /* take 0 holds settings, not a take; or not in the loop */
    { AspTake k; if( asp_take_get( s, l.e[i].id, &k ) ) continue; }     /* a state with no take: one a dead run never got to store (see a5recover) */
    rc = asp_move_take( t, s, l.e[i].id, st->mono ? st->src : -1, st->gone, -1, ASP_PLACE_OFF, (AspPlace){ st->l, st->r, st->flip, st->gsh, st->pan, st->mixed } );
    }
  free( l.e );
  return rc;
  }
