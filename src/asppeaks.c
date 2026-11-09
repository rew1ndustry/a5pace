#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "asppeaks.h"
#include "asptank.h"
#include "aspstore.h"
#include "takefile.h"
#include <stdio.h>

void asp_pk_reset( AspPk *p ) { p->n = 0; p->s[0] = p->s[1] = 0; p->cnt = 0; p->cur = -1; }
void asp_pk_free( AspPk *p ) { free( p->v ); memset( p, 0, sizeof *p ); }
double asp_pk_db( uint8_t b ) { return b ? ( b - 1 ) / 2.0 - 120 : -200; }

static uint8_t pk_byte( const double *s, int64_t cnt, int ch )
  {
  double m = ch >= 0 ? s[ch] : ( s[0] > s[1] ? s[0] : s[1] );
  if( m <= 0 || cnt <= 0 ) return 0;
  double db = 10 * log10( m / cnt / 4.611686018427388e18 );          /* 2^62: full scale squared */
  long b = lround( ( db + 120 ) * 2 ) + 1;
  return (uint8_t)( b < 1 ? 1 : b > 241 ? 241 : b );
  }

void asp_pk_add( AspPk *p, const int32_t *pcm, int64_t n, int64_t pos, int ch )
  {
  while( n > 0 )
    {
    int64_t k = pos / ASP_TICK, room = ( k + 1 ) * ASP_TICK - pos, m = n < room ? n : room;
    if( k > 100000 ) return;                                          /* 5.5 hours: not a take */
    if( k != p->cur ) { p->cur = k; p->s[0] = p->s[1] = 0; p->cnt = 0; }
    if( k >= p->cap )
      {
      int cap = p->cap ? p->cap : 1024; while( cap <= k ) cap *= 2;
      uint8_t *v = realloc( p->v, (size_t)cap );
      if( ! v ) return;
      p->v = v; p->cap = cap;
      }
    if( k >= p->n ) { memset( p->v + p->n, 0, (size_t)( k + 1 - p->n ) ); p->n = (int)k + 1; }
    for( int64_t i = 0; i < m; i++ )
      { double l = pcm[2*i], r = pcm[2*i+1]; p->s[0] += l * l; p->s[1] += r * r; }
    p->cnt += m;
    p->v[k] = pk_byte( p->s, p->cnt, ch );
    pcm += 2 * m; pos += m; n -= m;
    }
  }

int asp_peaks_backfill( AspStore *s, int *made, int *bad )
  {
  AspPk pk = { 0 }; int64_t ids[64], after = 0; int n, done = 0, nb = 0;
  static int32_t buf[2 * 4096], st[2 * 4096];
  while( ( n = asp_peaks_need( s, after, ids, 64 ) ) > 0 )
    for( int i = 0; i < n; i++ )
      {
      AspTake t; char path[1200]; AspTakeR *r; int64_t pos = 0; int got; after = ids[i];
      if( asp_take_get( s, ids[i], &t ) || asp_take_path( s, &t, path, sizeof path ) || ! ( r = asp_tr_open( path ) ) )
        { fprintf( stderr, "take %lld cannot be read\n", (long long)ids[i] ); nb++; continue; }
      int chans = asp_tr_hdr( r )->channels;
      asp_pk_reset( &pk );
      while( ( got = asp_tr_get( r, buf, 4096 ) ) > 0 )
        {
        const int32_t *p = buf;
        if( chans == 1 ) { for( int k = 0; k < got; k++ ) st[2*k] = st[2*k+1] = buf[k]; p = st; }
        asp_pk_add( &pk, p, got, pos, -1 ); pos += got;
        }
      asp_tr_close( r );
      if( got < 0 || ! pk.n || asp_peaks_set( s, ids[i], pk.v, pk.n ) ) { fprintf( stderr, "take %lld: no levels\n", (long long)ids[i] ); nb++; }
      else done++;
      }
  asp_pk_free( &pk );
  if( made ) *made = done;
  if( bad ) *bad = nb;
  return nb != 0;
  }
