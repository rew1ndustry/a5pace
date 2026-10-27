#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "asptank.h"
#include "plat.h"

#define HEAD 128
#define OLDHEAD 16
#define BLK  ASP_TICK

struct AspTank
  {
  FILE *f; int64_t frames; int hdr; int rate;
  uint64_t iter, peak, seq; uint32_t flags; unsigned char session[32];
  };

static void say( char *err, int n, const char *m, const char *path )
  { if( err ) snprintf( err, n, "%s: %s", m, path ); }

static int hexval( int c ) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int head_write( AspTank *t )
  {
  unsigned char h[HEAD];
  memset( h, 0, sizeof h );
  if( t->hdr == OLDHEAD ) { asp_st64( h, t->iter ); asp_st64( h + 8, t->peak ); }
  else
    {
    memcpy( h, "ASP5TANK", 8 );
    asp_st32( h + 8, 1 ); asp_st32( h + 12, HEAD );
    asp_st64( h + 16, (uint64_t)t->frames ); asp_st32( h + 24, (uint32_t)t->rate ); asp_st32( h + 28, 2 );
    asp_st64( h + 32, t->iter ); asp_st64( h + 40, t->peak ); asp_st64( h + 48, t->seq );
    asp_st32( h + 56, t->flags );
    memcpy( h + 64, t->session, 32 );
    }
  if( ASP_FSEEK( t->f, 0, SEEK_SET ) || fwrite( h, 1, (size_t)t->hdr, t->f ) != (size_t)t->hdr ) return -1;
  return fflush( t->f ) ? -1 : 0;
  }

AspTank *asp_tank_create( const char *path, int64_t frames, int rate, char *err, int errn )
  {
  FILE *f = fopen( path, "w+b" );
  if( ! f ) { say( err, errn, "cannot create", path ); return NULL; }
  AspTank *t = calloc( 1, sizeof *t );
  t->f = f; t->frames = frames; t->hdr = HEAD; t->rate = rate;
  unsigned char z[BLK * 16];
  memset( z, 0, sizeof z );
  if( head_write( t ) ) { fclose( f ); free( t ); say( err, errn, "write failed", path ); return NULL; }
  for( int64_t n = frames; n > 0; n -= BLK )
    if( fwrite( z, 16, n < BLK ? (size_t)n : BLK, f ) != ( n < BLK ? (size_t)n : BLK ) )
      { fclose( f ); free( t ); say( err, errn, "write failed", path ); return NULL; }
  fflush( f );
  return t;
  }

AspTank *asp_tank_open( const char *path, char *err, int errn )
  {
  FILE *f = fopen( path, "r+b" );
  unsigned char h[HEAD];
  if( ! f ) { say( err, errn, "cannot open", path ); return NULL; }
  size_t got = fread( h, 1, HEAD, f );
  if( got < OLDHEAD || ASP_FSEEK( f, 0, SEEK_END ) ) { fclose( f ); say( err, errn, "short file", path ); return NULL; }
  int64_t size = ASP_FTELL( f );
  AspTank *t = calloc( 1, sizeof *t );
  t->f = f;
  if( got == HEAD && ! memcmp( h, "ASP5TANK", 8 ) )
    {
    if( asp_ld32( h + 8 ) != 1 || asp_ld32( h + 12 ) != HEAD ) { fclose( f ); free( t ); say( err, errn, "unknown tank version", path ); return NULL; }
    t->hdr = HEAD; t->frames = (int64_t)asp_ld64( h + 16 ); t->rate = (int)asp_ld32( h + 24 );
    t->iter = asp_ld64( h + 32 ); t->peak = asp_ld64( h + 40 ); t->seq = asp_ld64( h + 48 );
    t->flags = asp_ld32( h + 56 ); memcpy( t->session, h + 64, 32 );
    if( size != HEAD + t->frames * 16 ) { fclose( f ); free( t ); say( err, errn, "size does not match header", path ); return NULL; }
    }
  else
    {
    if( ( size - OLDHEAD ) % 16 ) { fclose( f ); free( t ); say( err, errn, "not a tank", path ); return NULL; }
    t->hdr = OLDHEAD; t->frames = ( size - OLDHEAD ) / 16; t->rate = 48000;
    t->iter = asp_ld64( h ); t->peak = asp_ld64( h + 8 );
    }
  return t;
  }

void asp_tank_close( AspTank *t )
  { if( t ) { fclose( t->f ); free( t ); } }
int64_t  asp_tank_frames( AspTank *t ) { return t->frames; }
uint64_t asp_tank_iter( AspTank *t ) { return t->iter; }
uint64_t asp_tank_peak( AspTank *t ) { return t->peak; }
int      asp_tank_legacy( AspTank *t ) { return t->hdr == OLDHEAD; }
int      asp_tank_rate( AspTank *t ) { return t->rate; }
uint64_t asp_tank_seq( AspTank *t ) { return t->seq; }
int      asp_tank_dirty( AspTank *t ) { return ( t->flags & 1 ) != 0; }

int asp_tank_set_head( AspTank *t, uint64_t iter, uint64_t peak )
  { t->iter = iter; t->peak = peak; return head_write( t ); }
int asp_tank_set_seq( AspTank *t, uint64_t seq )
  { if( t->hdr == OLDHEAD ) return -1; t->seq = seq; return head_write( t ); }
int asp_tank_set_dirty( AspTank *t, int dirty )
  { if( t->hdr == OLDHEAD ) return -1; t->flags = dirty ? t->flags | 1 : t->flags & ~1u; return head_write( t ); }
int asp_tank_bind( AspTank *t, const char *hex )
  {
  if( t->hdr == OLDHEAD || strlen( hex ) != 64 ) return -1;
  for( int i = 0; i < 32; i++ )
    { int a = hexval( hex[2*i] ), b = hexval( hex[2*i+1] ); if( a < 0 || b < 0 ) return -1; t->session[i] = (unsigned char)( a << 4 | b ); }
  return head_write( t );
  }
int asp_tank_bound( AspTank *t, const char *hex )
  {
  unsigned char z[32] = { 0 }, want[32];
  if( t->hdr == OLDHEAD || ! memcmp( t->session, z, 32 ) ) return 1;
  if( strlen( hex ) != 64 ) return 0;
  for( int i = 0; i < 32; i++ ) want[i] = (unsigned char)( hexval( hex[2*i] ) << 4 | hexval( hex[2*i+1] ) );
  return ! memcmp( t->session, want, 32 );
  }

/* [pos, pos+n) split at the end of the loop */
static int span( AspTank *t, int64_t pos, int64_t n, int put, unsigned char *buf, int64_t *mix )
  {
  pos %= t->frames; if( pos < 0 ) pos += t->frames;
  while( n > 0 )
    {
    int64_t k = n < t->frames - pos ? n : t->frames - pos, i;
    if( k > BLK ) k = BLK;
    if( ASP_FSEEK( t->f, t->hdr + pos * 16, SEEK_SET ) ) return -1;
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

uint64_t asp_mix_out( uint64_t peak, int32_t *pcm, int64_t n, const int64_t *mix, int64_t gain, int *shift )
  {
  int msb = 0, sh, old;
  uint64_t p = peak | 1;
  while( p ) { msb++; p >>= 1; }                     /* 64 - clz( peak | 1 ) */
  sh = msb > 30 ? msb - 30 : 0;                      /* how far the loop must come down to fit */
  old = shift ? *shift : sh;
  double s0 = ldexp( 1.0, -old ), s1 = ldexp( 1.0, -sh );
  int64_t min = 0, max = 0;
  for( int64_t i = 0; i < 2 * n; i++ )
    {
    int64_t sam = mix[i], v;
    if( gain ) sam += ( sam * gain ) >> 31;
    if( old == sh ) v = sam >> sh;                   /* steady: exact, as asp4 */
    else
      {
      /* the shift has changed, a step of 6 dB or more: fade from the old scale to the new across this tick */
      double sc = s0 + ( s1 - s0 ) * ( (double)( i / 2 + 1 ) / (double)n ), d = (double)sam * sc;
      v = (int64_t)( d < 0 ? d - 0.5 : d + 0.5 );
      }
    if( v > 2147483647 ) v = 2147483647; else if( v < -2147483647 - 1 ) v = -2147483647 - 1;
    pcm[i] = (int32_t)v;
    if( i == 0 ) min = max = v;
    if( v < min ) min = v; else if( v > max ) max = v;
    }
  if( shift ) *shift = sh;
  return (uint64_t)( max - min ) > peak ? (uint64_t)( max - min ) : peak;
  }
