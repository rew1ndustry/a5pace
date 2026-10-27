#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
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

int asp_audio_read( const char *path, int32_t **pcm, int64_t *frames, int *channels, int *rate, char *err, int errn )
  {
  size_t pl = strlen( path );
  if( pl > 5 && ! strcmp( path + pl - 5, ".flac" ) )
    {
    int bits = 0;
    uint64_t fr;
    if( asp_flac_info( path, NULL, &fr, channels, rate, &bits ) ) { snprintf( err, errn, "%s: not a flac file", path ); return -1; }
    if( *channels > 2 ) { snprintf( err, errn, "%s: %d channels (one or two, please)", path, *channels ); return -1; }
    if( *rate != RATE ) { snprintf( err, errn, "%s is %d Hz; aspace works at 48000 (sox in.flac -r 48000 out.flac converts)", path, *rate ); return -1; }
    int ch; 
    if( asp_pcm_load( path, pcm, frames, &ch ) ) { snprintf( err, errn, "%s: cannot decode", path ); return -1; }
    if( bits < 32 ) for( int64_t i = 0; i < *frames * ch; i++ ) (*pcm)[i] = (int32_t)( (uint32_t)(*pcm)[i] << ( 32 - bits ) );
    return 0;
    }
  FILE *f = fopen( path, "rb" );
  if( ! f ) { snprintf( err, errn, "%s: cannot open", path ); return -1; }
  unsigned char h[12], ck[8];
  int fmt = 0, ch = 0, bits = 0, r = 0, found = 0;
  long dstart = 0; uint32_t dlen = 0;
  if( fread( h, 1, 12, f ) != 12 || memcmp( h, "RIFF", 4 ) || memcmp( h + 8, "WAVE", 4 ) )
    { fclose( f ); snprintf( err, errn, "%s: not a wav or flac file (convert other formats first, e.g. with sox)", path ); return -1; }
  while( fread( ck, 1, 8, f ) == 8 )
    {
    uint32_t len = asp_ld32( ck + 4 );
    if( ! memcmp( ck, "fmt ", 4 ) )
      {
      unsigned char b[40] = { 0 };
      if( len > 40 || fread( b, 1, len, f ) != len ) break;
      fmt = (int)le16( b ); ch = (int)le16( b + 2 ); r = (int)asp_ld32( b + 4 ); bits = (int)le16( b + 14 );
      if( fmt == 0xFFFE && len >= 26 ) fmt = (int)le16( b + 24 );          /* extensible: the sub format */
      if( len & 1 ) fgetc( f );
      }
    else if( ! memcmp( ck, "data", 4 ) ) { dstart = ftell( f ); dlen = len; found = 1; break; }
    else { fseek( f, (long)len + ( len & 1 ), SEEK_CUR ); }
    }
  if( ! found || ! ch ) { fclose( f ); snprintf( err, errn, "%s: no audio found", path ); return -1; }
  if( ch > 2 ) { fclose( f ); snprintf( err, errn, "%s: %d channels (one or two, please)", path, ch ); return -1; }
  if( r != RATE ) { fclose( f ); snprintf( err, errn, "%s is %d Hz; aspace works at 48000 (sox in.wav -r 48000 out.wav converts)", path, r ); return -1; }
  if( ! ( ( fmt == 1 && ( bits == 16 || bits == 24 || bits == 32 ) ) || ( fmt == 3 && bits == 32 ) ) )
    { fclose( f ); snprintf( err, errn, "%s: %d bit format %d not handled (pcm 16, 24, 32 or float 32)", path, bits, fmt ); return -1; }
  int bytes = bits / 8;
  int64_t n = dlen / ( (int64_t)bytes * ch );
  unsigned char *raw = malloc( (size_t)n * bytes * ch );
  fseek( f, dstart, SEEK_SET );
  size_t got = fread( raw, (size_t)bytes * ch, (size_t)n, f );
  fclose( f );
  if( (int64_t)got < n ) n = (int64_t)got;
  int32_t *o = malloc( (size_t)n * ch * 4 );
  for( int64_t i = 0; i < n * ch; i++ )
    {
    const unsigned char *p = raw + i * bytes;
    if( fmt == 3 ) { union { uint32_t u; float f; } c; c.u = asp_ld32( p ); double v = c.f * 2147483648.0; o[i] = (int32_t)( v > 2147483647 ? 2147483647 : v < -2147483648.0 ? -2147483647 : v ); }
    else if( bits == 16 ) o[i] = (int32_t)( (uint32_t)le16( p ) << 16 );
    else if( bits == 24 ) o[i] = (int32_t)( ( (uint32_t)p[0] << 8 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 24 ) );
    else o[i] = (int32_t)asp_ld32( p );
    }
  free( raw );
  *pcm = o; *frames = n; *channels = ch; *rate = r;
  return 0;
  }

/* --- into a session -------------------------------------------------------------------------------------- */
static void lev_s( int l, char *b ) { if( l == ASP_LEV_OFF ) strcpy( b, "-" ); else sprintf( b, "%d", l ); }

int64_t asp_click_insert( AspStore *s, AspTank *t, int64_t iter, int32_t *mono, int64_t frames, const char *style, int lev_l, int lev_r, const char *meta )
  {
  char tmp[1300], st[500], l[8], r[8], args[600];
  AspTake k; int64_t pk = 0;
  if( asp_tmp_path( s, tmp, sizeof tmp ) ) return -1;
  AspFlacEnc *e = asp_flac_enc_open( tmp, 1, RATE );
  if( ! e ) return -1;
  for( int64_t i = 0; i < frames; i += 4096 )
    asp_flac_enc_write( e, mono + i, (unsigned)( frames - i < 4096 ? frames - i : 4096 ) );
  if( asp_flac_enc_close( e ) ) return -1;
  for( int64_t i = 0; i < frames; i++ ) { int64_t v = mono[i] < 0 ? -(int64_t)mono[i] : mono[i]; if( v > pk ) pk = v; }
  lev_s( lev_l, l ); lev_s( lev_r, r );
  snprintf( st, sizeof st, "{\"status\":\"mix\",\"lev\":[\"%s\",\"%s\"],\"flip\":0,\"mono\":1,\"src\":0,\"len\":%lld,\"gone\":0,\"peak\":%lld,\"generated\":\"click\",\"style\":\"%s\"}",
            l, r, (long long)frames, (long long)pk, style );
  if( asp_take_adopt( s, iter, 0, tmp, "mix", st, 0, &k ) ) return -1;
  snprintf( args, sizeof args, "{\"take\":%lld,\"style\":\"%s\",%s}", (long long)k.id, style, meta ? meta : "\"meta\":0" );
  asp_log( s, "click", "click_added", args, 0 );
  if( t && asp_move_take( t, s, k.id, 0, -1, (AspPlace)ASP_PLACE_OFF, (AspPlace){ lev_l, lev_r, 0 } ) ) return -1;
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
    if( t && asp_move_take( t, s, g->id[i], 0, -1, (AspPlace){ g->st[i].l, g->st[i].r, g->st[i].flip }, (AspPlace)ASP_PLACE_OFF ) ) { free( g ); return -1; }
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
