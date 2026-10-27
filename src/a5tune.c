/* a5tune: try gate settings against a capture and pick the best (the engine behind a5cal).
     a5tune synth SCRIPT OUT.raw OUT.windows [--seed N] [--hum HZ]   a made-up capture of a script, for testing
     a5tune windows SCRIPT OUT.windows                                the timeline of a script (a5cal capture uses it)
     a5tune eval CAPTURE WINDOWS ["gate=18;ring=7.5;lowcut=190;strict=1"]   how one setting does, window by window
     a5tune tune CAPTURE WINDOWS [--lowest HZ] [--base "settings"]    search, print the proposal as a settings text
     a5tune floor CAPTURE WINDOWS                                     the noise floor, DC and hum lines in the quiet windows
   a capture is raw s32 little endian stereo 48 kHz (.raw) or flac. WINDOWS is "class start end prompt" a line (seconds):
   Q quiet, G good (must be caught), B bad (must not be), X not judged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "gatesim.h"
#include "aspengine.h"
#include "settings.h"
#include "plat.h"

#define RATE 48000.0
#define MAXW 200
#define MAXE 4000

typedef struct { char cls; double s, e; char label[100]; int hit; } Win;

static int load_script( const char *path, Win *w, double gap )
  {
  FILE *f = fopen( path, "r" );
  char line[300];
  int n = 0;
  double t = 1.0;
  if( ! f ) return -1;
  while( fgets( line, sizeof line, f ) && n < MAXW )
    {
    char *p = line, cls; double secs; int used = 0;
    while( isspace( (unsigned char)*p ) ) p++;
    if( ! *p || *p == '#' ) continue;
    if( sscanf( p, "%c %lf %n", &cls, &secs, &used ) < 2 || ! strchr( "QGBX", cls ) ) continue;
    w[n].cls = cls; w[n].s = t; w[n].e = t + secs; w[n].hit = 0;
    snprintf( w[n].label, sizeof w[n].label, "%s", p + used ); w[n].label[strcspn( w[n].label, "\r\n" )] = 0;
    t += secs + gap; n++;
    }
  fclose( f );
  return n;
  }

static int load_windows( const char *path, Win *w )
  {
  FILE *f = fopen( path, "r" );
  char line[300];
  int n = 0;
  if( ! f ) return -1;
  while( fgets( line, sizeof line, f ) && n < MAXW )
    {
    char cls; double s, e; int used = 0;
    if( line[0] == '#' || sscanf( line, "%c %lf %lf %n", &cls, &s, &e, &used ) < 3 ) continue;
    w[n].cls = cls; w[n].s = s; w[n].e = e; w[n].hit = 0;
    snprintf( w[n].label, sizeof w[n].label, "%s", line + used ); w[n].label[strcspn( w[n].label, "\r\n" )] = 0;
    n++;
    }
  fclose( f );
  return n;
  }

static int32_t *load_capture( const char *path, int64_t *frames )
  {
  size_t l = strlen( path );
  if( l > 5 && ! strcmp( path + l - 5, ".flac" ) )
    {
    int32_t *p; int ch;
    if( asp_pcm_load( path, &p, frames, &ch ) ) return NULL;
    if( ch == 2 ) return p;
    int32_t *st = malloc( (size_t)*frames * 8 );
    for( int64_t i = 0; i < *frames; i++ ) st[2*i] = st[2*i+1] = p[i];
    free( p );
    return st;
    }
  FILE *f = fopen( path, "rb" );
  if( ! f ) return NULL;
  ASP_FSEEK( f, 0, SEEK_END );
  int64_t n = ASP_FTELL( f ) / 8;
  ASP_FSEEK( f, 0, SEEK_SET );
  unsigned char *b = malloc( (size_t)n * 8 );
  int32_t *p = malloc( (size_t)n * 8 );
  if( fread( b, 8, (size_t)n, f ) != (size_t)n ) { fclose( f ); free( b ); free( p ); return NULL; }
  fclose( f );
  for( int64_t i = 0; i < 2 * n; i++ ) p[i] = (int32_t)asp_ld32( b + 4 * i );
  free( b );
  *frames = n;
  return p;
  }

/* --- scoring ----------------------------------------------------------------------------------------- */
typedef struct { int misses, fps, stray, events, good, bad; double score; } Score;

static int in_win( const Win *w, double t ) { return t >= w->s - 0.5 && t < w->e; }

static Score score_it( const AspGateEvent *ev, int ne, Win *w, int nw )
  {
  Score sc = { 0 };
  for( int i = 0; i < nw; i++ ) w[i].hit = 0;
  for( int k = 0; k < ne; k++ )
    {
    double t = ev[k].start / RATE, te = ev[k].end / RATE;
    int inwin = 0;
    if( ! ev[k].kept ) continue;
    sc.events++;
    for( int i = 0; i < nw; i++ )
      {
      /* a good window is caught by any kept event that overlaps it (a take already open when the note comes still has it);
         a bad or quiet window is only opened by an event that starts in it (a tail from before is not its fault) */
      if( w[i].cls == 'G' ) { if( t < w[i].e && te > w[i].s ) { inwin = 1; w[i].hit++; } }
      else if( in_win( w + i, t ) ) { inwin = 1; w[i].hit++; }
      }
    if( ! inwin ) sc.stray++;
    }
  for( int i = 0; i < nw; i++ )
    {
    if( w[i].cls == 'G' ) { sc.good++; if( ! w[i].hit ) sc.misses++; }
    else if( w[i].cls == 'B' || w[i].cls == 'Q' ) { sc.bad++; if( w[i].hit ) sc.fps++; }
    }
  sc.score = 1000.0 * sc.misses + 10.0 * sc.fps + sc.stray;
  return sc;
  }

static AspGateEvent evs[MAXE];

static Score run_one( const int32_t *pcm, int64_t frames, const AspGateParams *p, Win *w, int nw )
  {
  int ne = asp_gatesim( pcm, frames, p, evs, MAXE );
  return score_it( evs, ne, w, nw );
  }

static void params_from( AspGateParams *p, const char *text )
  {
  char v[100];
  asp_gateparams_default( p );
  if( ! asp_settings_get( text, "gate", v, sizeof v ) ) p->gate = atof( v );
  if( ! asp_settings_get( text, "ring", v, sizeof v ) ) p->ring = atof( v );
  if( ! asp_settings_get( text, "lowcut", v, sizeof v ) ) p->lowcut = atof( v );
  if( ! asp_settings_get( text, "strict", v, sizeof v ) ) p->strict = atoi( v );
  }
static void params_text( const AspGateParams *p, char *out, size_t n )
  { snprintf( out, n, "gate=%g;ring=%g;lowcut=%g;strict=%d", p->gate, p->ring, p->lowcut, p->strict ); }

static void print_eval( const int32_t *pcm, int64_t frames, const AspGateParams *p, Win *w, int nw )
  {
  char t[200];
  int ne = asp_gatesim( pcm, frames, p, evs, MAXE );
  Score sc = score_it( evs, ne, w, nw );
  params_text( p, t, sizeof t );
  printf( "%s\n  %d events kept; good caught %d of %d; false opens %d of %d quiet/bad windows; stray events %d\n", t, sc.events, sc.good - sc.misses, sc.good, sc.fps, sc.bad, sc.stray );
  for( int i = 0; i < nw; i++ )
    {
    int ok = w[i].cls == 'G' ? w[i].hit > 0 : ( w[i].cls == 'X' ? 1 : w[i].hit == 0 );
    printf( "  %c %6.1f-%6.1fs %-4s %s%s\n", w[i].cls, w[i].s, w[i].e, ok ? "ok" : "WRONG", w[i].label, w[i].hit ? " (opened)" : "" );
    }
  if( getenv( "A5TUNE_EVENTS" ) ) for( int k = 0; k < ne; k++ ) printf( "  event %6.2f-%6.2fs %s peak %.0f dBFS share %.2f\n", evs[k].start / RATE, evs[k].end / RATE, evs[k].kept ? "kept" : "bump", evs[k].peak_db, evs[k].share );
  for( int k = 0; k < ne; k++ )
    {
    double s = evs[k].start / RATE; int inw = 0;
    for( int i = 0; i < nw; i++ ) if( in_win( w + i, s ) ) inw = 1;
    if( ! inw ) printf( "  stray event at %6.1fs%s  peak %.0f dBFS%s\n", s, evs[k].kept ? "" : " (judged a bump, not kept)", evs[k].peak_db, evs[k].kept ? "" : "" );
    }
  }

/* --- the search ---------------------------------------------------------------------------------------- */
static const double G_[] = { 12, 18, 24, 30 }, R_[] = { 6, 7.5, 9, 12, 15, 18 };
static const double L_[] = { 30, 45, 60, 80, 100, 130, 160, 190, 230, 300 };
#define NG 4
#define NR 6
#define NL 10

static int cmd_tune( int argc, char **argv )
  {
  Win w[MAXW]; int nw = load_windows( argv[3], w ); int64_t frames;
  int32_t *pcm = load_capture( argv[2], &frames );
  double lowest = 0; AspGateParams base; asp_gateparams_default( &base );
  if( nw <= 0 || ! pcm ) { fprintf( stderr, "cannot read the capture or the windows\n" ); return 1; }
  for( int i = 4; i + 1 < argc; i += 2 )
    { if( ! strcmp( argv[i], "--lowest" ) ) lowest = atof( argv[i+1] ); else if( ! strcmp( argv[i], "--base" ) ) params_from( &base, argv[i+1] ); }
  double maxcut = lowest > 30 ? lowest * 0.97 : 30;                  /* nothing the instrument plays may be cut */
  typedef struct { AspGateParams p; Score s; } Cand;
  static Cand c[600];
  int nc = 0, best = 0;
  Score sb = run_one( pcm, frames, &base, w, nw );
  { char t[200]; params_text( &base, t, sizeof t );
    printf( "start: %s  -> good caught %d/%d, false opens %d/%d, stray %d\n", t, sb.good - sb.misses, sb.good, sb.fps, sb.bad, sb.stray ); }
  printf( "trying settings (lowcut up to %.0f Hz): ", maxcut ); fflush( stdout );
  /* stage 1: what the instrument's range lets us cut, with strict on and off, at the starting gate and ring */
  for( int l = 0; l < NL; l++ ) for( int s = 0; s < 2; s++ )
    {
    if( L_[l] > maxcut && L_[l] > 30 ) continue;
    Cand *k = c + nc++;
    k->p = base; k->p.lowcut = L_[l]; k->p.strict = s;
    k->s = run_one( pcm, frames, &k->p, w, nw );
    if( k->s.score < c[best].s.score ) best = nc - 1;
    }
  printf( "%d ", nc ); fflush( stdout );
  /* stage 2: the gate margin and the ring rule, around the three best of those */
  int firsts[3] = { -1, -1, -1 }, nf = 0, n1 = nc;
  for( int r = 0; r < 3; r++ )
    {
    int bi = -1;
    for( int i = 0; i < n1; i++ )
      { int used = 0; for( int j = 0; j < nf; j++ ) if( firsts[j] == i ) used = 1; if( ! used && ( bi < 0 || c[i].s.score < c[bi].s.score ) ) bi = i; }
    if( bi >= 0 ) firsts[nf++] = bi;
    }
  for( int j = 0; j < nf; j++ )
    {
    AspGateParams cur = c[firsts[j]].p; double curscore = c[firsts[j]].s.score;
    for( int pass = 0; pass < 3; pass++ )                    /* gate, ring, gate: one at a time, keeping what helps */
      {
      int n2 = pass == 1 ? NR : NG;
      for( int v = 0; v < n2; v++ )
        {
        Cand *k = c + nc++;
        k->p = cur; if( pass == 1 ) k->p.ring = R_[v]; else k->p.gate = G_[v];
        k->s = run_one( pcm, frames, &k->p, w, nw );
        if( k->s.score < c[best].s.score ) best = nc - 1;
        if( k->s.score < curscore ) { curscore = k->s.score; cur = k->p; }
        }
      printf( "." ); fflush( stdout );
      }
    }
  printf( "%d settings tried\n", nc );
  double top = c[best].s.score; int same = 0;
  double mg = 0, mr = 0, ml = 0, ms = 0;
  for( int i = 0; i < nc; i++ ) if( c[i].s.score == top ) { same++; mg += log( c[i].p.gate ); mr += c[i].p.ring; ml += log( c[i].p.lowcut ); ms += c[i].p.strict; }
  mg = exp( mg / same ); mr /= same; ml = exp( ml / same ); ms /= same;
  int pick = best; double bd = 1e9;
  for( int i = 0; i < nc; i++ ) if( c[i].s.score == top )
    {
    double d = fabs( log( c[i].p.gate / mg ) ) + fabs( c[i].p.ring - mr ) / 3 + fabs( log( c[i].p.lowcut / ml ) ) + fabs( c[i].p.strict - ms );
    if( d < bd ) { bd = d; pick = i; }
    }
  char t[200]; params_text( &c[pick].p, t, sizeof t );
  printf( "\n%d of %d settings score best (%s). I take the one in the middle of them:\n", same, nc, same > nc / 4 ? "a wide plateau: forgiving" : same > 3 ? "a narrow one" : "a single point: fragile, record more" );
  printf( "proposal: %s\n  -> good caught %d/%d, false opens %d/%d, stray %d\n", t, c[pick].s.good - c[pick].s.misses, c[pick].s.good, c[pick].s.fps, c[pick].s.bad, c[pick].s.stray );
  printf( "SETTINGS %s\n", t );
  free( pcm );
  return 0;
  }

/* --- the floor and the hum ------------------------------------------------------------------------------ */
static void fft( double *re, double *im, int n )
  {
  for( int i = 1, j = 0; i < n; i++ )
    {
    int bit = n >> 1;
    for( ; j & bit; bit >>= 1 ) j ^= bit;
    j ^= bit;
    if( i < j ) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
  for( int len = 2; len <= n; len <<= 1 )
    {
    double a = -2 * M_PI / len;
    for( int i = 0; i < n; i += len )
      for( int k = 0; k < len / 2; k++ )
        {
        double wr = cos( a * k ), wi = sin( a * k );
        double tr = re[i+k+len/2] * wr - im[i+k+len/2] * wi, ti = re[i+k+len/2] * wi + im[i+k+len/2] * wr;
        re[i+k+len/2] = re[i+k] - tr; im[i+k+len/2] = im[i+k] - ti; re[i+k] += tr; im[i+k] += ti;
        }
    }
  }

static int cmd_floor( char **argv )
  {
  Win w[MAXW]; int nw = load_windows( argv[3], w ); int64_t frames;
  int32_t *pcm = load_capture( argv[2], &frames );
  if( nw <= 0 || ! pcm ) { fprintf( stderr, "cannot read the capture or the windows\n" ); return 1; }
  const int N = 1 << 15;
  static double acc[2][1 << 14], re[1 << 15], im[1 << 15];
  int nseg[2] = { 0, 0 };
  double rms[2] = { 0, 0 }, dc[2] = { 0, 0 }; int64_t cnt = 0;
  for( int i = 0; i < nw; i++ )
    {
    if( w[i].cls != 'Q' ) continue;
    int64_t a = (int64_t)( ( w[i].s + 0.5 ) * RATE ), b = (int64_t)( w[i].e * RATE );
    if( b > frames ) b = frames;
    for( int64_t t = a; t < b; t++ ) for( int ch = 0; ch < 2; ch++ ) { double x = pcm[2*t+ch]; rms[ch] += x * x; dc[ch] += x; } 
    cnt += b > a ? b - a : 0;
    for( int ch = 0; ch < 2; ch++ )
      for( int64_t t = a; t + N <= b; t += N )
        {
        for( int k = 0; k < N; k++ ) { re[k] = pcm[2*( t + k ) + ch] * ( 0.5 - 0.5 * cos( 2 * M_PI * k / N ) ); im[k] = 0; }
        fft( re, im, N );
        for( int k = 0; k < N / 2; k++ ) acc[ch][k >> 1] += re[k] * re[k] + im[k] * im[k];     /* two bins a slot */
        nseg[ch]++;
        }
    }
  if( ! cnt ) { printf( "no quiet windows to measure\n" ); return 0; }
  for( int ch = 0; ch < 2; ch++ )
    {
    double r = sqrt( rms[ch] / cnt ) / 2147483648.0, d = dc[ch] / cnt / 2147483648.0;
    printf( "channel %d: noise floor %.1f dBFS rms, DC %.1f dBFS\n", ch + 1, 20 * log10( r + 1e-12 ), 20 * log10( fabs( d ) + 1e-12 ) );
    if( nseg[ch] < 1 ) continue;
    /* tonal lines below 2 kHz: a slot well above the median of its neighbours */
    double bin = RATE / N * 2;
    int found = 0;
    for( int k = 3; k < (int)( 2000 / bin ) && found < 8; k++ )
      {
      double v = acc[ch][k] / nseg[ch], nb[10]; int m = 0;
      for( int j = -6; j <= 6; j++ ) if( j < -1 || j > 1 ) nb[m++ % 10] = acc[ch][k+j] / nseg[ch];
      double mean = 0; for( int j = 0; j < 10; j++ ) mean += nb[j]; mean /= 10;
      if( v > mean * 20 && v >= acc[ch][k-1] / nseg[ch] && v >= acc[ch][k+1] / nseg[ch] )
        {
        double f = ( k + 0.5 ) * bin;
        double hum = fmod( f + 1.5, 50 ) < 3 || fmod( f + 1.5, 60 ) < 3;
        printf( "  line at %6.1f Hz, %.0f dB over its surroundings%s\\n", f, 10 * log10( v / mean ), hum ? "  (mains hum or a harmonic)" : "" );
        found++;
        }
      }
    if( ! found ) printf( "  no tonal lines (no hum) in the quiet windows\n" );
    }
  free( pcm );
  return 0;
  }

/* --- a made-up capture, for testing ---------------------------------------------------------------------- */
static unsigned rs = 1;
static double rnd( void ) { rs = rs * 1664525u + 1013904223u; return ( (int)( rs >> 8 ) & 0xffff ) / 32768.0 - 1; }

static int cmd_synth( int argc, char **argv )
  {
  Win w[MAXW]; int nw = load_script( argv[2], w, 3.0 );
  double hum = 0;
  for( int i = 5; i + 1 < argc; i += 2 ) { if( ! strcmp( argv[i], "--seed" ) ) rs = (unsigned)atoi( argv[i+1] ); else if( ! strcmp( argv[i], "--hum" ) ) hum = atof( argv[i+1] ); }
  if( nw <= 0 ) { fprintf( stderr, "cannot read the script\n" ); return 1; }
  double total = w[nw-1].e + 4;
  int64_t n = (int64_t)( total * RATE );
  double *x = calloc( (size_t)n, sizeof *x );
  const double FS = 2147483648.0, floor_ = FS * pow( 10, -95.0 / 20 );
  for( int64_t i = 0; i < n; i++ )
    {
    x[i] = rnd() * floor_;
    if( hum > 0 ) x[i] += FS * pow( 10, -72.0 / 20 ) * ( sin( 2 * M_PI * hum * i / RATE ) + 0.5 * sin( 2 * M_PI * 2 * hum * i / RATE ) + 0.3 * sin( 2 * M_PI * 3 * hum * i / RATE ) );
    }
  /* intermittent hiss bursts at random through the whole thing: the junk the gate must learn to ignore */
  for( int h = 0; h < (int)( total / 6 ); h++ )
    {
    int64_t at = (int64_t)( ( 1 + rnd() * 0.5 + 0.5 ) * total / 2 * RATE * rnd() * 0 ) ;
    at = (int64_t)( ( 0.5 + 0.5 * ( rnd() + 1 ) / 2 * ( total - 2 ) ) * RATE );
    int len = (int)( 0.06 * RATE );
    for( int k = 0; k < len && at + k < n; k++ ) x[at+k] += rnd() * floor_ * 5 * sin( M_PI * k / len );
    }
  for( int i = 0; i < nw; i++ )
    {
    int64_t at = (int64_t)( ( w[i].s + 0.8 ) * RATE );
    if( w[i].cls == 'G' )
      {
      int soft = strstr( w[i].label, "soft" ) != NULL, pluck = strstr( w[i].label, "pluck" ) != NULL;
      double amp = FS * pow( 10, ( soft ? -50.0 : -35.0 ) / 20 );
      int len = (int)( ( pluck ? 1.0 : 2.4 ) * RATE );
      for( int k = 0; k < len && at + k < n; k++ )
        {
        double env = pluck ? exp( -k / ( 0.25 * RATE ) ) * ( 1 - exp( -k / 80.0 ) ) : ( 0.5 - 0.5 * cos( M_PI * fmin( k / ( 0.08 * RATE ), 1 ) ) ) * ( k > len - 0.2 * RATE ? ( len - k ) / ( 0.2 * RATE ) : 1 );
        double t = k / RATE;
        x[at+k] += amp * env * ( sin( 2 * M_PI * 440 * t ) + 0.4 * sin( 2 * M_PI * 880 * t ) + 0.2 * sin( 2 * M_PI * 1320 * t ) );
        }
      }
    else if( w[i].cls == 'B' )
      {
      int len = (int)( 0.5 * RATE );
      for( int k = 0; k < len && at + k < n; k++ )
        {
        double t = k / RATE, env = exp( -t / 0.12 );
        x[at+k] += FS * pow( 10, -33.0 / 20 ) * env * sin( 2 * M_PI * 70 * t );                  /* the thump */
        if( k < 96 ) x[at+k] += FS * pow( 10, -62.0 / 20 ) * rnd() * ( 1 - k / 96.0 );          /* its edge */
        }
      }
    }
  FILE *f = fopen( argv[3], "wb" );
  for( int64_t i = 0; i < n; i++ )
    {
    int32_t v = (int32_t)fmax( -2147483647.0, fmin( 2147483647.0, x[i] ) );
    unsigned char b[8]; asp_st32( b, (uint32_t)v ); asp_st32( b + 4, (uint32_t)v );
    fwrite( b, 1, 8, f );
    }
  fclose( f );
  f = fopen( argv[4], "w" );
  for( int i = 0; i < nw; i++ ) fprintf( f, "%c %.2f %.2f %s\n", w[i].cls, w[i].s, w[i].e, w[i].label );
  fclose( f );
  free( x );
  return 0;
  }

static int cmd_windows( char **argv )
  {
  Win w[MAXW]; int nw = load_script( argv[2], w, 3.0 );
  if( nw <= 0 ) return 1;
  FILE *f = fopen( argv[3], "w" );
  for( int i = 0; i < nw; i++ ) fprintf( f, "%c %.2f %.2f %s\n", w[i].cls, w[i].s, w[i].e, w[i].label );
  fclose( f );
  printf( "%d %.1f\n", nw, w[nw-1].e + 2 );                      /* prompts, seconds to record */
  return 0;
  }

int main( int argc, char **argv )
  {
  if( argc >= 5 && ! strcmp( argv[1], "synth" ) ) return cmd_synth( argc, argv );
  if( argc >= 4 && ! strcmp( argv[1], "windows" ) ) return cmd_windows( argv );
  if( argc >= 4 && ! strcmp( argv[1], "tune" ) ) return cmd_tune( argc, argv );
  if( argc >= 4 && ! strcmp( argv[1], "floor" ) ) return cmd_floor( argv );
  if( argc >= 4 && ! strcmp( argv[1], "eval" ) )
    {
    Win w[MAXW]; int nw = load_windows( argv[3], w ); int64_t frames; int32_t *pcm = load_capture( argv[2], &frames );
    AspGateParams p; params_from( &p, argc > 4 ? argv[4] : "" );
    if( nw <= 0 || ! pcm ) { fprintf( stderr, "cannot read the capture or the windows\n" ); return 1; }
    print_eval( pcm, frames, &p, w, nw ); free( pcm ); return 0;
    }
  fprintf( stderr, "usage: a5tune synth|windows|eval|tune|floor ... (see the top of src/a5tune.c)\n" );
  return 2;
  }
