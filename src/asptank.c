#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "asptank.h"
#include "plat.h"

#include "aspchip.h"

#define HEAD   ASP_CHIP_HEAD
#define BLK    ASP_TICK
#define STRIDE ( HEAD + BLK * 16 )

/* The file is chips back to back (aspchip.h): each is a header then ASP_TICK frames, the last one fewer.
   Three handles on it, all unbuffered, so that none ever sees another's stale read-ahead and the stream never seeks:
   f for random access (get/put, restamp), rf the stream's reader, wf the stream's writer. The one seek in a lap is
   the wrap from the last chip back to the first. */
struct AspTank
  {
  FILE *f, *rf, *wf; char path[1024];
  int64_t frames; uint32_t chips; int rate;
  uint64_t iter, peak, seq; unsigned char session[32];
  int64_t rnext, wnext;                    /* the chip each stream handle is at, -1 if not known */
  unsigned char *cb;                       /* one chip, for the stream */
  unsigned char *stale;                    /* per chip: frames changed by put since the last stream write */
  };

static void say( char *err, int n, const char *m, const char *path )
  { if( err ) snprintf( err, n, "%s: %s", m, path ); }

static int hexval( int c ) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int64_t cframes( const AspTank *t, uint32_t k ) { return k + 1 == t->chips ? t->frames - (int64_t)k * BLK : BLK; }
static int64_t coff( uint32_t k ) { return (int64_t)k * STRIDE; }

static void head_fill( const AspTank *t, AspChipHead *h, uint32_t k )
  {
  h->f.index = k; h->f.chips = t->chips; h->f.frames = (uint32_t)cframes( t, k ); h->f.rate = (uint32_t)t->rate;
  memcpy( h->f.session, t->session, 32 );
  }

/* a chip's header out of the file by the random handle */
static int head_get( AspTank *t, uint32_t k, AspChipHead *h )
  {
  unsigned char b[HEAD];
  if( ASP_FSEEK( t->f, coff( k ), SEEK_SET ) || fread( b, 1, HEAD, t->f ) != HEAD ) return -1;
  return asp_chip_unpack( b, h, NULL ) ? -1 : 0;
  }
static int head_put( AspTank *t, uint32_t k, const AspChipHead *h )
  {
  unsigned char b[HEAD];
  asp_chip_pack( h, b );
  if( ASP_FSEEK( t->f, coff( k ), SEEK_SET ) || fwrite( b, 1, HEAD, t->f ) != HEAD ) return -1;
  return fflush( t->f ) ? -1 : 0;
  }

/* give every chip the tank's current iteration, peak, seq and session (their frames and crc stay) */
static int restamp( AspTank *t )
  {
  for( uint32_t k = 0; k < t->chips; k++ )
    {
    AspChipHead h;
    if( head_get( t, k, &h ) ) return -1;
    h.f.iter = t->iter; h.f.peak = t->peak; h.f.seq = t->seq;
    memcpy( h.f.session, t->session, 32 );
    if( head_put( t, k, &h ) ) return -1;
    }
  return 0;
  }

static int mark_stale( AspTank *t, uint32_t k )
  {
  AspChipHead h;
  if( t->stale[k] ) return 0;
  if( head_get( t, k, &h ) ) return -1;
  h.f.flags |= ASP_CHIP_FSTALE;
  if( head_put( t, k, &h ) ) return -1;
  t->stale[k] = 1;
  return 0;
  }

static AspTank *tank_new( const char *path, FILE *f )
  {
  AspTank *t = calloc( 1, sizeof *t );
  t->f = f; t->rnext = t->wnext = -1;
  snprintf( t->path, sizeof t->path, "%s", path );
  setvbuf( f, NULL, _IONBF, 0 );
  return t;
  }

static void tank_free( AspTank *t )
  {
  if( ! t ) return;
  if( t->f ) fclose( t->f );
  if( t->rf ) fclose( t->rf );
  if( t->wf ) fclose( t->wf );
  free( t->cb ); free( t->stale ); free( t );
  }

AspTank *asp_tank_create( const char *path, int64_t frames, int rate, char *err, int errn )
  {
  FILE *f = fopen( path, "w+b" );
  if( ! f ) { say( err, errn, "cannot create", path ); return NULL; }
  AspTank *t = tank_new( path, f );
  t->frames = frames; t->rate = rate; t->chips = (uint32_t)( ( frames + BLK - 1 ) / BLK );
  t->stale = calloc( t->chips, 1 );
  unsigned char *z = calloc( 1, STRIDE );
  uint32_t full = asp_crc32( 0, z, (size_t)BLK * 16 );
  for( uint32_t k = 0; k < t->chips; k++ )
    {
    AspChipHead h; size_t n = (size_t)cframes( t, k ) * 16;
    memset( &h, 0, sizeof h );
    head_fill( t, &h, k );
    h.f.dcrc = n == (size_t)BLK * 16 ? full : asp_crc32( 0, z, n );
    asp_chip_pack( &h, z );
    if( fwrite( z, 1, HEAD + n, f ) != HEAD + n ) { free( z ); tank_free( t ); say( err, errn, "write failed", path ); return NULL; }
    memset( z, 0, STRIDE );
    }
  free( z );
  fflush( f );
  return t;
  }

AspTank *asp_tank_open( const char *path, char *err, int errn )
  {
  FILE *f = fopen( path, "r+b" );
  unsigned char b[HEAD];
  AspChipHead h;
  if( ! f ) { say( err, errn, "cannot open", path ); return NULL; }
  if( fread( b, 1, HEAD, f ) != HEAD || asp_chip_unpack( b, &h, NULL ) || ASP_FSEEK( f, 0, SEEK_END ) )
    { fclose( f ); say( err, errn, "not a tank (no chip header)", path ); return NULL; }
  int64_t size = ASP_FTELL( f ), full = size / STRIDE, rem = size % STRIDE;
  AspTank *t = tank_new( path, f );
  if( rem && ( rem <= HEAD || ( rem - HEAD ) % 16 ) ) { tank_free( t ); say( err, errn, "size is not whole chips", path ); return NULL; }
  t->frames = full * BLK + ( rem ? ( rem - HEAD ) / 16 : 0 );
  t->chips = (uint32_t)( full + ( rem ? 1 : 0 ) );
  if( h.f.chips != t->chips || h.f.index != 0 ) { tank_free( t ); say( err, errn, "size does not match the chip headers", path ); return NULL; }
  t->rate = (int)h.f.rate; t->iter = h.f.iter; t->peak = h.f.peak; t->seq = h.f.seq;
  memcpy( t->session, h.f.session, 32 );
  t->stale = calloc( t->chips, 1 );
  for( uint32_t k = 0; k < t->chips; k++ )
    {
    if( head_get( t, k, &h ) || h.f.index != k || h.f.chips != t->chips ) { tank_free( t ); say( err, errn, "bad chip header", path ); return NULL; }
    t->stale[k] = ( h.f.flags & ASP_CHIP_FSTALE ) != 0;
    }
  return t;
  }

void asp_tank_close( AspTank *t ) { tank_free( t ); }
int64_t  asp_tank_frames( AspTank *t ) { return t->frames; }
uint64_t asp_tank_iter( AspTank *t ) { return t->iter; }
uint64_t asp_tank_peak( AspTank *t ) { return t->peak; }
int      asp_tank_rate( AspTank *t ) { return t->rate; }
uint64_t asp_tank_seq( AspTank *t ) { return t->seq; }
uint32_t asp_tank_chips( AspTank *t ) { return t->chips; }
int      asp_tank_chip_frames( AspTank *t, uint32_t k ) { return k < t->chips ? (int)cframes( t, k ) : 0; }

int asp_tank_set_head( AspTank *t, uint64_t iter, uint64_t peak )
  { t->iter = iter; t->peak = peak; return restamp( t ); }
int asp_tank_set_seq( AspTank *t, uint64_t seq )
  { t->seq = seq; return restamp( t ); }
int asp_tank_bind( AspTank *t, const char *hex )
  {
  if( strlen( hex ) != 64 ) return -1;
  for( int i = 0; i < 32; i++ )
    { int a = hexval( hex[2*i] ), b = hexval( hex[2*i+1] ); if( a < 0 || b < 0 ) return -1; t->session[i] = (unsigned char)( a << 4 | b ); }
  return restamp( t );
  }
int asp_tank_bound( AspTank *t, const char *hex )
  {
  unsigned char z[32] = { 0 }, want[32];
  if( ! memcmp( t->session, z, 32 ) ) return 1;
  if( strlen( hex ) != 64 ) return 0;
  for( int i = 0; i < 32; i++ ) want[i] = (unsigned char)( hexval( hex[2*i] ) << 4 | hexval( hex[2*i+1] ) );
  return ! memcmp( t->session, want, 32 );
  }

/* the stream: chip k out of the file, header checked, and the frames as mix (2 * frames int64).
   0 good; -1 io or k out of range; -2 bad header; -3 frames do not match their crc */
int asp_tank_chip_read( AspTank *t, uint32_t k, AspChipHead *h, int64_t *mix )
  {
  if( k >= t->chips ) return -1;
  if( ! t->rf )
    {
    t->rf = fopen( t->path, "rb" );
    if( ! t->rf ) return -1;
    setvbuf( t->rf, NULL, _IONBF, 0 );
    }
  if( ! t->cb ) t->cb = malloc( STRIDE );
  size_t n = (size_t)cframes( t, k ) * 16;
  if( t->rnext != (int64_t)k && ASP_FSEEK( t->rf, coff( k ), SEEK_SET ) ) { t->rnext = -1; return -1; }
  if( fread( t->cb, 1, HEAD + n, t->rf ) != HEAD + n ) { t->rnext = -1; return -1; }
  t->rnext = k + 1 < t->chips ? (int64_t)k + 1 : -1;      /* after the last chip the next read is the wrap seek */
  if( asp_chip_unpack( t->cb, h, NULL ) || h->f.index != k || h->f.chips != t->chips ) return -2;
  if( ! ( h->f.flags & ASP_CHIP_FSTALE ) && asp_crc32( 0, t->cb + HEAD, n ) != h->f.dcrc ) return -3;
  for( size_t i = 0; i < n / 8; i++ ) mix[i] = (int64_t)asp_ld64( t->cb + HEAD + 8 * i );
  return 0;
  }

/* chip k into the file in one write. the caller gives iter, pass, seq, peak, out_shift and flags in h;
   index, chips, frames, rate, session and the crcs are set here, and the stale mark is cleared */
int asp_tank_chip_write( AspTank *t, uint32_t k, const AspChipHead *in, const int64_t *mix )
  {
  AspChipHead h = *in;
  if( k >= t->chips ) return -1;
  if( ! t->wf )
    {
    t->wf = fopen( t->path, "r+b" );
    if( ! t->wf ) return -1;
    setvbuf( t->wf, NULL, _IONBF, 0 );
    }
  if( ! t->cb ) t->cb = malloc( STRIDE );
  size_t n = (size_t)cframes( t, k ) * 16;
  for( size_t i = 0; i < n / 8; i++ ) asp_st64( t->cb + HEAD + 8 * i, (uint64_t)mix[i] );
  head_fill( t, &h, k );
  h.f.flags &= ~ASP_CHIP_FSTALE;
  h.f.dcrc = asp_crc32( 0, t->cb + HEAD, n );
  asp_chip_pack( &h, t->cb );
  if( t->wnext != (int64_t)k && ASP_FSEEK( t->wf, coff( k ), SEEK_SET ) ) { t->wnext = -1; return -1; }
  if( fwrite( t->cb, 1, HEAD + n, t->wf ) != HEAD + n || fflush( t->wf ) ) { t->wnext = -1; return -1; }
  t->wnext = k + 1 < t->chips ? (int64_t)k + 1 : -1;
  t->stale[k] = 0;
  return 0;
  }

/* [pos, pos+n) split at the end of the loop and at every chip */
static int span( AspTank *t, int64_t pos, int64_t n, int put, unsigned char *buf, int64_t *mix )
  {
  pos %= t->frames; if( pos < 0 ) pos += t->frames;
  while( n > 0 )
    {
    int64_t k = n, i, inchip = BLK - pos % BLK;
    uint32_t ck = (uint32_t)( pos / BLK );
    if( k > inchip ) k = inchip;
    if( k > t->frames - pos ) k = t->frames - pos;
    if( put && mark_stale( t, ck ) ) return -1;
    if( ASP_FSEEK( t->f, coff( ck ) + HEAD + ( pos % BLK ) * 16, SEEK_SET ) ) return -1;
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

uint64_t asp_mix_out( uint64_t peak, int32_t *pcm, int64_t n, const int64_t *mix, int *shift )
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
