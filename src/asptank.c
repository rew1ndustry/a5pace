#include <stdlib.h>
#include <string.h>
#include "asptank.h"
#include "plat.h"

#define HEAD 16
#define BLK  ASP_TICK

struct AspTank { FILE *f; int64_t frames; uint64_t iter, peak; };

static void say( char *err, int n, const char *m, const char *path )
  { if( err ) snprintf( err, n, "%s: %s", m, path ); }

AspTank *asp_tank_create( const char *path, int64_t frames, char *err, int errn )
  {
  FILE *f = fopen( path, "w+b" );
  if( ! f ) { say( err, errn, "cannot create", path ); return NULL; }
  unsigned char z[BLK * 16], h[HEAD];
  memset( z, 0, sizeof z ); memset( h, 0, sizeof h );
  fwrite( h, 1, HEAD, f );
  for( int64_t n = frames; n > 0; n -= BLK )
    if( fwrite( z, 16, n < BLK ? (size_t)n : BLK, f ) != ( n < BLK ? (size_t)n : BLK ) )
      { fclose( f ); say( err, errn, "write failed", path ); return NULL; }
  fflush( f );
  AspTank *t = calloc( 1, sizeof *t );
  t->f = f; t->frames = frames;
  return t;
  }

AspTank *asp_tank_open( const char *path, char *err, int errn )
  {
  FILE *f = fopen( path, "r+b" );
  unsigned char h[HEAD];
  if( ! f ) { say( err, errn, "cannot open", path ); return NULL; }
  if( fread( h, 1, HEAD, f ) != HEAD || ASP_FSEEK( f, 0, SEEK_END ) ) { fclose( f ); say( err, errn, "short file", path ); return NULL; }
  int64_t size = ASP_FTELL( f );
  if( size < HEAD || ( size - HEAD ) % 16 ) { fclose( f ); say( err, errn, "not a tank", path ); return NULL; }
  AspTank *t = calloc( 1, sizeof *t );
  t->f = f; t->frames = ( size - HEAD ) / 16;
  t->iter = asp_ld64( h ); t->peak = asp_ld64( h + 8 );
  return t;
  }

void asp_tank_close( AspTank *t ) { if( t ) { fclose( t->f ); free( t ); } }
int64_t  asp_tank_frames( AspTank *t ) { return t->frames; }
uint64_t asp_tank_iter( AspTank *t ) { return t->iter; }
uint64_t asp_tank_peak( AspTank *t ) { return t->peak; }

int asp_tank_set_head( AspTank *t, uint64_t iter, uint64_t peak )
  {
  unsigned char h[HEAD];
  asp_st64( h, iter ); asp_st64( h + 8, peak );
  t->iter = iter; t->peak = peak;
  if( ASP_FSEEK( t->f, 0, SEEK_SET ) || fwrite( h, 1, HEAD, t->f ) != HEAD ) return -1;
  return fflush( t->f ) ? -1 : 0;
  }

/* [pos, pos+n) split at the end of the loop */
static int span( AspTank *t, int64_t pos, int64_t n, int put, unsigned char *buf, int64_t *mix )
  {
  pos %= t->frames; if( pos < 0 ) pos += t->frames;
  while( n > 0 )
    {
    int64_t k = n < t->frames - pos ? n : t->frames - pos, i;
    if( k > BLK ) k = BLK;
    if( ASP_FSEEK( t->f, HEAD + pos * 16, SEEK_SET ) ) return -1;
    if( put )
      {
      for( i = 0; i < 2 * k; i++ ) asp_st64( buf + 8 * i, (uint64_t)mix[i] );
      if( fwrite( buf, 16, (size_t)k, t->f ) != (size_t)k ) return -1;
      }
    else
      {
      if( fread( buf, 16, (size_t)k, t->f ) != (size_t)k ) return -1;
      for( i = 0; i < 2 * k; i++ ) mix[i] = (int64_t)asp_ld64( buf + 8 * i );
      }
    mix += 2 * k; n -= k; pos += k; if( pos >= t->frames ) pos = 0;
    }
  return 0;
  }

int asp_tank_get( AspTank *t, int64_t pos, int64_t n, int64_t *mix )
  { unsigned char b[BLK * 16]; return span( t, pos, n, 0, b, mix ); }
int asp_tank_put( AspTank *t, int64_t pos, int64_t n, const int64_t *mix )
  { unsigned char b[BLK * 16]; int rc = span( t, pos, n, 1, b, (int64_t *)mix ); fflush( t->f ); return rc; }

int64_t asp_level( int64_t x, int lev )
  {
  if( lev == ASP_LEV_OFF ) return 0;
  if( lev >= 0 ) return x >> lev;                 /* arithmetic shift, as on every target we build for */
  return x * ( (int64_t)1 << -lev );
  }

void asp_mix_level( const int32_t *pcm, int64_t n, int64_t *mix, int ol, int orr, int nl, int nr )
  {
  for( int64_t i = 0; i < n; i++ )
    {
    *mix++ += asp_level( pcm[2*i],   nl ) - asp_level( pcm[2*i],   ol );
    *mix++ += asp_level( pcm[2*i+1], nr ) - asp_level( pcm[2*i+1], orr );
    }
  }

int asp_tank_level( AspTank *t, int64_t pos, const int32_t *pcm, int64_t n, int ol, int orr, int nl, int nr )
  {
  int64_t mix[BLK * 2];
  while( n > 0 )
    {
    int64_t k = n < BLK ? n : BLK;
    if( asp_tank_get( t, pos, k, mix ) ) return -1;
    asp_mix_level( pcm, k, mix, ol, orr, nl, nr );
    if( asp_tank_put( t, pos, k, mix ) ) return -1;
    pcm += 2 * k; pos += k; n -= k;
    }
  return 0;
  }

uint64_t asp_mix_out( uint64_t peak, int32_t *pcm, int64_t n, const int64_t *mix, int64_t gain )
  {
  int64_t min = mix[0], max = mix[0];
  int msb = 0;
  uint64_t p = peak | 1;
  while( p ) { msb++; p >>= 1; }                     /* 64 - clz( peak | 1 ) */
  for( int64_t i = 0; i < 2 * n; i++ )
    {
    int64_t sam = mix[i];
    if( gain ) sam += ( sam * gain ) >> 31;
    if( msb > 30 ) sam >>= ( msb - 30 );
    pcm[i] = (int32_t)sam;
    if( sam < min ) min = sam; else if( sam > max ) max = sam;
    }
  return (uint64_t)( max - min ) > peak ? (uint64_t)( max - min ) : peak;
  }
