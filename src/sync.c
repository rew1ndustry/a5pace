/* sync: the FSK round-trip hunt, ported from asp4 (CPV/MSK, 0 = two cycles of 1000 Hz, 1 = three of
   1500 Hz, signal "001sss0pp": a header, the second modulo 8, and the mark's position). the
   decoding is asp4's, byte for byte (tests/sync_diff.sh holds it to that); on top of it AspSync
   adds what asp4 lacked:
     - starts quiet and rises until the return reads cleanly, then trims to a target input level
     - tries both polarities every second (asp4 alternated one per second)
     - takes the median of several readings and refuses an unsteady one
   the loop is offset by the shell/loop code, not here. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sync.h"

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

static int32_t _dith( double d )
  {
  double i, f = fabs( modf( d, &i ) );
  return( ( f < 0.5 ) ? i : ( i > 0.0 ? ( i + 1.0 ) : ( i - 1.0 ) ) );
  }

#define sineSR		48000.0
#define sinePI		3.14159265359
#define sineF(H)	( ( 2.0 * sinePI * (H) ) / sineSR )
#define SINE(X,F,A)	( (A) * ( sin( (F) * (X) ) ) )

// fix & 1 prefixes half amplitude half wave
// fix & 2 postfixes same

static int32_t *audSine( int32_t *pad, int32_t *end, double hz, double am, int many, int fix )
  {
  if( many > 0 )
    {
    double fp = sineF( hz );
    double li = 0.0, lf = fabs( modf(sineSR/hz,&li) );
    double y = 0.0;
    int32_t len = (2*li) + (lf?1:0);
//  
//  
    if( fix & 1 )
      {
      int32_t x = 1;
      while( (y=SINE(x++,fp,am/2)) > 0.0 ) { *pad++ = _dith(0.0-y); *pad++ = _dith(0.0-y); }
      }
    while( ((pad+len)<=end) && many-- )
      {
      int32_t x = 0;
      while( (y=SINE(x++,fp,am)) >= 0.0 ) { *pad++ = _dith(y); *pad++ = _dith(y); }
                                          { *pad++ = _dith(y); *pad++ = _dith(y); }
      while( (y=SINE(x++,fp,am)) <  0.0 ) { *pad++ = _dith(y); *pad++ = _dith(y); }
      }
    if( fix & 2 )
      {
      int32_t x = 0;
      while( (y=SINE(x++,fp,am/2)) >= 0.0 ) { *pad++ = _dith(y); *pad++ = _dith(y); }
      }
    }
  return( pad );
  }

// scan given channel only, return average if more than noise


typedef struct
  {
  uint32_t rms;
  uint32_t pk;
  int32_t dc;
  }
  AudInfo;

// scan given channel only, return info if more than noise
/* this is NOT thread safe */
static AudInfo *audInfo( int32_t *pad, int32_t *end, uint32_t noise, AudInfo *info )
  {
  int32_t  min = *pad, max = *pad;
  int32_t  len = ( end - pad ) / 2;
  int64_t  sum = 0;
  int64_t  dc = 0;

  if( (end-pad) % 2 ) ++len;

//

  while( pad < end )
    {
    int32_t sam = *pad;
         if( sam < min ) min = sam;
    else if( sam > max ) max = sam;
    sum += abs( sam );
    dc  += sam;
    pad += 2;
    }

//

  info->pk = ( (max>>1) - (min>>1) ) * 2;

  if( info->pk > noise )
    {
    info->rms = sum / len;
    info->dc = dc / len;
    }
  else info->pk = 0;

  return( info );
  }


static int32_t *audCross( int32_t *pad, int32_t *end, uint32_t noise )
  {
  while( pad < end )
    {
    int64_t was = *pad, min = *pad, max = *pad;
    while( pad < end )
      {
      int32_t sam = *pad;
           if( was < 0 && sam > 0 ) break;
      else if( was > 0 && sam < 0 ) break;
      else
        {
        if( sam < min ) min = sam;
        if( sam > max ) max = sam;
        }
      if( sam ) was = sam;
      pad += 2;
      }
//  
    if( ( max - min ) > noise ) break;
    }
  return( pad );
  }


#define SYNC0 1000.0
#define SYNC1 1500.0
#define AMP0 2000000000.0
#define AMP1 1300000000.0  /* what is exact ratio? */
static double sync_scale = 1.0;      /* set by putSync for this call */
#define PUTZERO(T,E,F) audSine( T, E, SYNC0, AMP0 * sync_scale, 2, F )
#define PUTMARK(T,E,F) audSine( T, E, SYNC1, AMP1 * sync_scale, 3, F )

static int32_t *putOctal( int32_t *now, int32_t *end, int oct, int fix )
  {
  switch( oct )
    {
    case  0: now = PUTZERO( now, end, fix & 1 );
             now = PUTZERO( now, end, 0 );
             now = PUTZERO( now, end, fix & 2 ); break;

    case  1: now = PUTZERO( now, end, fix & 1 );
             now = PUTZERO( now, end, 0 );
             now = PUTMARK( now, end, fix & 2 ); break;

    case  2: now = PUTZERO( now, end, fix & 1 );
             now = PUTMARK( now, end, 0 );
             now = PUTZERO( now, end, fix & 2 ); break;

    case  3: now = PUTZERO( now, end, fix & 1 );
             now = PUTMARK( now, end, 0 );
             now = PUTMARK( now, end, fix & 2 ); break;

    case  4: now = PUTMARK( now, end, fix & 1 );
             now = PUTZERO( now, end, 0 );
             now = PUTZERO( now, end, fix & 2 ); break;

    case  5: now = PUTMARK( now, end, fix & 1 );
             now = PUTZERO( now, end, 0 );
             now = PUTMARK( now, end, fix & 2 ); break;

    case  6: now = PUTMARK( now, end, fix & 1 );
             now = PUTMARK( now, end, 0 );
             now = PUTZERO( now, end, fix & 2 ); break;

    case  7: now = PUTMARK( now, end, fix & 1 );
             now = PUTMARK( now, end, 0 );
             now = PUTMARK( now, end, fix & 2 ); break;

    default:  break;
    }
             
  return( now );
  }


static void putSync( char *data, int len, int second, double scale )
  {
  sync_scale = scale;
  int32_t *top = (int32_t*)data;
  int32_t *one = (int32_t*)(data+(len/3));
  int32_t *two = (int32_t*)(data+((len*2)/3));
  int32_t *end = (int32_t*)(data+len);

  memset( data, 0, len );

  top = putOctal( top, end, 1, 1 );
  one = putOctal( one, end, 1, 1 );
  two = putOctal( two, end, 1, 1 );

  top = putOctal( top, end, second % 8, 0 );
  one = putOctal( one, end, second % 8, 0 );
  two = putOctal( two, end, second % 8, 0 );

  top = putOctal( top, end, 1, 2 );
  one = putOctal( one, end, 2, 2 );
  two = putOctal( two, end, 3, 2 );
  }


static int64_t calcSync( char *bits, uint64_t second, int64_t here )
  {
  int sec = -1, ond = -1;

//

  if( memcmp( bits, "0000111", 7   ) == 0 ) bits += 7;
  else return( 0 );

       if( memcmp( bits, "000000",    6 ) == 0 ) { sec = 0; bits += 6; }
  else if( memcmp( bits, "0000111",   7 ) == 0 ) { sec = 1; bits += 7; }
  else if( memcmp( bits, "0011100",   7 ) == 0 ) { sec = 2; bits += 7; }
  else if( memcmp( bits, "00111111",  8 ) == 0 ) { sec = 3; bits += 8; }
  else if( memcmp( bits, "1110000",   7 ) == 0 ) { sec = 4; bits += 7; }
  else if( memcmp( bits, "11100111",  8 ) == 0 ) { sec = 5; bits += 8; }
  else if( memcmp( bits, "11111100",  8 ) == 0 ) { sec = 6; bits += 8; }
  else if( memcmp( bits, "111111111", 9 ) == 0 ) { sec = 7; bits += 9; }
  else return( 0 );

       if( memcmp( bits, "000000",    6 ) == 0 ) { ond = 0; bits += 6; }
  else if( memcmp( bits, "0000111",   7 ) == 0 ) { ond = 1; bits += 7; }
  else if( memcmp( bits, "0011100",   7 ) == 0 ) { ond = 2; bits += 7; }
  else if( memcmp( bits, "00111111",  8 ) == 0 ) { ond = 3; bits += 8; }
  else if( memcmp( bits, "1110000",   7 ) == 0 ) { ond = 4; bits += 7; }
  else if( memcmp( bits, "11100111",  8 ) == 0 ) { ond = 5; bits += 8; }
  else if( memcmp( bits, "11111100",  8 ) == 0 ) { ond = 6; bits += 8; }
  else if( memcmp( bits, "111111111", 9 ) == 0 ) { ond = 7; bits += 9; }
  else return( 0 );

  if( ond < 1 || ond > 3 ) return( 0 );

  if( second < sec ) second += 8;

  here -= ( ond - 1 ) * 16000;
  here += ( second - ( sec + 1 ) ) * 48000;


  return( here );
  }

static int64_t readSync( int32_t *pad, int32_t *end, uint64_t second, int noise, char invert )
  {
  char bits[31], *bit = bits, *bend = bits + 30;
  int32_t *top = audCross( pad, end, noise );
  int32_t *one = audCross( top, end, noise );
  int32_t *two = audCross( one, end, noise );
  int64_t here = -1, last = -1;

  *bend = 0;


// 0 is 48 samples per cycle, and 24 in the middle.
// 1 is 32 samples per cycle, and 16 in the middle.

  while( two < end )
    {
//  

    if( invert ? ( *top < 0 && *one > 0 && *two < 0 )
               : ( *top > 0 && *one < 0 && *two > 0 ) )
      {
      int pos = ( one - top ) / 2;
      int neg = ( two - one ) / 2;
      int off = abs( pos - neg );

      if( bit == bits ) here = ((top-pad)/2)-24;

//    

      if( off < 10 )
        {
        char x = pos + neg;
             if( ( x > 42 ) && ( x < 54 ) ) *bit++ = '0';
        else if( ( x > 26 ) && ( x < 38 ) ) *bit++ = '1';
        else bit = bits;
        *bit = 0;
//      if( bit > bits ) 
        if( bit > bend ) bit = bits;
        else if( ( bit - bits ) > 19 )
          {
          int64_t sync = calcSync( bits, second, here );
          if( sync > 0 )
            {
            /* zero crossings through real converters jitter by a sample,
               an exact match can miss every mark in the second */
            if( last > 0 && llabs( last - sync ) <= 2 ) return( sync );
            last = sync;
            bit = bits;
            }
          }
        }
      else bit = bits;
      }

//  
//  
    top = one;
    one = two;
    two = audCross( one, end, noise );
    if( ( ( two - one ) / 2 ) > ( ( 48 * 9 * 2 ) + 20 ) )
      {
      top = two;
      one = audCross( top, end, noise );
      two = audCross( one, end, noise );
      bit = bits;
      }
    }

  return( 0 );
  }



/* --- what asp4's findSync does, for the differential test --- */

void asp_sync_out_asp4( int32_t *out, int second )
  { putSync( (char *)out, ASP_SYNC_FRAMES * 8, second, 1.0 ); }

int64_t asp_sync_find_asp4( const int32_t *in, int second )
  {
  int32_t *pad = (int32_t *)in, *end = pad + 2 * ASP_SYNC_FRAMES;
  AudInfo l, r;
  audInfo( pad, end, 0, &l );
  audInfo( pad + 1, end, 0, &r );
  int64_t sync = r.rms > l.rms ? readSync( pad + 1, end, second % 8, r.rms, second % 2 )
                               : readSync( pad, end, second % 8, l.rms, second % 2 );
  return sync < 0 ? 0 : sync;
  }

/* --- the hunt --- */

#define MAXREAD 16

struct AspSync
  {
  AspSyncCfg c;
  AspSyncResult r;
  double out_db;
  int n;                           /* seconds sent */
  int64_t read[MAXREAD];
  int nread, inv, ch, fails;
  };

void asp_sync_defaults( AspSyncCfg *c )
  {
  c->start_db = -48; c->max_db = 0; c->step_db = 6; c->target_db = -12;
  c->need = 3; c->tol = 2; c->give_up = 60; c->output_muted = 0;
  }

AspSync *asp_sync_new( const AspSyncCfg *c )
  {
  AspSync *s = calloc( 1, sizeof *s );
  if( c ) s->c = *c; else asp_sync_defaults( &s->c );
  if( s->c.need < 1 ) s->c.need = 1;
  if( s->c.need > MAXREAD ) s->c.need = MAXREAD;
  s->out_db = s->c.output_muted ? s->c.max_db : s->c.start_db;
  if( s->out_db > s->c.max_db ) s->out_db = s->c.max_db;
  return s;
  }
void asp_sync_free( AspSync *s ) { free( s ); }
const AspSyncResult *asp_sync_result( const AspSync *s ) { return &s->r; }
double asp_sync_level( const AspSync *s ) { return s->out_db; }

void asp_sync_out( AspSync *s, int32_t *out )
  { putSync( (char *)out, ASP_SYNC_FRAMES * 8, s->n++, pow( 10, s->out_db / 20 ) ); }

static int cmp64( const void *a, const void *b )
  { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }

static double peak_db( const int32_t *p, int ch )
  {
  int64_t pk = 0;
  for( int i = 0; i < ASP_SYNC_FRAMES; i++ ) { int64_t v = llabs( (int64_t)p[2*i+ch] ); if( v > pk ) pk = v; }
  return pk ? 20 * log10( pk / 2147483648.0 ) : -200;
  }

int asp_sync_in( AspSync *s, const int32_t *in )
  {
  int32_t *pad = (int32_t *)in, *end = pad + 2 * ASP_SYNC_FRAMES;
  int second = s->n;                                   /* the second just sent is n - 1 */
  AudInfo li, ri;
  audInfo( pad, end, 0, &li );
  audInfo( pad + 1, end, 0, &ri );
  int first = ri.rms > li.rms, got = 0, ch = 0, inv = 0;
  int64_t sync = 0;
  /* the louder channel first, then the other, each both ways up; the first that reads wins */
  for( int a = 0; a < 4 && ! got; a++ )
    {
    ch = a < 2 ? first : ! first;
    inv = a & 1;
    uint32_t noise = ( ch ? ri : li ).rms;
    sync = readSync( pad + ch, end, second % 8, (int)noise, (char)inv );
    if( sync > 0 ) got = 1;
    }

  if( ! got )
    {
    if( ++s->fails >= 3 ) s->nread = 0;                /* lost it: start the readings again */
    s->out_db += s->c.step_db;
    if( s->out_db > s->c.max_db ) s->out_db = s->c.max_db;
    s->r.seconds = s->n;
    return s->c.give_up && s->n >= s->c.give_up ? -1 : 0;
    }
  s->fails = 0;

  double pk = peak_db( pad, ch ), err = s->c.target_db - pk;
  if( ( s->nread && ( inv != s->inv || ch != s->ch ) ) )
    s->nread = 0;                                      /* the path changed under us */
  s->inv = inv; s->ch = ch;

  /* trim the output so the input peak lands on the target; readings taken at another level are dropped */
  if( fabs( err ) > 2 && ! ( err > 0 && s->out_db >= s->c.max_db ) )
    {
    double step = err > 30 ? 30 : ( err < -30 ? -30 : err );   /* the path is linear: this lands it */
    s->out_db += step;
    if( s->out_db > s->c.max_db ) s->out_db = s->c.max_db;
    s->nread = 0;
    s->r.seconds = s->n;
    return s->c.give_up && s->n >= s->c.give_up ? -1 : 0;
    }

  if( s->nread == s->c.need )                          /* slide */
    { memmove( s->read, s->read + 1, ( s->c.need - 1 ) * sizeof *s->read ); s->nread--; }
  s->read[s->nread++] = sync;
  s->r.seconds = s->n;
  if( s->nread < s->c.need ) return s->c.give_up && s->n >= s->c.give_up ? -1 : 0;

  int64_t sorted[MAXREAD];
  memcpy( sorted, s->read, s->nread * sizeof *sorted );
  qsort( sorted, s->nread, sizeof *sorted, cmp64 );
  s->r.spread = (int)( sorted[s->nread - 1] - sorted[0] );
  if( s->r.spread > s->c.tol ) return s->c.give_up && s->n >= s->c.give_up ? -1 : 0;   /* unsteady */
  s->r.frames = sorted[s->nread / 2];
  s->r.inverted = inv; s->r.channel = ch;
  s->r.out_db = s->out_db; s->r.in_peak_db = pk;
  return 1;
  }
