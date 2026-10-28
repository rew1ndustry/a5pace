/* a5import: put an audio file into a session as a take. whether or not the session is running.
     a5import DIR FILE [--play] [--level N] [--pan l|c|r]
   FILE is wav or flac at 48 kHz, one or two channels. it becomes a generated track in the next free place of the 90000
   series (90001, 90002 ...), starting at the loop's start. it is kept, muted: --play puts it into the loop as well, at --level (a right shift, 6 dB a step: the
   default is 1 for a mono track, which sits centred at 0 dB, and 0 for a stereo one). --mute keeps it as a take that is not in the loop. a running session is told (IMPORT) and the track joins
   the loop behind the play head like any other change; a closed one is written to. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "clicklib.h"
#include "aspengine.h"
#include "ctlclient.h"

static int64_t json_num( const char *j, const char *key, int64_t d )
  {
  char k[64]; snprintf( k, sizeof k, "\"%s\":", key );
  const char *p = strstr( j, k );
  return p ? atoll( p + strlen( k ) ) : d;
  }

static int running( const char *dir )
  {
  char p[1200]; int port, pid;
  snprintf( p, sizeof p, "%s/ctl", dir );
  FILE *f = fopen( p, "r" );
  if( ! f ) return 0;
  int ok = fscanf( f, "%d %d", &port, &pid ) == 2;
  fclose( f );
  return ok && kill( pid, 0 ) == 0;
  }

static char reply[400];
static void keep( const char *l, void *u ) { (void)u; if( ! reply[0] ) snprintf( reply, sizeof reply, "%s", l ); }

int main( int argc, char **argv )
  {
  const char *dir = NULL, *file = NULL, *pan = "c";
  int level = -99, in = 0;
  char err[ASP_ERR], tp[1100];
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--play" ) ) in = 1;
    else if( ! strcmp( argv[i], "--mute" ) ) in = 0;                  /* (the default) */
    else if( ! strcmp( argv[i], "--level" ) && i + 1 < argc ) level = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--pan" ) && i + 1 < argc ) pan = argv[++i];
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else if( argv[i][0] != '-' && ! file ) file = argv[i];
    else { fprintf( stderr, "a5import: what is %s?\n", argv[i] ); return 2; }
    }
  if( ! dir || ! file ) { fprintf( stderr, "usage: a5import DIR FILE.flac|FILE.wav [--play] [--level N] [--pan l|c|r]\n" ); return 2; }
  int ll, lr;
  AspHdr h; int64_t told; int ch = 0;
  { AspAudio *a = asp_audio_open( file, &ch, &told, err, sizeof err );        /* what it is, before any of it is read */
    if( ! a ) { fprintf( stderr, "a5import: %s\n", err ); return 1; }
    asp_audio_close( a ); }
  if( level == -99 ) level = ch == 1 ? 1 : 0;                      /* a mono track is shared between the two outputs: one step down is 0 dB centred */
  ll = lr = level;
  if( ! strcmp( pan, "l" ) ) lr = ASP_LEV_OFF; else if( ! strcmp( pan, "r" ) ) ll = ASP_LEV_OFF; else if( strcmp( pan, "c" ) ) { fprintf( stderr, "a5import: --pan l, c or r\n" ); return 2; }
  int64_t id = -1, iter = 0;
  int fd = running( dir ) ? ctlc_connect( dir ) : -1;
  if( fd >= 0 )
    {
    char name[80], line[400]; long long tid = -1, it = 0;
    if( asp_audio_import_part( dir, file, name, sizeof name, &h, err, sizeof err ) ) { fprintf( stderr, "a5import: %s\n", err ); return 1; }
    snprintf( line, sizeof line, "IMPORT %s %d %d %s", name, ll, lr, in ? "IN" : "KEEP" );
    reply[0] = 0;
    if( ctlc_cmd( fd, line, keep, NULL ) || sscanf( reply, "%lld %lld", &tid, &it ) != 2 )
      { fprintf( stderr, "a5import: the running space did not take it (%s)\n", reply ); return 1; }
    id = tid; iter = it;
    }
  else
    {
    AspStore *s = asp_open( dir, 0, err );
    if( ! s ) { fprintf( stderr, "a5import: %s\n", err ); return 1; }
    int64_t loop = json_num( asp_descriptor( s ), "loop", 0 );
    snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
    AspTank *t = asp_tank_open( tp, err, sizeof err );
    if( t && ( asp_tank_dirty( t ) || asp_tank_frames( t ) != loop ) ) { asp_tank_close( t ); t = NULL; }     /* it will be rebuilt at the next start */
    iter = asp_gen_next_iter( s );
    char part[1300];
    if( asp_tmp_path( s, part, sizeof part ) || asp_audio_to_take( file, part, &h, err, sizeof err ) ) { fprintf( stderr, "a5import: %s\n", err ); return 1; }
    id = asp_import_adopt( s, t, iter, part, ll, lr, in );
    if( t ) asp_tank_close( t );
    asp_close( s );
    if( id >= 0 && in && ! t ) printf( "(the loop will be rebuilt at the next start)\n" );
    }
  if( id < 0 ) { fprintf( stderr, "a5import: could not import %s\n", file ); return 1; }
  printf( "%s: %.2f s, %s, take %lld, iteration %lld, %s\n", file, h.frames / 48000.0, ch == 1 ? "mono" : "stereo", (long long)id, (long long)iter,
          in ? "in the loop" : "kept, not in the loop" );
  return 0;
  }
