#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "asppeaks.h"
#include "asptank.h"

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
