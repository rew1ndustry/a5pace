/* a5click: make click tracks (a metronome) for a session, fitted to its loop. it works whether or not the session is running.
     a5click DIR --sig 4/4 [--bpm 120] --bars 8 [--style beats]... [--pan l|c|r] [--level N] [--insert] [--clear] [--strict]
                  the click is only made (kept as a take, not in the loop) unless --insert puts it into the loop as well
                  without --bpm the tempo is worked out from the loop length: the bars fill the loop exactly
                  [--accent FILE] [--beat FILE] [--sub FILE]
   the bars are spread evenly over the loop whatever its length, so a loop that is not exactly that many bars gets
   the tempo that fits it (and it says so). each --style makes a take of its own, so a loop can carry several.
   styles: beats bars eighths triplets sixteenths offbeat. bpm counts quarter notes; in 6/8 9/8 12/8 a beat is a dotted quarter.
   --clear first takes every click this tool made out of the loop. --level N is a right shift (0 full, 1 -6 dB: the default). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include "ctlclient.h"
#include "clicklib.h"
#include "aspengine.h"

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
  AspMeter m = { 0, 0, 0, 0 };
  const char *dir = NULL, *styles[8], *files[3] = { NULL, NULL, NULL }, *pan = "c";
  int ns = 0, level = 1, clear = 0, strict = 0, insert = 0;
  char err[ASP_ERR], tp[1100], sigs[16] = "";
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--sig" ) && i + 1 < argc ) snprintf( sigs, sizeof sigs, "%s", argv[++i] );
    else if( ! strcmp( argv[i], "--bpm" ) && i + 1 < argc ) m.bpm = atof( argv[++i] );
    else if( ! strcmp( argv[i], "--bars" ) && i + 1 < argc ) m.bars = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--style" ) && i + 1 < argc && ns < 8 ) styles[ns++] = argv[++i];
    else if( ! strcmp( argv[i], "--pan" ) && i + 1 < argc ) pan = argv[++i];
    else if( ! strcmp( argv[i], "--level" ) && i + 1 < argc ) level = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--accent" ) && i + 1 < argc ) files[0] = argv[++i];
    else if( ! strcmp( argv[i], "--beat" ) && i + 1 < argc ) files[1] = argv[++i];
    else if( ! strcmp( argv[i], "--sub" ) && i + 1 < argc ) files[2] = argv[++i];
    else if( ! strcmp( argv[i], "--clear" ) ) clear = 1;
    else if( ! strcmp( argv[i], "--strict" ) ) strict = 1;
    else if( ! strcmp( argv[i], "--insert" ) ) insert = 1;
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { fprintf( stderr, "a5click: what is %s?\n", argv[i] ); return 2; }
    }
  if( ! dir || ( ! clear && ( ! *sigs || m.bars < 1 ) ) )
    { fprintf( stderr, "usage: a5click DIR --sig 4/4 [--bpm 120] --bars 8 [--style S]... [--pan l|c|r] [--level N] [--insert] [--clear] [--strict]\n       styles: %s\n", asp_click_styles ); return 2; }
  int guess = m.bpm <= 0; if( guess ) m.bpm = 60;                         /* the tempo is found below, from the loop */
  if( *sigs && asp_meter_parse( sigs, &m ) ) { fprintf( stderr, "a5click: %s is not a time signature (4/4, 3/4, 6/8, 7/8 ...)\n", sigs ); return 2; }
  int fd = running( dir ) ? ctlc_connect( dir ) : -1;                  /* a running space is told; a closed one is written to */
  AspStore *s = NULL; AspTank *t = NULL; int64_t loop = 0;
  if( fd >= 0 )
    {
    reply[0] = 0;
    if( ctlc_cmd( fd, "STATE", keep, NULL ) ) { fprintf( stderr, "a5click: %s is running but did not answer\n", dir ); return 1; }
    loop = json_num( reply, "loop_frames", 0 );
    if( clear )
      {
      reply[0] = 0;
      if( ctlc_cmd( fd, "CLICK CLEAR", keep, NULL ) ) { fprintf( stderr, "a5click: could not clear\n" ); return 1; }
      printf( "%s\n", reply );
      if( ! *sigs ) return 0;
      }
    }
  else
    {
    s = asp_open( dir, 0, err );
    if( ! s ) { fprintf( stderr, "a5click: %s\n", err ); return 1; }
    loop = json_num( asp_descriptor( s ), "loop", 0 );
    snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
    t = asp_tank_open( tp, err, sizeof err );
    if( t && ( asp_tank_dirty( t ) || asp_tank_frames( t ) != loop ) ) { asp_tank_close( t ); t = NULL; }     /* it will be rebuilt at the next start */
    if( clear )
      {
      int n = asp_click_clear( s, t );
      printf( "%d click track(s) taken out of the loop\n", n < 0 ? 0 : n );
      if( ! *sigs ) { if( t ) asp_tank_close( t ); asp_close( s ); return n < 0; }
      }
    }
  if( ! loop ) { fprintf( stderr, "a5click: no loop length in the session\n" ); return 1; }
  if( ! ns ) styles[ns++] = "beats";

  double have = loop / 48000.0, eff = asp_meter_quarters( &m ) * 60.0 / have;
  if( guess ) { m.bpm = eff; printf( "no --bpm: %d bars of %s in %.3f s is %.2f bpm\n", m.bars, sigs, have, eff ); }
  double want = asp_meter_seconds( &m );
  printf( "%s: %d bars of %s, loop %.3f s; at %.2f bpm the bars would last %.3f s\n", dir, m.bars, sigs, have, m.bpm, want );
  if( fabs( eff / m.bpm - 1 ) > 0.002 )
    {
    if( strict ) { fprintf( stderr, "a5click: the loop does not fit that many bars at that tempo (--strict)\n" ); return 1; }
    printf( "  they do not fit: the clicks are spread over the loop, which makes the tempo %.2f bpm\n", eff );
    }
  AspClickSounds sn; asp_click_sounds_default( &sn );
  for( int i = 0; i < 3; i++ ) if( files[i] && asp_click_sound_file( &sn, i, files[i], err, sizeof err ) ) { fprintf( stderr, "a5click: %s\n", err ); return 1; }
  int ll = level, lr = level;
  if( ! strcmp( pan, "l" ) ) lr = ASP_LEV_OFF; else if( ! strcmp( pan, "r" ) ) ll = ASP_LEV_OFF; else if( strcmp( pan, "c" ) ) { fprintf( stderr, "a5click: --pan l, c or r\n" ); return 2; }
  AspClickEv *ev = malloc( sizeof *ev * 20000 );
  for( int k = 0; k < ns; k++ )
    {
    int64_t iter = -1, id;
    int ne = asp_click_events( &m, styles[k], loop, ev, 20000 );
    if( ne < 0 ) { fprintf( stderr, "a5click: no style \"%s\" (%s)\n", styles[k], asp_click_styles ); return 2; }
    int32_t *pcm = asp_click_render( ev, ne, loop, &sn );
    if( fd >= 0 )
      {
      char name[80], line[400]; long long tid = -1, it = 0;
      if( asp_click_write_part( dir, pcm, loop, name, sizeof name ) ) { fprintf( stderr, "a5click: could not write the click\n" ); free( pcm ); return 1; }
      snprintf( line, sizeof line, "CLICK ADD 0 %s %s %d %d %s", name, styles[k], ll, lr, insert ? "IN" : "KEEP" );
      reply[0] = 0;
      if( ctlc_cmd( fd, line, keep, NULL ) || sscanf( reply, "%lld %lld", &tid, &it ) != 2 )
        { fprintf( stderr, "a5click: the running space did not take the %s click\n", styles[k] ); free( pcm ); return 1; }
      id = tid; iter = it;
      }
    else
      {
      AspTake tk;
      while( ! asp_take_find( s, iter, 0, &tk ) ) iter--;                  /* generated takes have negative iterations */
      char meta[200]; snprintf( meta, sizeof meta, "\"sig\":\"%s\",\"bpm\":%.3f,\"bars\":%d,\"effective_bpm\":%.3f,\"clicks\":%d", sigs, m.bpm, m.bars, eff, ne );
      id = asp_click_insert( s, t, iter, pcm, loop, styles[k], ll, lr, meta, insert );
      }
    free( pcm );
    if( id < 0 ) { fprintf( stderr, "a5click: could not make %s\n", styles[k] ); return 1; }
    printf( "  %-10s %d clicks, take %lld (iteration %lld), %s%s\n", styles[k], ne, (long long)id, (long long)iter, insert ? "in the loop" : "kept, not in the loop (--insert to play it)",
            fd >= 0 || t || ! insert ? "" : " (the loop will be rebuilt at the next start)" );
    }
  free( ev ); asp_click_sounds_free( &sn );
  if( t ) asp_tank_close( t );
  if( s ) asp_close( s );
  return 0;
  }
