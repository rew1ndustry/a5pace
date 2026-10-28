/* a5shell DIR: a terminal shell for a running aspace. a status line, what has just happened in words, and a few keys;
   press ? for all of them. if the space is not running it is started first (a5start; --no-start to not).
     space  safe / live     h  history: places to return to     w  wipe the loop     a  mode: al ak a0 a1-9 ap
     r  sync again   l  relearn the floor   g/G  gate   p  profile   t  takes   ?  help   q  leave   Q  stop aspace
   nothing here is needed to run aspace: it is only a window on the control port. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <sys/socket.h>
#include "ctlclient.h"
#include "a5mixer.h"

static struct termios saved;
static void restore( void ) { tcsetattr( 0, TCSANOW, &saved ); }

static char status[400], events[8][220], space[100]; static int nev;
static void keep_state( const char *l, void *u ) { snprintf( (char *)u, 1500, "%s", l ); }
static void keep( const char *l, void *u ) { (void)u; snprintf( status, sizeof status, "%s", l ); }
static void show( const char *l, void *u ) { (void)u; printf( "\r\033[K%s\n", l ); }
static void quiet( const char *l, void *u ) { (void)u; if( ! strncmp( l, "ERR", 3 ) ) snprintf( events[nev++ % 8], 220, "%.200s", l ); }

/* the middle of the screen: the log as it happens, in words, and only what matters */
static double jfld( const char *j, const char *key, double d ) { char q[48]; snprintf( q, sizeof q, "\"%s\":", key ); const char *p = strstr( j, q ); return p ? atof( p + strlen( q ) ) : d; }
static int pretty( const char *line, char *out, size_t n )
  {
  const char *p = line, *type, *json; char t[40];
  if( *p != '@' ) { snprintf( out, n, "%s", line ); return 1; }
  p = strchr( p, ' ' ); if( ! p ) return 0; type = ++p;
  json = strchr( type, ' ' ); if( ! json ) json = ""; 
  snprintf( t, sizeof t, "%.*s", (int)( json - type > 39 ? 39 : json - type ), type );
  if( ! strcmp( t, "end" ) ) { const char *pk = strstr( json, "\"peak_db\":[" ); double a = pk ? atof( pk + 11 ) : 0, b = pk && strchr( pk + 11, ',' ) ? atof( strchr( pk + 11, ',' ) + 1 ) : a;
    snprintf( out, n, "recorded: iteration %d at %ds, %.1f s long, peak %.0f dB%s", (int)jfld( json, "iter", 0 ), (int)jfld( json, "off", 0 ), jfld( json, "len", 0 ) / 48000.0, a > b ? a : b, jfld( json, "mono", 0 ) > 0 ? ", mono" : "" ); }
  else if( ! strcmp( t, "discarded" ) ) snprintf( out, n, "ignored a %s (%.1f s)", strstr( json, "\"bump\"" ) ? "bump" : "sound too quiet to keep", jfld( json, "len", 0 ) / 48000.0 );
  else if( ! strcmp( t, "wipe" ) ) snprintf( out, n, "wiped %d take(s) out of the loop", (int)jfld( json, "takes", 0 ) );
  else if( ! strcmp( t, "reverted" ) ) snprintf( out, n, "went back to log point %d: %d take(s) in the loop", (int)jfld( json, "to", 0 ), (int)jfld( json, "takes", 0 ) );
  else if( ! strcmp( t, "sync_found" ) ) snprintf( out, n, "synced: round trip %.0f ms", jfld( json, "frames", 0 ) / 48.0 );
  else if( ! strcmp( t, "sync_start" ) || ! strcmp( t, "resync" ) ) snprintf( out, n, "syncing..." );
  else if( ! strcmp( t, "sync_failed" ) ) snprintf( out, n, "could not sync: check the loopback and the levels" );
  else if( ! strcmp( t, "safe_on" ) ) snprintf( out, n, "safe: nothing is being recorded" );
  else if( ! strcmp( t, "safe_off" ) ) snprintf( out, n, "live: recording" );
  else if( ! strcmp( t, "settings" ) ) { const char *pf = strstr( json, "\"profile\":\"" ); snprintf( out, n, "profile %.*s", pf ? (int)strcspn( pf + 11, "\"" ) : 0, pf ? pf + 11 : "" ); }
  else if( ! strcmp( t, "automix" ) ) snprintf( out, n, "mode: %s", jfld( json, "mode", 0 ) == 1 ? "overlap" : jfld( json, "mode", 0 ) == 2 ? "swap" : jfld( json, "mode", 0 ) == 3 ? "keep (recorded, not put into the loop)" : "loop" );
  else if( ! strcmp( t, "gate" ) ) { if( strstr( json, "learnt" ) ) snprintf( out, n, "the gate has learnt the noise floor" ); else return 0; }
  else if( ! strcmp( t, "edge_drop" ) || ! strcmp( t, "edge_starve" ) ) snprintf( out, n, "the sound card dropped samples: syncing again" );
  else if( ! strcmp( t, "error" ) ) snprintf( out, n, "error: %.200s", json );
  else if( ! strcmp( t, "click_added" ) ) snprintf( out, n, "click track added" );
  else return 0;                                                  /* wrap, top, new, relevel ...: not for this screen */
  return 1;
  }

static void draw( void )
  {
  int n = nev < 8 ? nev : 8;
  printf( "\033[H\033[J a5shell: %s     [space] safe/live  [h] history  [w] wipe  [t] mix  [a] mode  [?] help  [q] quit\n\n", space[0] ? space : "?" );
  for( int i = n; i > 0; i-- ) printf( "  %s\n", events[( nev - i ) % 8] );
  printf( "\n %s\n", status );
  fflush( stdout );
  }

static void help( void )
  {
  printf( "\033[H\033[J"
" a5shell: keys                                                  (any key to return)\n\n"
" recording\n"
"   space   safe / live    safe: nothing is recorded, the loop keeps playing\n"
"   r       sync again     play the sync tones again and re-measure the round trip\n"
"   l       relearn        listen to the quiet again and learn the noise floor\n"
"                          (do it when nothing is playing)\n"
"   g / G   gate           more / less sensitive, 3 dB a press\n"
"   p       profile        choose a gate profile made with a5cal\n\n"
" the loop\n"
"   w       wipe           y = the recorded takes out of the loop, c = everything\n"
"                          including clicks. nothing is deleted\n"
"   h       history        the moments the loop changed, newest first. go back to a\n"
"                          row, or to a take by iteration.offset such as 7.3\n"
"   t       mixing         iterations x takes: mute, play, gain and pan (below)\n"
"   T       takes          every take and its state, as text\n\n"
" mixing (t; ESC to leave)\n"
"   arrows move, shift+arrows select; the left column is a whole iteration\n"
"   [ITER[.TAKE]] [+N|-N] [/N|\\N] then  .  queue   !  now   m p  mute / play   M P  now\n"
"   123.15-2/20.   take 123.15, gain -2 dB, pan 20% right, from its next start\n"
"   123.15\\90!    pan 90% left, at once       123m  mute all of iteration 123\n"
"   123..          toggle every take of 123      m  p  .  on what is selected\n"
"   gain and pan are absolute; a sign or slash with no number keeps the current value.\n"
"   a take given a gain or pan is left alone by the automix. A shows muted iterations.\n\n"
" mode (one at a time; a, then one key)\n"
"   al        loop         every take goes into the loop (the usual)\n"
"   ak        keep         takes are recorded and kept, but not put into the loop\n"
"   a0        hold off     same as al: takes stay in the loop\n"
"   a1 - a9   hold n       a take leaves the loop when n newer takes have overlapped it\n"
"   ap        ping-pong    successive takes swap channels (pan left, then right)\n\n"
" q  leave this shell, aspace carries on       Q  stop aspace\n\n"
" the status line: PLAY or SYNC, time, place in the loop, iteration, takes in the\n"
" loop ((moving) while it adjusts them), REC while a take is open, live/SAFE, mode,\n"
" gate, round trip, profile. above it, in words, what has just happened.\n" );
  fflush( stdout );
  }

int main( int argc, char **argv )
  {
  int nostart = argc == 3 && ! strcmp( argv[2], "--no-start" );
  if( argc != 2 && ! nostart ) { fprintf( stderr, "usage: a5shell DIR [--no-start]\n" ); return 2; }
  int cmd = ctlc_connect( argv[1] ), ev;
  if( cmd < 0 && ! nostart )                                      /* not running: start it (how is a5start's business) */
    {
    char line[1300];
    fprintf( stderr, "a5shell: %s is not running; starting it\n", argv[1] );
    snprintf( line, sizeof line, "a5start '%s'", argv[1] );
    if( system( line ) ) { fprintf( stderr, "a5shell: could not start it\n" ); return 1; }
    cmd = ctlc_connect( argv[1] );
    }
  ev = cmd >= 0 ? ctlc_connect( argv[1] ) : -1;
  if( cmd < 0 || ev < 0 ) { fprintf( stderr, "a5shell: %s is not running\n", argv[1] ); return 1; }
  ctlc_cmd( ev, "WATCH", NULL, NULL );
  { char st[1500] = ""; ctlc_cmd( cmd, "STATE", keep_state, st ); const char *n = strstr( st, "\"name\":\"" ); if( n ) { n += 8; size_t l = strcspn( n, "\"" ); if( l > 99 ) l = 99; memcpy( space, n, l ); space[l] = 0; } }
  struct termios raw;
  if( isatty( 0 ) ) { tcgetattr( 0, &saved ); raw = saved; raw.c_lflag &= ~(tcflag_t)( ICANON | ECHO ); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0; tcsetattr( 0, TCSANOW, &raw ); atexit( restore ); }
  int amode = 0, amount = 4, each = 0, live = 1; (void)each; (void)amount;
  for( ;; )
    {
    ctlc_cmd( cmd, "LINE", keep, NULL );
    live = strstr( status, "SAFE" ) == NULL;
    { const char *a = strstr( status, "automix " );                 /* follow what aspace says, not what this shell last sent */
      if( a ) { a += 8; if( ! strncmp( a, "overlap ", 8 ) ) { amode = 2; amount = atoi( a + 8 ); } else if( ! strncmp( a, "swap", 4 ) ) { amode = 3; each = strncmp( a + 4, " each", 5 ) == 0; } else if( ! strncmp( a, "keep", 4 ) ) amode = 1; else amode = 0; } }
    draw();
    struct pollfd p[2] = { { 0, POLLIN, 0 }, { ev, POLLIN, 0 } };
    if( poll( p, 2, 250 ) <= 0 ) continue;
    if( p[1].revents & POLLIN )
      {
      char b[1200];
      if( ctlc_readline( ev, b, sizeof b, 10 ) < 0 ) break;
      { char pr[240]; if( pretty( b, pr, sizeof pr ) ) snprintf( events[nev++ % 8], 220, "%.200s", pr ); }
      }
    if( p[0].revents & POLLIN )
      {
      char k;
      if( read( 0, &k, 1 ) != 1 ) continue;
      char c[100] = "";
      switch( k )
        {
        case ' ': strcpy( c, live ? "SAFE" : "LIVE" ); break;
        case 'r': strcpy( c, "SYNC" ); break;
        case 'l': strcpy( c, "RELEARN" ); break;
        case 'g': strcpy( c, "GATE -3" ); break;                    /* a lower margin hears more */
        case 'G': strcpy( c, "GATE +3" ); break;
        case '?': help(); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
        case 'a':
          {
          /* a, then one key: l loop, k keep, 0 hold off (= loop), 1-9 hold n iterations (overlap), p ping-pong panning (swap) */
          char d = 0; int w = 0;
          printf( "\033[H\033[J mode:  l loop   k keep   0 hold off   1-9 hold n iterations   p ping-pong pan   (now: %s)", amode == 2 ? "hold" : amode == 3 ? "ping-pong" : amode == 1 ? "keep" : "loop" ); fflush( stdout );
          while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
          if( d == 'l' || d == '0' ) strcpy( c, "AUTOMIX LOOP" );
          else if( d == 'k' ) strcpy( c, "AUTOMIX KEEP" );
          else if( d == 'p' ) strcpy( c, "AUTOMIX SWAP" );
          else if( d >= '1' && d <= '9' ) snprintf( c, sizeof c, "AUTOMIX OVERLAP %d", d - '0' );
          break;
          }
        case 't': a5mixer( cmd, ev, space ); break;
        case 'T': printf( "\033[H\033[J" ); ctlc_cmd( cmd, "TAKES", show, NULL ); printf( "\n(any key)" ); fflush( stdout ); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
        case 'u':
          {
          char l[40] = "";
          restore(); printf( "\nrevert to log point: " ); fflush( stdout );
          if( fgets( l, sizeof l, stdin ) && atoll( l ) > 0 ) snprintf( c, sizeof c, "REVERT %lld", atoll( l ) );
          tcsetattr( 0, TCSANOW, &raw );
          break;
          }
        case 'p':
          {
          /* the profiles are made elsewhere (calibration); here one is picked from the list */
          char names[20][80]; int np = 0;
          printf( "\033[H\033[J profiles:\n\n" );
          { int fd = cmd; char b[300]; snprintf( b, sizeof b, "PROFILES\n" ); if( send( fd, b, strlen( b ), 0 ) > 0 )
              { int r; while( ( r = ctlc_readline( fd, b, sizeof b, 3000 ) ) > 0 && strcmp( b, "OK" ) && strncmp( b, "ERR", 3 ) )
                  { if( np < 20 ) { snprintf( names[np], 80, "%.79s", b ); names[np][strcspn( names[np], "\t" )] = 0; printf( "  %d  %s\n", ++np, b ); } } } }
          if( ! np ) printf( "  (none; a5pace profiles install)\n" );
          printf( "\n number to choose, anything else to cancel: " ); fflush( stdout );
          { char d; int w = 0; while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
            if( d >= '1' && d < '1' + np ) snprintf( c, sizeof c, "PROFILE %s", names[d - '1'] ); }
          break;
          }
        case 'w':
          {
          char d = 0; int w = 0;
          printf( "\033[H\033[J wipe the loop?  y = the recorded takes   c = everything, clicks too   anything else cancels  (a revert can bring them back)" ); fflush( stdout );
          while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
          if( d == 'y' ) strcpy( c, "WIPE" ); else if( d == 'c' ) strcpy( c, "WIPE ALL" );
          break;
          }
        case 'h':
          {
          /* the places to return to, newest first. choose a row, or an iteration and offset (7.3), and the loop goes back to
             how it was just after that take; it needs safe, so it goes safe first and asks before going live again */
          char rows[40][300]; long long seqs[40]; int nr = 0, r;
          printf( "\033[H\033[J history (newest first), the loop as it was after each:\n\n" );
          { char b[400]; snprintf( b, sizeof b, "HISTORY 30\n" ); if( send( cmd, b, strlen( b ), 0 ) > 0 )
              while( ( r = ctlc_readline( cmd, b, sizeof b, 3000 ) ) > 0 && strcmp( b, "OK" ) && strncmp( b, "ERR", 3 ) )
                if( nr < 40 ) { char *t1 = strchr( b, '\t' ); if( ! t1 ) continue; seqs[nr] = atoll( t1 + 1 ); char *t2 = strchr( t1 + 1, '\t' ); snprintf( rows[nr], 300, "%.*s  %s", (int)( t1 - b ), b, t2 ? t2 + 1 : "" ); printf( "  %s\n", rows[nr] ); nr++; } }
          if( ! nr ) printf( "  (nothing yet)\n" );
          char l[60] = "";
          restore(); printf( "\n go back to: a row number, or iteration.offset such as 7.3 (enter cancels): " ); fflush( stdout );
          if( fgets( l, sizeof l, stdin ) && nr )
            {
            int a = 0, o = 0;
            if( strchr( l, '.' ) && sscanf( l, "%d.%d", &a, &o ) == 2 ) snprintf( c, sizeof c, "REVERT TAKE %d %d", a, o );
            else if( atoi( l ) >= 1 && atoi( l ) <= nr ) snprintf( c, sizeof c, "REVERT %lld", seqs[atoi( l ) - 1] );
            }
          tcsetattr( 0, TCSANOW, &raw );
          if( c[0] )
            {
            if( live ) ctlc_cmd( cmd, "SAFE", quiet, NULL );
            ctlc_cmd( cmd, c, quiet, NULL );
            snprintf( events[nev++ % 8], 220, "went back (%s). safe is on: press space to go live", c );
            c[0] = 0;
            }
          break;
          }
        case 'Q': ctlc_cmd( cmd, "QUIT", NULL, NULL ); return 0;
        case 'q': return 0;
        }
      if( c[0] ) ctlc_cmd( cmd, c, quiet, NULL );
      }
    }
  return 0;
  }
