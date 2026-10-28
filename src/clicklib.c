#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include "clicklib.h"
#include "aspengine.h"
#include "flacio.h"
#include "plat.h"

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif
#define RATE 48000

const char *asp_click_styles = "beats bars eighths triplets sixteenths offbeat";

/* --- time signatures ---------------------------------------------------------------------------------- */
int asp_meter_parse( const char *sig, AspMeter *m )
  {
  int n, d;
  if( ! sig || sscanf( sig, "%d/%d", &n, &d ) != 2 || n < 1 || n > 32 ) return -1;
  if( d != 1 && d != 2 && d != 4 && d != 8 && d != 16 ) return -1;
  m->n = n; m->d = d;
  return 0;
  }

static int compound( const AspMeter *m ) { return m->d == 8 && m->n >= 6 && m->n % 3 == 0; }
int asp_meter_beats( const AspMeter *m ) { return compound( m ) ? m->n / 3 : m->n; }
static double beat_quarters( const AspMeter *m ) { return compound( m ) ? 1.5 : 4.0 / m->d; }
double asp_meter_quarters( const AspMeter *m ) { return m->bars * m->n * 4.0 / m->d; }
double asp_meter_seconds( const AspMeter *m ) { return asp_meter_quarters( m ) * 60.0 / m->bpm; }

int asp_click_events( const AspMeter *m, const char *style, int64_t loop, AspClickEv *ev, int max )
  {
  int beats = asp_meter_beats( m ), n = 0, comp = compound( m );
  double bl = beat_quarters( m ), qpb = m->n * 4.0 / m->d, total = asp_meter_quarters( m );
  int div = 1, offbeat = 0, bars_only = 0;
  if( ! strcmp( style, "bars" ) ) bars_only = 1;
  else if( ! strcmp( style, "eighths" ) ) div = comp ? 3 : 2;
  else if( ! strcmp( style, "triplets" ) ) div = 3;
  else if( ! strcmp( style, "sixteenths" ) ) div = comp ? 6 : 4;
  else if( ! strcmp( style, "offbeat" ) ) { div = comp ? 3 : 2; offbeat = 1; }
  else if( strcmp( style, "beats" ) ) return -1;
  for( int b = 0; b < m->bars; b++ )
    for( int k = 0; k < beats; k++ )
      for( int s = 0; s < div; s++ )
        {
        int strength;
        if( bars_only ) { if( k || s ) continue; strength = 0; }
        else if( offbeat ) { if( s == 0 ) continue; strength = 1; if( comp && s != 1 ) continue; }
        else strength = s ? 2 : ( k ? 1 : 0 );
        if( n >= max ) return n;
        double q = b * qpb + k * bl + s * bl / div;
        ev[n].at = (int64_t)llround( q * (double)loop / total ); if( ev[n].at >= loop ) ev[n].at = loop - 1;
        ev[n].strength = strength; n++;
        }
  return n;
  }

/* --- the sounds ---------------------------------------------------------------------------------------- */
static int32_t *click_synth( double hz, double peak_db, double ms, double tau_ms, int64_t *len )
  {
  int64_t n = (int64_t)( ms / 1000 * RATE );
  int32_t *p = malloc( (size_t)n * 4 );
  double amp = 2147483647.0 * pow( 10, peak_db / 20 );
  for( int64_t i = 0; i < n; i++ )
    {
    double t = (double)i / RATE, env = exp( -t / ( tau_ms / 1000 ) ) * ( i < 48 ? i / 48.0 : 1 ) * ( i > n - 96 ? ( n - i ) / 96.0 : 1 );
    p[i] = (int32_t)( amp * env * sin( 2 * M_PI * hz * t ) );
    }
  *len = n;
  return p;
  }

void asp_click_sounds_default( AspClickSounds *s )
  {
  s->pcm[0] = click_synth( 1500, -10, 40, 7, &s->len[0] );
  s->pcm[1] = click_synth( 1000, -16, 40, 7, &s->len[1] );
  s->pcm[2] = click_synth( 800, -24, 30, 5, &s->len[2] );
  }

void asp_click_sounds_free( AspClickSounds *s ) { for( int i = 0; i < 3; i++ ) { free( s->pcm[i] ); s->pcm[i] = NULL; } }

int asp_click_sound_file( AspClickSounds *s, int which, const char *path, char *err, int errn )
  {
  int32_t *p; int64_t n; int ch, rate;
  if( asp_audio_read( path, &p, &n, &ch, &rate, err, errn ) ) return -1;
  int32_t *m = malloc( (size_t)n * 4 );
  for( int64_t i = 0; i < n; i++ ) m[i] = ch == 2 ? (int32_t)( ( (int64_t)p[2*i] + p[2*i+1] ) / 2 ) : p[i];
  free( p );
  free( s->pcm[which] ); s->pcm[which] = m; s->len[which] = n;
  return 0;
  }

int32_t *asp_click_render( const AspClickEv *ev, int nev, int64_t loop, const AspClickSounds *s )
  {
  int32_t *out = calloc( (size_t)loop, 4 );
  for( int e = 0; e < nev; e++ )
    {
    const int32_t *p = s->pcm[ev[e].strength]; int64_t len = s->len[ev[e].strength];
    for( int64_t i = 0; i < len; i++ )
      {
      int64_t at = ( ev[e].at + i ) % loop, v = (int64_t)out[at] + p[i];            /* the tail of a click at the end wraps round */
      out[at] = (int32_t)( v > 2147483647 ? 2147483647 : v < -2147483647 ? -2147483647 : v );
      }
    }
  return out;
  }

/* --- audio files in ------------------------------------------------------------------------------------- */
static uint32_t le16( const unsigned char *p ) { return p[0] | p[1] << 8; }

struct AspAudio { AspFlacR *fl; FILE *f; int ch, bits, fmt; int64_t left; };

/* an audio file read a block at a time: wav (pcm 16/24/32, float 32) or flac, one or two channels at 48000. the frame count
   is what the file says (a flac may say 0, not knowing) */
AspAudio *asp_audio_open( const char *path, int *channels, int64_t *frames, char *err, int errn )
  {
  size_t pl = strlen( path );
  AspAudio *a = calloc( 1, sizeof *a );
  if( ! a ) return NULL;
  if( pl > 5 && ! strcmp( path + pl - 5, ".flac" ) )
    {
    int rate = 0; uint64_t fr;
    a->fl = asp_flac_r_open( path, &a->ch, &rate, &a->bits, &fr );
    if( ! a->fl ) { snprintf( err, errn, "%s: not a flac file", path ); free( a ); return NULL; }
    if( a->ch > 2 ) { snprintf( err, errn, "%s: %d channels (one or two, please)", path, a->ch ); asp_audio_close( a ); return NULL; }
    if( rate != RATE ) { snprintf( err, errn, "%s is %d Hz; aspace works at 48000 (sox in.flac -r 48000 out.flac converts)", path, rate ); asp_audio_close( a ); return NULL; }
    *channels = a->ch; *frames = (int64_t)fr;
    return a;
    }
  a->f = fopen( path, "rb" );
  if( ! a->f ) { snprintf( err, errn, "%s: cannot open", path ); free( a ); return NULL; }
  unsigned char h[12], ck[8];
  int r = 0, found = 0;
  uint32_t dlen = 0;
  if( fread( h, 1, 12, a->f ) != 12 || memcmp( h, "RIFF", 4 ) || memcmp( h + 8, "WAVE", 4 ) )
    { snprintf( err, errn, "%s: not a wav or flac file (convert other formats first, e.g. with sox)", path ); asp_audio_close( a ); return NULL; }
  while( fread( ck, 1, 8, a->f ) == 8 )
    {
    uint32_t len = asp_ld32( ck + 4 );
    if( ! memcmp( ck, "fmt ", 4 ) )
      {
      unsigned char b[40] = { 0 };
      if( len > 40 || fread( b, 1, len, a->f ) != len ) break;
      a->fmt = (int)le16( b ); a->ch = (int)le16( b + 2 ); r = (int)asp_ld32( b + 4 ); a->bits = (int)le16( b + 14 );
      if( a->fmt == 0xFFFE && len >= 26 ) a->fmt = (int)le16( b + 24 );          /* extensible: the sub format */
      if( len & 1 ) fgetc( a->f );
      }
    else if( ! memcmp( ck, "data", 4 ) ) { dlen = len; found = 1; break; }
    else { fseek( a->f, (long)len + ( len & 1 ), SEEK_CUR ); }
    }
  if( ! found || ! a->ch ) { snprintf( err, errn, "%s: no audio found", path ); asp_audio_close( a ); return NULL; }
  if( a->ch > 2 ) { snprintf( err, errn, "%s: %d channels (one or two, please)", path, a->ch ); asp_audio_close( a ); return NULL; }
  if( r != RATE ) { snprintf( err, errn, "%s is %d Hz; aspace works at 48000 (sox in.wav -r 48000 out.wav converts)", path, r ); asp_audio_close( a ); return NULL; }
  if( ! ( ( a->fmt == 1 && ( a->bits == 16 || a->bits == 24 || a->bits == 32 ) ) || ( a->fmt == 3 && a->bits == 32 ) ) )
    { snprintf( err, errn, "%s: %d bit format %d not handled (pcm 16, 24, 32 or float 32)", path, a->bits, a->fmt ); asp_audio_close( a ); return NULL; }
  a->left = dlen / ( (int64_t)( a->bits / 8 ) * a->ch );
  *channels = a->ch; *frames = a->left;
  return a;
  }

int asp_audio_get( AspAudio *a, int32_t *pcm, unsigned max )
  {
  if( a->fl )
    {
    int n = asp_flac_r_get( a->fl, pcm, max );
    if( n > 0 && a->bits < 32 ) for( int i = 0; i < n * a->ch; i++ ) pcm[i] = (int32_t)( (uint32_t)pcm[i] << ( 32 - a->bits ) );
    return n;
    }
  unsigned char raw[4096 * 2 * 4];
  int bytes = a->bits / 8;
  if( max > 4096 ) max = 4096;
  if( (int64_t)max > a->left ) max = (unsigned)a->left;
  size_t got = max ? fread( raw, (size_t)bytes * a->ch, max, a->f ) : 0;
  a->left -= (int64_t)got;
  for( size_t i = 0; i < got * a->ch; i++ )
    {
    const unsigned char *p = raw + i * bytes;
    if( a->fmt == 3 ) { union { uint32_t u; float f; } c; c.u = asp_ld32( p ); double v = c.f * 2147483648.0; pcm[i] = (int32_t)( v > 2147483647 ? 2147483647 : v < -2147483648.0 ? -2147483647 : v ); }
    else if( a->bits == 16 ) pcm[i] = (int32_t)( (uint32_t)le16( p ) << 16 );
    else if( a->bits == 24 ) pcm[i] = (int32_t)( ( (uint32_t)p[0] << 8 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 24 ) );
    else pcm[i] = (int32_t)asp_ld32( p );
    }
  return (int)got;
  }

void asp_audio_close( AspAudio *a )
  { if( a ) { asp_flac_r_close( a->fl ); if( a->f ) fclose( a->f ); free( a ); } }

/* a whole file in memory: for short ones (the sounds of a click) */
int asp_audio_read( const char *path, int32_t **pcm, int64_t *frames, int *channels, int *rate, char *err, int errn )
  {
  int64_t told; int ch;
  AspAudio *a = asp_audio_open( path, &ch, &told, err, errn );
  if( ! a ) return -1;
  int64_t cap = told > 0 ? told : 1 << 16, n = 0;
  int32_t *p = malloc( (size_t)cap * ch * 4 );
  int k = 0;
  while( p )
    {
    if( n + 4096 > cap ) { cap *= 2; int32_t *q = realloc( p, (size_t)cap * ch * 4 ); if( ! q ) { free( p ); p = NULL; break; } p = q; }
    k = asp_audio_get( a, p + n * ch, 4096 );
    if( k <= 0 ) break;
    n += k;
    }
  asp_audio_close( a );
  if( ! p || k < 0 ) { free( p ); snprintf( err, errn, "%s: cannot decode", path ); return -1; }
  *pcm = p; *frames = n; *channels = ch; *rate = RATE;
  return 0;
  }

/* --- into a session -------------------------------------------------------------------------------------- */
static void lev_s( int l, char *b ) { if( l == ASP_LEV_OFF ) strcpy( b, "-" ); else sprintf( b, "%d", l ); }

/* pcm as a take waiting to be adopted, header and all */
static int write_take( const char *path, const int32_t *pcm, int64_t frames, int channels, AspHdr *h )
  {
  unsigned char artist[ASP_ARTIST];
  if( asp_artist_local( artist, 1 ) ) return -1;
  AspTakeW *w = asp_tw_open( path, artist, channels, RATE, 0 );
  if( ! w ) return -1;
  for( int64_t i = 0; i < frames; i += 4096 )
    if( asp_tw_put( w, pcm + i * channels, (unsigned)( frames - i < 4096 ? frames - i : 4096 ) ) ) { asp_tw_abort( w ); unlink( path ); return -1; }
  if( asp_tw_close( w, 0, h ) ) { unlink( path ); return -1; }
  return 0;
  }

static int64_t peak_of( const AspHdr *h )
  { uint32_t pk = 0; for( int i = 0; i < h->channels; i++ ) if( h->peak[i] > pk ) pk = h->peak[i]; return pk; }

static int asp_pcm_write_part( const char *dir, const int32_t *pcm, int64_t frames, int channels, char *name, size_t n )
  {
  static int serial;
  char path[1300];
  snprintf( path, sizeof path, "%s/tmp", dir ); mkdir( path, 0777 );
  snprintf( name, n, "gen_%d_%d.part", (int)getpid(), serial++ );
  snprintf( path, sizeof path, "%s/tmp/%s", dir, name );
  return write_take( path, pcm, frames, channels, NULL );
  }
int asp_click_write_part( const char *dir, const int32_t *mono, int64_t frames, char *name, size_t n )
  { return asp_pcm_write_part( dir, mono, frames, 1, name, n ); }

/* the next free place in the 90000 series (the old negative iterations count as 90000 - n) */
int64_t asp_gen_next_iter( AspStore *s )
  {
  AspTake tk; int64_t it = 90001;
  while( ! asp_take_find( s, it, 0, &tk ) || ! asp_take_find( s, 90000 - it, 0, &tk ) ) it++;
  return it;
  }

int64_t asp_click_insert( AspStore *s, AspTank *t, int64_t iter, int32_t *mono, int64_t frames, const char *style, int lev_l, int lev_r, const char *meta, int in )
  {
  char tmp[1300], st[500], l[8], r[8], args[600];
  AspTake k; AspHdr h;
  if( asp_tmp_path( s, tmp, sizeof tmp ) || write_take( tmp, mono, frames, 1, &h ) ) return -1;
  int64_t pk = peak_of( &h );
  lev_s( lev_l, l ); lev_s( lev_r, r );
  snprintf( st, sizeof st, "{\"status\":\"%s\",\"lev\":[\"%s\",\"%s\"],\"flip\":0,\"mono\":1,\"src\":0,\"len\":%lld,\"gone\":0,\"peak\":%lld,\"generated\":\"click\",\"style\":\"%s\"}",
            in ? "mix" : "off", l, r, (long long)frames, (long long)pk, style );
  if( asp_take_adopt( s, iter, 0, tmp, in ? "mix" : "off", st, 0, &k ) ) return -1;
  snprintf( args, sizeof args, "{\"take\":%lld,\"style\":\"%s\",\"in\":%d,%s}", (long long)k.id, style, in, meta ? meta : "\"meta\":0" );
  asp_log( s, "click", "click_added", args, 0 );
  if( in && t && asp_move_take( t, s, k.id, -1, 0, -1, (AspPlace)ASP_PLACE_OFF, (AspPlace){ lev_l, lev_r, 0 } ) ) return -1;
  if( t ) asp_tank_set_seq( t, (uint64_t)asp_log_last( s ) );
  return k.id;
  }

typedef struct { int64_t id[200]; AspTakeState st[200]; int n; } Gen;
static void gen_cb( int64_t id, const char *json, void *u )
  {
  Gen *g = u;
  if( id == 0 || ! strstr( json, "\"generated\":\"click\"" ) || g->n >= 200 ) return;
  g->id[g->n] = id; asp_parse_state( json, &g->st[g->n] ); g->n++;
  }

int asp_click_clear( AspStore *s, AspTank *t )
  {
  Gen *g = calloc( 1, sizeof *g );
  int cleared = 0;
  asp_fold( s, -1, gen_cb, g );
  for( int i = 0; i < g->n; i++ )
    {
    char st[400], l[8], r[8];
    if( ! strcmp( g->st[i].status, "off" ) ) continue;
    if( t && asp_move_take( t, s, g->id[i], -1, 0, -1, (AspPlace){ g->st[i].l, g->st[i].r, g->st[i].flip }, (AspPlace)ASP_PLACE_OFF ) ) { free( g ); return -1; }
    lev_s( g->st[i].l, l ); lev_s( g->st[i].r, r );
    snprintf( st, sizeof st, "{\"status\":\"off\",\"lev\":[\"%s\",\"%s\"],\"flip\":0,\"mono\":1,\"src\":0,\"gone\":0,\"generated\":\"click\"}", l, r );
    asp_take_set_state( s, g->id[i], st, "click", 0 );
    cleared++;
    }
  asp_log( s, "click", "click_cleared", "{}", 0 );
  if( t ) asp_tank_set_seq( t, (uint64_t)asp_log_last( s ) );
  free( g );
  return cleared;
  }

/* an audio file written, a block at a time, as a take waiting to be adopted; h says what it came to */
int asp_audio_to_take( const char *src, const char *part, AspHdr *h, char *err, int errn )
  {
  int ch; int64_t told;
  AspAudio *a = asp_audio_open( src, &ch, &told, err, errn );
  if( ! a ) return -1;
  unsigned char artist[ASP_ARTIST];
  AspTakeW *w = asp_artist_local( artist, 1 ) ? NULL : asp_tw_open( part, artist, ch, RATE, 0 );
  int32_t *buf = malloc( (size_t)4096 * ch * 4 );
  int k = 0, rc = 0;
  if( ! w || ! buf ) { snprintf( err, errn, "%s: cannot make the track", src ); asp_audio_close( a ); free( buf ); if( w ) { asp_tw_abort( w ); unlink( part ); } return -1; }
  while( ! rc && ( k = asp_audio_get( a, buf, 4096 ) ) > 0 ) rc = asp_tw_put( w, buf, (unsigned)k );
  free( buf ); asp_audio_close( a );
  if( rc || k < 0 ) { asp_tw_abort( w ); unlink( part ); snprintf( err, errn, "%s: cannot decode", src ); return -1; }
  if( asp_tw_close( w, 0, h ) ) { unlink( part ); snprintf( err, errn, "%s: cannot write the track", src ); return -1; }
  if( h->frames < 1 ) { unlink( part ); snprintf( err, errn, "%s is empty", src ); return -1; }
  return 0;
  }

/* the same for a session that is running: the take goes under DIR/tmp, its name returned for the running aspace to adopt */
int asp_audio_import_part( const char *dir, const char *src, char *name, size_t n, AspHdr *h, char *err, int errn )
  {
  static int serial;
  char path[1300];
  snprintf( path, sizeof path, "%s/tmp", dir ); mkdir( path, 0777 );
  snprintf( name, n, "gen_%d_%d.part", (int)getpid(), serial++ );
  snprintf( path, sizeof path, "%s/tmp/%s", dir, name );
  return asp_audio_to_take( src, path, h, err, errn );
  }

/* a take written by asp_audio_to_take, as a take of its own in a session that is not running. in puts it into the loop */
int64_t asp_import_adopt( AspStore *s, AspTank *t, int64_t iter, const char *part, int lev_l, int lev_r, int in )
  {
  char st[500], l[8], r[8], args[300];
  AspTake k; AspHdr h;
  if( asp_take_header( part, &h ) ) return -1;
  int64_t pk = peak_of( &h ), frames = (int64_t)h.frames;
  int channels = h.channels;
  lev_s( lev_l, l ); lev_s( lev_r, r );
  snprintf( st, sizeof st, "{\"status\":\"%s\",\"lev\":[\"%s\",\"%s\"],\"flip\":0,\"mono\":%d,\"src\":0,\"len\":%lld,\"gone\":0,\"peak\":%lld,\"generated\":\"file\"}",
            in ? "mix" : "off", l, r, channels == 1, (long long)frames, (long long)pk );
  if( asp_take_adopt( s, iter, 0, part, in ? "mix" : "off", st, 0, &k ) ) return -1;
  snprintf( args, sizeof args, "{\"take\":%lld,\"iter\":%lld,\"len\":%lld,\"mono\":%d,\"in\":%d}", (long long)k.id, (long long)iter, (long long)frames, channels == 1, in );
  asp_log( s, "click", "take_imported", args, 0 );
  if( in && t && asp_move_take( t, s, k.id, -1, 0, -1, (AspPlace)ASP_PLACE_OFF, (AspPlace){ lev_l, lev_r, 0 } ) ) return -1;
  if( t ) asp_tank_set_seq( t, (uint64_t)asp_log_last( s ) );
  return k.id;
  }
