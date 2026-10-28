#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
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

int asp_place_eq( AspPlace a, AspPlace b )
  {
  if( a.mixed != b.mixed ) return 0;
  if( a.mixed ) return a.gdb == b.gdb && a.pan == b.pan;
  return a.l == b.l && a.r == b.r && a.flip == b.flip;
  }

int64_t asp_gain( int64_t x, int64_t m ) { return ( x * m ) >> 24; }

static int64_t gain_q24( int gdb )
  {
  static int64_t tab[ASP_GDB_MAX - ASP_GDB_MIN + 1]; static int done;
  if( ! done ) { for( int i = 0; i < ASP_GDB_MAX - ASP_GDB_MIN + 1; i++ ) tab[i] = (int64_t)( pow( 10.0, ( i + ASP_GDB_MIN ) / 20.0 ) * 16777216.0 + 0.5 ); done = 1; }
  if( gdb < ASP_GDB_MIN ) gdb = ASP_GDB_MIN;
  if( gdb > ASP_GDB_MAX ) gdb = ASP_GDB_MAX;
  return tab[gdb - ASP_GDB_MIN];
  }

void asp_place_mult( AspPlace p, int mono, int64_t *ml, int64_t *mr )
  {
  int64_t g = gain_q24( p.gdb ); int pan = p.pan < -100 ? -100 : p.pan > 100 ? 100 : p.pan;
  if( mono ) { *ml = g * ( 100 - pan ) / 200; *mr = g * ( 100 + pan ) / 200; }
  else { *ml = g; *mr = g; }                           /* pan on a stereo take is kept but does nothing yet */
  }

void asp_place_to_mix( AspPlace p, int mono, int *gdb, int *pan )
  {
  if( p.mixed ) { *gdb = p.gdb; *pan = p.pan; return; }
  double l = p.l == ASP_LEV_OFF ? 0 : ldexp( 1.0, -p.l ), r = p.r == ASP_LEV_OFF ? 0 : ldexp( 1.0, -p.r ), g, pn;
  if( mono ) { g = l + r; pn = g > 0 ? 100.0 * ( r - l ) / g : 0; }
  else { g = l > r ? l : r; pn = g <= 0 ? 0 : r < l ? -100.0 * ( 1 - r / l ) : r > l ? 100.0 * ( 1 - l / r ) : 0; }
  *gdb = g > 0 ? (int)lround( 20.0 * log10( g ) ) : 0; *pan = (int)lround( pn );
  }

static inline void contrib( const int32_t *p, int ch, AspPlace pl, int sign, int64_t *m )
  {
  int64_t a = p[0], b = ch > 1 ? p[1] : p[0];
  if( pl.mixed )
    {
    int64_t ml, mr;
    asp_place_mult( pl, ch == 1, &ml, &mr );
    m[0] += sign * asp_gain( a, ml ); m[1] += sign * asp_gain( b, mr );
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
  return asp_place_eq( j->from, j->to ) && ! j->np ? 1 : 0;
  }

/* d runs along the loop from where the take starts: frame d, d+loop, d+2 loop ... all land on one position */
int asp_job_step_to( AspJob *j, AspTank *t, int64_t budget, int64_t limit )
  {
  const int32_t *np = j->np ? j->np : j->op;
  int64_t nn = j->np ? j->nn : j->on, nch = j->np ? j->nch : j->och, size = asp_tank_frames( t );
  int64_t end = j->hi - j->lo > size ? j->lo + size : j->hi;          /* positions to visit */
  if( limit >= 0 && j->lo + limit < end ) end = j->lo + limit;
  while( j->cursor < end && budget > 0 )
    {
    int64_t k = end - j->cursor, i, mix[2 * ASP_TICK];
    if( k > ASP_TICK ) k = ASP_TICK;
    if( asp_tank_get( t, j->pos0 + j->cursor, k, mix ) ) return -1;
    for( i = 0; i < k; i++ )
      for( int64_t f = j->cursor + i; f < j->hi; f += size )
        {
        if( f < j->on ) contrib( j->op + f * j->och, j->och, j->from, -1, mix + 2 * i );
        if( f < nn )    contrib( np + f * nch, (int)nch, j->to, 1, mix + 2 * i );
        }
    if( asp_tank_put( t, j->pos0 + j->cursor, k, mix ) ) return -1;
    j->cursor += k; budget -= k;
    }
  return j->cursor >= ( j->hi - j->lo > size ? j->lo + size : j->hi );
  }

int asp_job_step( AspJob *j, AspTank *t, int64_t budget ) { return asp_job_step_to( j, t, budget, -1 ); }

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
  strcpy( st->status, "legacy" ); st->l = st->r = 0; st->flip = st->mono = 0; st->gone = 0; st->peak = -1; st->len = -1; st->gdb = st->pan = st->mixed = 0;
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
  st->mixed = (int)num_after( j, "\"mixed\":", 0 ); st->gdb = (int)num_after( j, "\"gdb\":", 0 ); st->pan = (int)num_after( j, "\"pan\":", 0 );
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
    rc = asp_move_take( t, s, l.e[i].id, st->gone, -1, ASP_PLACE_OFF, (AspPlace){ st->l, st->r, st->flip, st->gdb, st->pan, st->mixed } );
    }
  free( l.e );
  return rc;
  }
