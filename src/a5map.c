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
  if( *h == '-' ) return;
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
  return db < c->green_below_db ? C_LOW : db > c->red_above_db ? C_HIGH : C_MID;
  }

void map_draw( const Map *m, long top, int rows, int r0, int w, int T, long cur )
  {
  int tpc, cpc, cells, lw = 4, i;
  long hi = top + rows - 1;
  for( long v = hi; v >= 1000; v /= 10 ) lw++;
  cells = map_scale( T, w - lw - 1, &tpc, &cpc );
  if( cells < 1 || rows < 1 ) return;
  unsigned char *live = calloc( (size_t)rows * cells, 1 ), *mute = calloc( (size_t)rows * cells, 1 );
  if( ! live || ! mute ) { free( live ); free( mute ); return; }
  for( i = 0; i < m->n; i++ )
    {
    const MapTake *t = m->t + i; long start = t->off * 5;           /* 5 ticks a second */
    for( int k = 0; k < t->n; k++ )
      {
      unsigned char b = t->pk[k]; long a = start + k, row = t->iter + a / T - top;
      if( ! b ) continue;
      if( row < 0 ) continue;
      if( row >= rows ) break;                                       /* later ticks are later rows */
      unsigned char *p = ( t->st ? live : mute ) + row * cells + ( a % T ) / tpc;
      if( b > *p ) *p = b;
      }
    }
  for( int r = 0; r < rows; r++ )
    {
    long it = top + r; char line[64]; int cur_role = -1;
    printf( "\033[%d;1H", r0 + r );
    if( it >= 0 ) snprintf( line, sizeof line, "%*ld ", lw, it ); else snprintf( line, sizeof line, "%*s ", lw, "" );
    printf( "%s%s%s", term_sgr( C_TEXT ), it == cur ? "\033[1m" : "", line );
    for( int c = 0; c < cells; c++ )
      {
      unsigned char l = live[r * cells + c], mu = mute[r * cells + c];
      int role = l ? role_of( l ) : mu ? role_of( mu ) : C_TEXT; char ch = l ? '=' : mu ? '-' : ' ';
      if( role != cur_role ) { fputs( term_sgr( role ), stdout ); cur_role = role; }
      for( int k = 0; k < cpc; k++ ) putchar( ch );
      }
    printf( "%s\033[K", term_sgr( C_TEXT ) );
    }
  free( live ); free( mute );
  }
