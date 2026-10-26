#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "aspengine.h"
#include "flacio.h"

typedef struct { AspTank *t; int64_t pos, at, lo, hi; int ol, orr, nl, nr; int bad; } Apply;

static int feed( const int32_t *pcm, unsigned frames, int channels, void *u )
  {
  Apply *a = u;
  if( channels != 2 ) return -1;
  int64_t from = a->at, to = a->at + frames;
  int64_t lo = from < a->lo ? a->lo : from;
  int64_t hi = a->hi >= 0 && a->hi < to ? a->hi : to;
  if( lo < hi && asp_tank_level( a->t, a->pos + lo, pcm + 2 * ( lo - from ), hi - lo, a->ol, a->orr, a->nl, a->nr ) )
    return -1;
  a->at = to;
  return 0;
  }

int asp_apply_take( AspTank *t, AspStore *s, int64_t take_id, int64_t lo, int64_t hi,
                    int ol, int orr, int nl, int nr )
  {
  AspTake k;
  char path[1200];
  if( asp_take_get( s, take_id, &k ) || asp_take_path( s, &k, path, sizeof path ) ) return -1;
  Apply a = { t, k.off * k.rate, 0, lo < 0 ? 0 : lo, hi, ol, orr, nl, nr, 0 };
  return asp_flac_decode_frames( path, feed, &a );
  }

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

void asp_parse_state( const char *j, AspTakeState *st )
  {
  const char *p;
  strcpy( st->status, "legacy" ); st->l = st->r = 0; st->gone = 0;
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
  if( ( p = strstr( j, "\"gone\":" ) ) && ( isdigit( (unsigned char)p[7] ) || p[7] == '"' ) )
    st->gone = atoll( p[7] == '"' ? p + 8 : p + 7 );
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
    if( ! strcmp( st->status, "off" ) ) continue;               /* not in the loop */
    rc = asp_apply_take( t, s, l.e[i].id, st->gone, -1, ASP_LEV_OFF, ASP_LEV_OFF, st->l, st->r );
    }
  free( l.e );
  return rc;
  }
