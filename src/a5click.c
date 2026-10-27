/* a5click: put click tracks (a metronome) into a session that is not running, fitted to its loop.
     a5click DIR --sig 4/4 --bpm 120 --bars 8 [--style beats]... [--pan l|c|r] [--level N] [--clear] [--strict]
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

int main( int argc, char **argv )
  {
  AspMeter m = { 0, 0, 0, 0 };
  const char *dir = NULL, *styles[8], *files[3] = { NULL, NULL, NULL }, *pan = "c";
  int ns = 0, level = 1, clear = 0, strict = 0;
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
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { fprintf( stderr, "a5click: what is %s?\n", argv[i] ); return 2; }
    }
  if( ! dir || ( ! clear && ( ! *sigs || m.bpm <= 0 || m.bars < 1 ) ) )
    { fprintf( stderr, "usage: a5click DIR --sig 4/4 --bpm 120 --bars 8 [--style S]... [--pan l|c|r] [--level N] [--clear] [--strict]\n       styles: %s\n", asp_click_styles ); return 2; }
  if( *sigs && asp_meter_parse( sigs, &m ) ) { fprintf( stderr, "a5click: %s is not a time signature (4/4, 3/4, 6/8, 7/8 ...)\n", sigs ); return 2; }
  if( running( dir ) ) { fprintf( stderr, "a5click: %s is running; close the session first\n", dir ); return 1; }
  AspStore *s = asp_open( dir, 0, err );
  if( ! s ) { fprintf( stderr, "a5click: %s\n", err ); return 1; }
  int64_t loop = json_num( asp_descriptor( s ), "loop", 0 );
  snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
  AspTank *t = asp_tank_open( tp, err, sizeof err );
  if( t && ( asp_tank_dirty( t ) || asp_tank_frames( t ) != loop ) ) { asp_tank_close( t ); t = NULL; }     /* it will be rebuilt at the next start */
  if( clear )
    {
    int n = asp_click_clear( s, t );
    printf( "%d click track(s) taken out of the loop\n", n < 0 ? 0 : n );
    if( ! *sigs ) { if( t ) asp_tank_close( t ); asp_close( s ); return n < 0; }
    }
  if( ! loop ) { fprintf( stderr, "a5click: no loop length in the session\n" ); return 1; }
  if( ! ns ) styles[ns++] = "beats";

  double want = asp_meter_seconds( &m ), have = loop / 48000.0, eff = asp_meter_quarters( &m ) * 60.0 / have;
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
    int64_t iter = -1;
    AspTake tk;
    while( ! asp_take_find( s, iter, 0, &tk ) ) iter--;                  /* generated takes have negative iterations */
    int ne = asp_click_events( &m, styles[k], loop, ev, 20000 );
    if( ne < 0 ) { fprintf( stderr, "a5click: no style \"%s\" (%s)\n", styles[k], asp_click_styles ); return 2; }
    int32_t *pcm = asp_click_render( ev, ne, loop, &sn );
    char meta[200]; snprintf( meta, sizeof meta, "\"sig\":\"%s\",\"bpm\":%.3f,\"bars\":%d,\"effective_bpm\":%.3f,\"clicks\":%d", sigs, m.bpm, m.bars, eff, ne );
    int64_t id = asp_click_insert( s, t, iter, pcm, loop, styles[k], ll, lr, meta );
    free( pcm );
    if( id < 0 ) { fprintf( stderr, "a5click: could not insert %s\n", styles[k] ); return 1; }
    printf( "  %-10s %d clicks, take %lld (iteration %lld)%s\n", styles[k], ne, (long long)id, (long long)iter, t ? "" : " (the loop will be rebuilt at the next start)" );
    }
  free( ev ); asp_click_sounds_free( &sn );
  if( t ) asp_tank_close( t );
  asp_close( s );
  return 0;
  }
