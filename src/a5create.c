/* a5create: make a session (a space). three ways to say how long its loop is:
     a5create DIR NAME --seconds 7.25                       seconds, with decimals
     a5create DIR NAME --sig 4/4 --bpm 120 --bars 8         music: the loop is that many bars; a click track is made too
     a5create DIR NAME --from FILE.wav|FILE.flac            an audio file becomes the loop (48 kHz, one or two channels)
   options: --click STYLE (again for more; with --seconds or --from there is no click unless asked), --no-click,
            --pan l|c|r, --level N, --profile NAME.  a click can always be added or changed later with a5click.
   styles: beats bars eighths triplets sixteenths offbeat. bpm counts quarter notes; 6/8 9/8 12/8 beat on dotted quarters. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "clicklib.h"
#include "aspengine.h"
#include "settings.h"
#include "plat.h"

static int usage( void )
  {
  fprintf( stderr, "usage: a5create DIR NAME (--seconds S | --sig N/D --bpm B --bars N | --from FILE) [--click STYLE]... [--no-click] [--pan l|c|r] [--level N] [--profile NAME]\n       styles: %s\n", asp_click_styles );
  return 2;
  }

int main( int argc, char **argv )
  {
  if( argc < 4 ) return usage();
  const char *dir = argv[1], *name = argv[2], *from = NULL, *profile = NULL, *pan = "c", *styles[8];
  double secs = 0; AspMeter m = { 0, 0, 0, 0 };
  int ns = 0, noclick = 0, level = 1, musical = 0;
  char sigs[16] = "", err[ASP_ERR], settings[1100], tp[1100];
  for( int i = 3; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--seconds" ) && i + 1 < argc ) secs = atof( argv[++i] );
    else if( ! strcmp( argv[i], "--sig" ) && i + 1 < argc ) snprintf( sigs, sizeof sigs, "%s", argv[++i] );
    else if( ! strcmp( argv[i], "--bpm" ) && i + 1 < argc ) m.bpm = atof( argv[++i] );
    else if( ! strcmp( argv[i], "--bars" ) && i + 1 < argc ) m.bars = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--from" ) && i + 1 < argc ) from = argv[++i];
    else if( ! strcmp( argv[i], "--click" ) && i + 1 < argc && ns < 8 ) styles[ns++] = argv[++i];
    else if( ! strcmp( argv[i], "--no-click" ) ) noclick = 1;
    else if( ! strcmp( argv[i], "--pan" ) && i + 1 < argc ) pan = argv[++i];
    else if( ! strcmp( argv[i], "--level" ) && i + 1 < argc ) level = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--profile" ) && i + 1 < argc ) profile = argv[++i];
    else if( i == 3 && argv[i][0] != '-' ) secs = atof( argv[i] );               /* a5create DIR NAME 7.25 */
    else { fprintf( stderr, "a5create: what is %s?\n", argv[i] ); return usage(); }
    }
  musical = *sigs && m.bpm > 0 && m.bars > 0;
  if( ( musical ? 1 : 0 ) + ( secs > 0 ? 1 : 0 ) + ( from ? 1 : 0 ) != 1 ) { fprintf( stderr, "a5create: say how long it is in one way: seconds, bars, or a file\n" ); return usage(); }
  if( *sigs && asp_meter_parse( sigs, &m ) ) { fprintf( stderr, "a5create: %s is not a time signature\n", sigs ); return 2; }

  int32_t *src = NULL; int64_t frames = 0; int sch = 0, srate = 0;
  if( from )
    {
    if( asp_audio_read( from, &src, &frames, &sch, &srate, err, sizeof err ) ) { fprintf( stderr, "a5create: %s\n", err ); return 1; }
    }
  else if( musical ) frames = (int64_t)llround( asp_meter_seconds( &m ) * 48000 );
  else frames = (int64_t)llround( secs * 48000 );
  if( frames < 4 * ASP_TICK ) { fprintf( stderr, "a5create: a loop needs at least %d frames (%.2f s)\n", 4 * ASP_TICK, 4 * ASP_TICK / 48000.0 ); return 1; }
  if( asp_build_settings( profile, settings, sizeof settings ) ) return 1;

  char desc[500];
  if( musical ) snprintf( desc, sizeof desc, "{\"v\":1,\"name\":\"%s\",\"loop\":%lld,\"rate\":48000,\"created\":%lld,\"sig\":\"%s\",\"bpm\":%.3f,\"bars\":%d}", name, (long long)frames, (long long)time( NULL ), sigs, m.bpm, m.bars );
  else snprintf( desc, sizeof desc, "{\"v\":1,\"name\":\"%s\",\"loop\":%lld,\"rate\":48000,\"created\":%lld%s}", name, (long long)frames, (long long)time( NULL ), from ? ",\"from\":\"file\"" : "" );
  AspStore *s = asp_create( dir, desc, err );
  if( ! s ) { fprintf( stderr, "a5create: %s\n", err ); return 1; }
  snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
  AspTank *t = asp_tank_create( tp, frames, 48000, err, sizeof err );
  if( ! t ) { fprintf( stderr, "a5create: %s\n", err ); return 1; }
  asp_tank_bind( t, asp_session_id( s ) );
  { char st[1300]; snprintf( st, sizeof st, "{\"profile\":\"%s\",\"settings\":\"%s\"}", profile ? profile : "", settings ); asp_take_set_state( s, 0, st, "a5create", 0 ); asp_log( s, "a5create", "settings", st, 0 ); }
  printf( "%s: %lld frames (%.3f s)", dir, (long long)frames, frames / 48000.0 );
  if( musical ) printf( ", %d bars of %s at %.2f bpm", m.bars, sigs, m.bpm );
  printf( "\n" );

  if( from )                                                       /* the file is the first take: the loop plays it */
    {
    char tmp[1300]; AspTake k; char st[400];
    asp_tmp_path( s, tmp, sizeof tmp );
    FILE *f = fopen( tmp, "wb" );
    for( int64_t i = 0; i < frames * sch; i++ ) { unsigned char b[4]; asp_st32( b, (uint32_t)src[i] ); fwrite( b, 1, 4, f ); }
    fclose( f );
    if( asp_take_add_d32( s, 0, 0, tmp, sch, 48000, "off", 0, &k ) ) { fprintf( stderr, "a5create: could not store the file\n" ); return 1; }
    remove( tmp );
    int64_t pk = 0; for( int64_t i = 0; i < frames * sch; i++ ) { int64_t v = src[i] < 0 ? -(int64_t)src[i] : src[i]; if( v > pk ) pk = v; }
    int lev = sch == 1 ? 1 : 0;
    snprintf( st, sizeof st, "{\"status\":\"mix\",\"lev\":[\"%d\",\"%d\"],\"flip\":0,\"mono\":%d,\"src\":0,\"len\":%lld,\"gone\":0,\"peak\":%lld,\"generated\":\"file\"}", lev, lev, sch == 1, (long long)frames, (long long)pk );
    asp_take_set_state( s, k.id, st, "a5create", 0 );
    asp_move_take( t, s, k.id, 0, -1, (AspPlace)ASP_PLACE_OFF, (AspPlace){ lev, lev, 0 } );
    printf( "  %s is the loop: %s, take %lld\n", from, sch == 1 ? "mono" : "stereo", (long long)k.id );
    free( src );
    }

  if( musical && ! noclick && ! ns ) styles[ns++] = "beats";
  if( noclick ) ns = 0;
  if( ns )
    {
    int ll = level, lr = level;
    if( ! strcmp( pan, "l" ) ) lr = ASP_LEV_OFF; else if( ! strcmp( pan, "r" ) ) ll = ASP_LEV_OFF;
    if( ! musical ) { fprintf( stderr, "a5create: a click needs --sig, --bpm and --bars (or add it afterwards with a5click)\n" ); return 2; }
    AspClickSounds sn; asp_click_sounds_default( &sn );
    AspClickEv *ev = malloc( sizeof *ev * 20000 );
    for( int k = 0; k < ns; k++ )
      {
      int ne = asp_click_events( &m, styles[k], frames, ev, 20000 );
      if( ne < 0 ) { fprintf( stderr, "a5create: no style \"%s\" (%s)\n", styles[k], asp_click_styles ); return 2; }
      int32_t *pcm = asp_click_render( ev, ne, frames, &sn );
      char meta[200]; snprintf( meta, sizeof meta, "\"sig\":\"%s\",\"bpm\":%.3f,\"bars\":%d,\"effective_bpm\":%.3f,\"clicks\":%d", sigs, m.bpm, m.bars, m.bpm, ne );
      int64_t id = asp_click_insert( s, t, 90001 + k, pcm, frames, styles[k], ll, lr, meta, 1 );
      free( pcm );
      if( id < 0 ) { fprintf( stderr, "a5create: could not make the %s click\n", styles[k] ); return 1; }
      printf( "  click %-10s %d clicks, take %lld\n", styles[k], ne, (long long)id );
      }
    free( ev ); asp_click_sounds_free( &sn );
    }
  asp_tank_close( t ); asp_close( s );
  return 0;
  }
