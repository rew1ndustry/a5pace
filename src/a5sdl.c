/* a5sdl: the portable audio edge. aspace on any sound card SDL2 can open (ALSA, Pulse, PipeWire, CoreAudio, WASAPI, ...),
   the same job a5card does with arecord and aplay, in one program, so it is not tied to Linux.

     a5sdl DIR [--in NAME] [--out NAME] [--frames N] [--soft-loop] [--list] [a5pace run options]

   it runs `a5pace run -C 0 DIR ...` as a child and feeds it: the capture device's samples go to the child's stdin, its
   stdout goes to the playback device. raw little endian s32 stereo 48 kHz, ticks of 9600 frames (docs/EDGE.md).
   the capture callback is the clock: the child answers a tick for a tick. SDL2 cannot open one duplex device, so the two
   devices have two clocks and may drift: a drop (capture ring full) or a starve (playback ring empty) is counted and
   told to aspace (EDGE drop|starve), as docs/EDGE.md says; it does not resample.
   the sync hunt needs the output to come back into the input. --soft-loop does that in software, with no hardware (it
   plays nothing to the card and hears what aspace sent): for a trial without a card, and for the test. otherwise
   the loopback is the user's (a cable, or the driver), as for a5card.
   --list shows the devices SDL sees. SDL_AUDIODRIVER=dummy runs with no device at all.
   not tested on macOS or Windows (no machine here): SDL2 makes it likely to build, nothing more is claimed. */
#include <SDL.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "ctlclient.h"

#define FRAME   8                       /* two s32 */
#define TICK    9600                    /* frames */
#define RING    ( TICK * FRAME * 8 )    /* bytes: eight ticks, a stall of the child must not lose lock for long */

typedef struct { unsigned char b[RING]; int r, w, n; pthread_mutex_t m; pthread_cond_t c; int closed; } Ring;

static Ring inr, outr;
static volatile long drops, starves;    /* frames, counted in callbacks, told to aspace by main */
static volatile int quit, softloop, flowing;   /* flowing: the first output has arrived, before it nothing is a starve */
static int cin = -1, cout = -1;         /* pipes to the child */

static void ring_init( Ring *q ) { memset( q, 0, sizeof *q ); pthread_mutex_init( &q->m, 0 ); pthread_cond_init( &q->c, 0 ); }

/* put up to n bytes, never blocking; returns the bytes taken */
static int ring_put( Ring *q, const unsigned char *p, int n )
  {
  pthread_mutex_lock( &q->m );
  int room = RING - q->n, k = n < room ? n : room;
  for( int i = 0; i < k; i++ ) { q->b[q->w] = p[i]; q->w = ( q->w + 1 ) % RING; }
  q->n += k;
  pthread_cond_signal( &q->c );
  pthread_mutex_unlock( &q->m );
  return k;
  }

/* take up to n bytes, never blocking */
static int ring_get( Ring *q, unsigned char *p, int n )
  {
  pthread_mutex_lock( &q->m );
  int k = n < q->n ? n : q->n;
  for( int i = 0; i < k; i++ ) { p[i] = q->b[q->r]; q->r = ( q->r + 1 ) % RING; }
  q->n -= k;
  pthread_mutex_unlock( &q->m );
  return k;
  }

/* take at least 1 and up to n bytes, waiting; 0 when closed and empty */
static int ring_wait( Ring *q, unsigned char *p, int n )
  {
  pthread_mutex_lock( &q->m );
  while( q->n == 0 && ! q->closed && ! quit ) pthread_cond_wait( &q->c, &q->m );
  pthread_mutex_unlock( &q->m );
  return ring_get( q, p, n );
  }

static void on_capture( void *u, Uint8 *s, int len )
  {
  (void)u;
  if( softloop ) return;                /* the input is what was played: see the output thread */
  int k = ring_put( &inr, s, len );
  if( k < len ) drops += ( len - k ) / FRAME;
  }

static void on_play( void *u, Uint8 *s, int len )
  {
  (void)u;
  int k = ring_get( &outr, s, len );
  if( k < len ) { memset( s + k, 0, len - k ); if( flowing && ! outr.closed ) starves += ( len - k ) / FRAME; }
  }

/* child's stdout to the playback ring (or, soft loop, straight to the input ring, the way a cable would) */
static void *reader( void *u )
  {
  (void)u;
  unsigned char buf[8192];
  for( ;; )
    {
    ssize_t n = read( cout, buf, sizeof buf );
    if( n <= 0 ) break;
    if( softloop ) { int k = ring_put( &inr, buf, (int)n ); if( k < n ) drops += ( n - k ) / FRAME; }
    else { ring_put( &outr, buf, (int)n ); flowing = 1; }
    }
  pthread_mutex_lock( &outr.m ); outr.closed = 1; pthread_cond_broadcast( &outr.c ); pthread_mutex_unlock( &outr.m );
  quit = 1;
  pthread_mutex_lock( &inr.m ); pthread_cond_broadcast( &inr.c ); pthread_mutex_unlock( &inr.m );
  return 0;
  }

/* the input ring to the child's stdin */
static void *writer( void *u )
  {
  (void)u;
  unsigned char buf[8192];
  while( ! quit )
    {
    int n = ring_wait( &inr, buf, sizeof buf );
    if( n <= 0 ) continue;
    for( int off = 0; off < n; )
      {
      ssize_t w = write( cin, buf + off, n - off );
      if( w <= 0 ) { quit = 1; return 0; }
      off += (int)w;
      }
    }
  return 0;
  }

static void on_signal( int s ) { (void)s; quit = 1; }

static void tell( const char *dir, const char *what, long frames )
  {
  int fd = ctlc_connect( dir );
  if( fd < 0 ) return;
  char l[64]; snprintf( l, sizeof l, "EDGE %s %ld", what, frames );
  ctlc_cmd( fd, l, 0, 0 );
  close( fd );
  }

static void list_devices( void )
  {
  for( int cap = 0; cap < 2; cap++ )
    {
    int n = SDL_GetNumAudioDevices( cap );
    printf( "%s devices:\n", cap ? "capture" : "playback" );
    for( int i = 0; i < n; i++ ) printf( "  %s\n", SDL_GetAudioDeviceName( i, cap ) );
    }
  }

static SDL_AudioDeviceID open_dev( const char *name, int cap, int frames, SDL_AudioCallback cb )
  {
  SDL_AudioSpec want, got;
  SDL_zero( want );
  want.freq = 48000; want.format = AUDIO_S32LSB; want.channels = 2; want.samples = (Uint16)frames; want.callback = cb;
  SDL_AudioDeviceID d = SDL_OpenAudioDevice( name, cap, &want, &got, 0 );   /* 0: SDL must not change the format; it converts */
  if( d == 0 ) { fprintf( stderr, "a5sdl: cannot open the %s device%s%s: %s\n", cap ? "capture" : "playback", name ? " " : "", name ? name : "", SDL_GetError() ); return 0; }
  if( got.freq != 48000 || got.channels != 2 || got.format != AUDIO_S32LSB )
    { fprintf( stderr, "a5sdl: the %s device would not give 48 kHz stereo s32\n", cap ? "capture" : "playback" ); SDL_CloseAudioDevice( d ); return 0; }
  return d;
  }

int main( int argc, char **argv )
  {
  const char *dir = 0, *inname = 0, *outname = 0;
  int frames = 1024, nx = 0, list = 0;
  char *xargs[64];
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--in" ) && i + 1 < argc ) inname = argv[++i];
    else if( ! strcmp( argv[i], "--out" ) && i + 1 < argc ) outname = argv[++i];
    else if( ! strcmp( argv[i], "--frames" ) && i + 1 < argc ) frames = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--soft-loop" ) ) softloop = 1;
    else if( ! strcmp( argv[i], "--list" ) ) list = 1;
    else if( ! dir && argv[i][0] != '-' ) dir = argv[i];
    else if( nx < 60 ) xargs[nx++] = argv[i];
    }
  if( ! list && ! dir ) { fprintf( stderr, "usage: a5sdl DIR [--in NAME] [--out NAME] [--frames N] [--soft-loop] [--list] [a5pace run options]\n" ); return 2; }
  if( SDL_Init( SDL_INIT_AUDIO ) != 0 ) { fprintf( stderr, "a5sdl: SDL: %s\n", SDL_GetError() ); return 1; }
  if( list ) { list_devices(); SDL_Quit(); return 0; }

  ring_init( &inr ); ring_init( &outr );
  SDL_AudioDeviceID dout = 0, din = 0;
  if( ! softloop )
    {
    dout = open_dev( outname, 0, frames, on_play );
    din  = dout ? open_dev( inname, 1, frames, on_capture ) : 0;
    if( ! din ) { if( dout ) SDL_CloseAudioDevice( dout ); SDL_Quit(); return 1; }
    }

  /* the child: a5pace beside us, else on the path */
  char self[1024] = "", bin[1100];
  ssize_t sl = readlink( "/proc/self/exe", self, sizeof self - 1 );
  if( sl > 0 ) { self[sl] = 0; char *s = strrchr( self, '/' ); if( s ) *s = 0; else self[0] = 0; }
  snprintf( bin, sizeof bin, "%s/a5pace", self );
  if( ! self[0] || access( bin, X_OK ) ) snprintf( bin, sizeof bin, "a5pace" );
  int p_in[2], p_out[2];
  if( pipe( p_in ) || pipe( p_out ) ) { perror( "a5sdl: pipe" ); return 1; }
  pid_t kid = fork();
  if( kid < 0 ) { perror( "a5sdl: fork" ); return 1; }
  if( kid == 0 )
    {
    dup2( p_in[0], 0 ); dup2( p_out[1], 1 );
    close( p_in[0] ); close( p_in[1] ); close( p_out[0] ); close( p_out[1] );
    char *av[80]; int k = 0;
    av[k++] = bin; av[k++] = "run"; av[k++] = "-C"; av[k++] = "0";
    for( int i = 0; i < nx; i++ ) av[k++] = xargs[i];
    av[k++] = (char *)dir; av[k] = 0;
    execvp( bin, av ); perror( "a5sdl: a5pace" ); _exit( 127 );
    }
  close( p_in[0] ); close( p_out[1] );
  cin = p_in[1]; cout = p_out[0];
  signal( SIGPIPE, SIG_IGN );
  signal( SIGINT, on_signal ); signal( SIGTERM, on_signal );

  pthread_t tr, tw;
  pthread_create( &tr, 0, reader, 0 ); pthread_create( &tw, 0, writer, 0 );
  if( softloop )
    {                                   /* a cable has one tick of the past in it: aspace answers a tick for a tick, so it must be started */
    static unsigned char zero[TICK * FRAME];
    ring_put( &inr, zero, sizeof zero );
    }
  if( softloop ) fprintf( stderr, "a5sdl: soft loop: aspace hears itself, no card is opened\n" );
  else { SDL_PauseAudioDevice( din, 0 ); SDL_PauseAudioDevice( dout, 0 ); fprintf( stderr, "a5sdl: running on SDL audio (%s); watch it with: a5hell %s\n", SDL_GetCurrentAudioDriver(), dir ); }

  long told_d = 0, told_s = 0;
  int status = 0, done = 0;
  while( ! done )
    {
    usleep( 200000 );
    long d = drops, s = starves;
    if( d != told_d ) { tell( dir, "drop", d - told_d ); fprintf( stderr, "a5sdl: dropped %ld frames of input\n", d - told_d ); told_d = d; }
    if( s != told_s ) { tell( dir, "starve", s - told_s ); fprintf( stderr, "a5sdl: starved %ld frames of output\n", s - told_s ); told_s = s; }
    if( waitpid( kid, &status, WNOHANG ) == kid ) { kid = 0; done = 1; }
    else if( quit && ! outr.closed )
      {                                  /* a signal: ask aspace to drain and close, the way a5card does */
      int fd = ctlc_connect( dir );
      if( fd >= 0 ) { ctlc_cmd( fd, "QUIT", 0, 0 ); close( fd ); }
      for( int i = 0; i < 100 && ! done; i++ ) { usleep( 100000 ); if( waitpid( kid, &status, WNOHANG ) == kid ) { kid = 0; done = 1; } }
      if( ! done ) { kill( kid, SIGTERM ); waitpid( kid, &status, 0 ); kid = 0; done = 1; }
      }
    else if( quit && outr.closed ) { waitpid( kid, &status, 0 ); kid = 0; done = 1; }
    }
  quit = 1;
  pthread_mutex_lock( &inr.m ); pthread_cond_broadcast( &inr.c ); pthread_mutex_unlock( &inr.m );
  close( cin );
  pthread_join( tw, 0 ); pthread_join( tr, 0 );
  if( din ) SDL_CloseAudioDevice( din );
  if( dout ) SDL_CloseAudioDevice( dout );
  SDL_Quit();
  fprintf( stderr, "a5sdl: done\n" );
  return WIFEXITED( status ) ? WEXITSTATUS( status ) : 1;
  }
