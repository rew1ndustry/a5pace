#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "aspengine.h"
#include "flacio.h"

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

int asp_take_peak( AspStore *s, int64_t id, int64_t *peak )
  {
  AspTake k; char path[1200]; int32_t *p; int64_t n, pk = 0; int ch;
  if( asp_take_get( s, id, &k ) || asp_take_path( s, &k, path, sizeof path ) || asp_pcm_load( path, &p, &n, &ch ) ) return -1;
  for( int64_t i = 0; i < n * ch; i++ ) { int64_t v = p[i] < 0 ? -(int64_t)p[i] : p[i]; if( v > pk ) pk = v; }
  free( p ); *peak = pk;
  return 0;
  }

/* --- jobs --- */

struct AspJob
  {
  int64_t pos0, lo, hi, cursor;
  int32_t *op, *np;
  int64_t on, nn;
  int och, nch;
  AspPlace from, to;
  };

AspJob *asp_job_new( int64_t pos0, int64_t lo, int64_t hi,
                     int32_t *oldpcm, int64_t oldframes, int oldch, AspPlace from,
                     int32_t *newpcm, int64_t newframes, int newch, AspPlace to )
  {
  AspJob *j = calloc( 1, sizeof *j );
  j->pos0 = pos0; j->lo = lo < 0 ? 0 : lo;
  j->op = oldpcm; j->on = oldframes; j->och = oldch; j->from = from;
  j->np = newpcm; j->nn = newframes; j->nch = newch; j->to = to;
  int64_t top = oldframes > newframes ? oldframes : newframes;
  j->hi = hi < 0 || hi > top ? top : hi;
  j->cursor = j->lo;
  return j;
  }

AspJob *asp_job_take( AspStore *s, int64_t id, int64_t lo, int64_t hi, AspPlace from, AspPlace to )
  {
  AspTake k; char path[1200]; int32_t *p; int64_t n; int ch;
  if( asp_take_get( s, id, &k ) || asp_take_path( s, &k, path, sizeof path ) || asp_pcm_load( path, &p, &n, &ch ) ) return NULL;
  /* the same buffer is read for both sides */
  AspJob *j = asp_job_new( k.off * k.rate, lo, hi, p, n, ch, from, p, n, ch, to );
  j->np = NULL;                                           /* shared: np == NULL means use op */
  return j;
  }

void asp_job_free( AspJob *j ) { if( j ) { free( j->op ); if( j->np ) free( j->np ); free( j ); } }
int64_t asp_job_span( const AspJob *j ) { return j->hi - j->lo; }
int64_t asp_job_pos0( const AspJob *j ) { return j->pos0; }

static inline void contrib( const int32_t *p, int ch, AspPlace pl, int sign, int64_t *m )
  {
  int64_t a = p[0], b = ch > 1 ? p[1] : p[0];
  int64_t s0 = pl.flip ? b : a, s1 = pl.flip ? a : b;
  m[0] += sign * asp_level( s0, pl.l );
  m[1] += sign * asp_level( s1, pl.r );
  }

int asp_job_step( AspJob *j, AspTank *t, int64_t budget )
  {
  const int32_t *np = j->np ? j->np : j->op;
  int64_t nn = j->np ? j->nn : j->on, nch = j->np ? j->nch : j->och;
  while( j->cursor < j->hi && budget > 0 )
    {
    int64_t k = j->hi - j->cursor, i, mix[2 * ASP_TICK];
    if( k > ASP_TICK ) k = ASP_TICK;
    if( asp_tank_get( t, j->pos0 + j->cursor, k, mix ) ) return -1;
    for( i = 0; i < k; i++ )
      {
      int64_t f = j->cursor + i;
      if( f < j->on ) contrib( j->op + f * j->och, j->och, j->from, -1, mix + 2 * i );
      if( f < nn )    contrib( np + f * nch, (int)nch, j->to, 1, mix + 2 * i );
      }
    if( asp_tank_put( t, j->pos0 + j->cursor, k, mix ) ) return -1;
    j->cursor += k; budget -= k;
    }
  return j->cursor >= j->hi;
  }

int asp_move_take( AspTank *t, AspStore *s, int64_t id, int64_t lo, int64_t hi, AspPlace from, AspPlace to )
  {
  AspJob *j = asp_job_take( s, id, lo, hi, from, to );
  if( ! j ) return -1;
  int rc = asp_job_step( j, t, INT64_MAX );
  asp_job_free( j );
  return rc < 0 ? -1 : 0;
  }

int asp_apply_take( AspTank *t, AspStore *s, int64_t id, int64_t lo, int64_t hi, int ol, int orr, int nl, int nr )
  { return asp_move_take( t, s, id, lo, hi, (AspPlace){ ol, orr, 0 }, (AspPlace){ nl, nr, 0 } ); }

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
  strcpy( st->status, "legacy" ); st->l = st->r = 0; st->flip = st->mono = 0; st->gone = 0; st->peak = -1; st->len = -1;
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
  st->mono = (int)num_after( j, "\"mono\":", 0 );
  st->peak = num_after( j, "\"peak\":", -1 );
  st->len = num_after( j, "\"len\":", -1 );
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
    rc = asp_move_take( t, s, l.e[i].id, st->gone, -1, ASP_PLACE_OFF, (AspPlace){ st->l, st->r, st->flip } );
    }
  free( l.e );
  return rc;
  }
