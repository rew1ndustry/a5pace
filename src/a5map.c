#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a5map.h"
#include "a5term.h"

void map_clear( Map *m ) { for( int i = 0; i < m->n; i++ ) free( m->t[i].pk ); m->n = 0; }

static int hexv( int c ) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

void map_line( const char *line, void *u )
  {
  Map *m = u; long iter, off, frames; char st[16]; int tick0, used = 0;
  if( sscanf( line, "%ld %ld %ld %15s %d %n", &iter, &off, &frames, st, &tick0, &used ) < 5 || ! used || tick0 < 0 || tick0 > 100000 ) return;
  MapTake *t = m->n ? m->t + m->n - 1 : NULL;
  if( ! t || t->iter != iter || t->off != off )                     /* a new take; the chunks of one come together */
    {
    if( m->n == m->cap ) { int c = m->cap ? m->cap * 2 : 256; MapTake *p = realloc( m->t, (size_t)c * sizeof *p ); if( ! p ) return; m->t = p; m->cap = c; }
    t = m->t + m->n++; memset( t, 0, sizeof *t );
    t->iter = iter; t->off = off; t->frames = frames; t->st = ! strcmp( st, "open" ) ? 2 : ! strcmp( st, "in" );
    }
  const char *h = line + used; int k = 0;
  if( *h == '-' )                                                   /* no levels kept for this take: it is where it is, at an unknown level */
    {
    int n = (int)( ( frames + 9599 ) / 9600 ); if( n < 1 || n > 100000 ) return;
    unsigned char *p = realloc( t->pk, (size_t)n ); if( ! p ) return;
    memset( p, 1, (size_t)n ); t->pk = p; t->cap = t->n = n;
    return;
    }
  while( hexv( h[2*k] ) >= 0 && hexv( h[2*k+1] ) >= 0 ) k++;
  if( tick0 + k > t->cap )
    { int c = tick0 + k + 64; unsigned char *p = realloc( t->pk, (size_t)c ); if( ! p ) return; memset( p + t->cap, 0, (size_t)( c - t->cap ) ); t->pk = p; t->cap = c; }
  for( int i = 0; i < k; i++ ) t->pk[tick0 + i] = (unsigned char)( hexv( h[2*i] ) * 16 + hexv( h[2*i+1] ) );
  if( tick0 + k > t->n ) t->n = tick0 + k;
  }

int map_scale( int T, int w, int *tpc, int *cpc )
  {
  if( T < 1 ) T = 1;
  if( w < 1 ) w = 1;
  if( T <= w ) { *tpc = 1; *cpc = w / T; }
  else { *tpc = ( T + w - 1 ) / w; *cpc = 1; }
  return ( T + *tpc - 1 ) / *tpc;
  }

static int role_of( int b )                                        /* the colour of a level byte */
  {
  const TermCfg *c = term_cfg(); double db = ( b - 1 ) / 2.0 - 120;
  if( b <= 1 ) return C_TEXT;                                       /* unknown or silent: grey */
  return db < c->green_below_db ? C_LOW : db > c->red_above_db ? C_HIGH : C_MID;
  }

int map_geom( int T, int w, long hi, int *lw, int *tpc, int *cpc )
  {
  int l = 4;
  for( long v = hi; v >= 1000; v /= 10 ) l++;
  *lw = l;
  return map_scale( T, w - l - 1, tpc, cpc );
  }

/* rows: nmain iterations from top, then (if there is a pinned set) a blank row and the pinned iterations */
int map_view_rows( const MapView *v ) { return v->nmain + ( v->npin ? 1 + v->npin : 0 ); }
static long view_it( const MapView *v, int r )          /* the iteration shown on row r; -1 for none */
  {
  if( r < v->nmain ) return v->top + r < 0 ? -1 : v->top + r;
  if( r == v->nmain ) return -1;
  return v->pin[r - v->nmain - 1];
  }
static int view_row( const MapView *v, long it )
  {
  if( it >= v->top && it < v->top + v->nmain ) return (int)( it - v->top );
  for( int i = 0; i < v->npin; i++ ) if( v->pin[i] == it ) return v->nmain + 1 + i;
  return -1;
  }
int map_iter_ok( const MapView *v, long it )            /* may the cursor be there: the main iterations (also above the top) and the pinned ones */
  {
  if( it < 0 ) return 0;
  if( it < v->top + v->nmain ) return 1;
  for( int i = 0; i < v->npin; i++ ) if( v->pin[i] == it ) return 1;
  return 0;
  }

/* does take t touch the selection? c1 < 0: the whole iterations r1..r2, i.e. every take that starts there. with mark, mark all its cells */
static int covers( const MapTake *t, const MapSel *s, const MapView *v, int T, int tpc, int cells, unsigned char *mark )
  {
  int hit = 0; long start = t->off * 5;
  if( s->c1 < 0 ) hit = t->iter >= s->r1 && t->iter <= s->r2 && map_iter_ok( v, t->iter );
  else
    for( int k = 0; k < t->n && ! hit; k++ )
      {
      long a = start + k, it = t->iter + a / T; int c = (int)( ( a % T ) / tpc );
      if( t->pk[k] && it >= s->r1 && it <= s->r2 && c >= s->c1 && c <= s->c2 && map_iter_ok( v, it ) ) hit = 1;
      }
  if( hit && mark )
    for( int k = 0; k < t->n; k++ )
      {
      long a = start + k; int row = view_row( v, t->iter + a / T );
      if( t->pk[k] && row >= 0 ) mark[row * cells + ( a % T ) / tpc] = 1;
      }
  return hit;
  }

int map_affected( const Map *m, const MapSel *sel, const MapView *v, int T, int w, long wide, int *idx, int max )
  {
  int lw, tpc, cpc, n = 0, cells = map_geom( T, w, wide, &lw, &tpc, &cpc );
  for( int i = 0; i < m->n && n < max; i++ )
    if( m->t[i].st != 2 && covers( m->t + i, sel, v, T, tpc, cells, NULL ) ) idx[n++] = i;
  return n;
  }

/* the cells where takes begin in iteration it (the first lit cell of each take in that row), sorted, no repeats */
int map_starts( const Map *m, const MapView *v, int T, int w, long wide, long it, int *out, int max )
  {
  int lw, tpc, cpc, n = 0; (void)v; map_geom( T, w, wide, &lw, &tpc, &cpc );
  for( int i = 0; i < m->n; i++ )
    {
    const MapTake *t = m->t + i; long start = t->off * 5;
    if( t->st == 2 ) continue;
    for( int k = 0; k < t->n; k++ )
      {
      long a = start + k;
      if( ! t->pk[k] || t->iter + a / T != it ) continue;
      int c = (int)( ( a % T ) / tpc ), j = 0;
      while( j < n && out[j] < c ) j++;
      if( j < n && out[j] == c ) break;
      if( n < max ) { memmove( out + j + 1, out + j, (size_t)( n - j ) * sizeof *out ); out[j] = c; n++; }
      break;
      }
    }
  return n;
  }

/* the nearest iteration above (dir > 0) or below that has a take lit in it and may be visited; it itself if none */
long map_step( const Map *m, const MapView *v, int T, long it, int dir )
  {
  long best = it;
  for( int i = 0; i < m->n; i++ )
    {
    const MapTake *t = m->t + i; long start = t->off * 5;
    if( t->st == 2 ) continue;
    for( int k = 0; k < t->n; k++ )
      {
      long x = t->iter + ( start + k ) / T;
      if( ! t->pk[k] || ! map_iter_ok( v, x ) ) continue;
      if( dir > 0 ? ( x > it && ( best == it || x < best ) ) : ( x < it && ( best == it || x > best ) ) ) best = x;
      }
    }
  return best;
  }

void map_draw( const Map *m, const MapView *v, int r0, int w, int T, long cur, const MapSel *sel, long wide )
  {
  int tpc, cpc, cells, lw, i, rows = map_view_rows( v );
  cells = map_geom( T, w, wide, &lw, &tpc, &cpc );
  if( cells < 1 || rows < 1 ) return;
  unsigned char *live = calloc( (size_t)rows * cells, 1 ), *mute = calloc( (size_t)rows * cells, 1 ), *mark = calloc( (size_t)rows * cells, 1 );
  if( ! live || ! mute || ! mark ) { free( live ); free( mute ); free( mark ); return; }
  for( i = 0; i < m->n; i++ )
    {
    const MapTake *t = m->t + i; long start = t->off * 5;           /* 5 ticks a second */
    for( int k = 0; k < t->n; k++ )
      {
      unsigned char b = t->pk[k]; long a = start + k; int row = view_row( v, t->iter + a / T );
      if( ! b || row < 0 ) continue;
      unsigned char *p = ( t->st ? live : mute ) + row * cells + ( a % T ) / tpc;
      if( b > *p ) *p = b;
      }
    if( sel && t->st != 2 ) covers( t, sel, v, T, tpc, cells, mark );
    }
  for( int r = 0; r < rows; r++ )
    {
    long it = view_it( v, r ); char line[64]; int cur_role = -1, cur_fx = -1;
    int whole = sel && it >= 0 && it >= sel->r1 && it <= sel->r2 && sel->c1 < 0;
    printf( "\033[%d;1H", r0 + r );
    if( it >= 0 ) snprintf( line, sizeof line, "%*ld", lw, it ); else snprintf( line, sizeof line, "%*s", lw, "" );
    printf( "%s%s%s%s%s ", term_sgr( C_TEXT ), it == cur ? "\033[1m" : "", whole ? ( sel->cur_cell < 0 && it == sel->cur_it ? "\033[1;7m" : "\033[7m" ) : "", line, term_sgr( C_TEXT ) );
    for( int c = 0; c < cells && it >= 0; c++ )
      {
      unsigned char l = live[r * cells + c], mu = mute[r * cells + c];
      int role = l ? role_of( l ) : mu ? role_of( mu ) : C_TEXT; char ch = l ? '=' : mu ? '-' : ' ';
      int fx = 0;                                                    /* 1 underlined, 2 selected, 3 the cursor */
      if( sel )
        {
        if( mark[r * cells + c] ) fx = 1;
        if( it >= sel->r1 && it <= sel->r2 && c >= sel->c1 && c <= sel->c2 ) fx = 2;
        if( it == sel->cur_it && c == sel->cur_cell ) fx = 3;
        }
      if( role != cur_role || fx != cur_fx )
        {
        fputs( term_sgr( role ), stdout ); cur_role = role; cur_fx = fx;
        if( fx == 1 ) fputs( "\033[4m", stdout ); else if( fx == 2 ) fputs( "\033[7m", stdout ); else if( fx == 3 ) fputs( "\033[1;7m", stdout );
        }
      for( int k = 0; k < cpc; k++ ) putchar( ch );
      }
    printf( "%s\033[K", term_sgr( C_TEXT ) );
    }
  free( live ); free( mute ); free( mark );
  }
