/* a5shell DIR: a terminal shell for a running aspace. a status line, the recent events, and a few keys:
     space  safe / live        r  sync again         l  learn the gate floor again
     g / G  gate more / less sensitive (3 dB)        a  next automix (off, overlap, swap)
     + / -  automix amount up / down                 e  swap: also within an iteration
     u      revert (asks for a log point; safe only) t  list takes
     w      wipe the recorded takes out of the loop (asks)    h  the history: places to return to, and restore one
     p      choose a gate profile from the list    q  leave this shell   Q  stop aspace
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

static struct termios saved;
static void restore( void ) { tcsetattr( 0, TCSANOW, &saved ); }

static char status[400], events[8][220]; static int nev;
static void keep( const char *l, void *u ) { (void)u; snprintf( status, sizeof status, "%s", l ); }
static void show( const char *l, void *u ) { (void)u; printf( "\r\033[K%s\n", l ); }
static void quiet( const char *l, void *u ) { (void)u; if( ! strncmp( l, "ERR", 3 ) ) snprintf( events[nev++ % 8], 220, "%.200s", l ); }

static void draw( void )
  {
  int n = nev < 8 ? nev : 8;
  printf( "\033[H\033[J a5shell   [space] safe/live  [r] sync  [l] relearn  [g/G] gate  [a] automix  [+/-] n  [e] each  [t] takes  [p] profile  [w] wipe  [h] history  [q] quit  [Q] stop\n\n" );
  for( int i = n; i > 0; i-- ) printf( "  %s\n", events[( nev - i ) % 8] );
  printf( "\n %s\n", status );
  fflush( stdout );
  }

int main( int argc, char **argv )
  {
  if( argc != 2 ) { fprintf( stderr, "usage: a5shell DIR\n" ); return 2; }
  int cmd = ctlc_connect( argv[1] ), ev = ctlc_connect( argv[1] );
  if( cmd < 0 || ev < 0 ) { fprintf( stderr, "a5shell: %s is not running\n", argv[1] ); return 1; }
  ctlc_cmd( ev, "WATCH", NULL, NULL );
  struct termios raw;
  if( isatty( 0 ) ) { tcgetattr( 0, &saved ); raw = saved; raw.c_lflag &= ~(tcflag_t)( ICANON | ECHO ); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0; tcsetattr( 0, TCSANOW, &raw ); atexit( restore ); }
  int amode = 0, amount = 4, each = 0, live = 1;
  for( ;; )
    {
    ctlc_cmd( cmd, "LINE", keep, NULL );
    live = strstr( status, "SAFE" ) == NULL;
    { const char *a = strstr( status, "automix " );                 /* follow what aspace says, not what this shell last sent */
      if( a ) { a += 8; if( ! strncmp( a, "overlap ", 8 ) ) { amode = 1; amount = atoi( a + 8 ); } else if( ! strncmp( a, "swap", 4 ) ) { amode = 2; each = strncmp( a + 4, " each", 5 ) == 0; } else amode = 0; } }
    draw();
    struct pollfd p[2] = { { 0, POLLIN, 0 }, { ev, POLLIN, 0 } };
    if( poll( p, 2, 250 ) <= 0 ) continue;
    if( p[1].revents & POLLIN )
      {
      char b[1200];
      if( ctlc_readline( ev, b, sizeof b, 10 ) < 0 ) break;
      snprintf( events[nev++ % 8], 220, "%.200s", b );
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
        case 'a': case '+': case '-': case 'e':
          if( k == 'a' ) amode = ( amode + 1 ) % 3;
          if( k == '+' ) amount++;
          if( k == '-' && amount > 1 ) amount--;
          if( k == 'e' ) each = ! each;
          snprintf( c, sizeof c, "AUTOMIX %s %d%s", amode == 1 ? "OVERLAP" : amode == 2 ? "SWAP" : "OFF", amount, amode == 2 && each ? " each" : "" );
          if( amode == 2 && each ) snprintf( c, sizeof c, "AUTOMIX SWAP each" );
          break;
        case 't': printf( "\033[H\033[J" ); ctlc_cmd( cmd, "TAKES", show, NULL ); printf( "\n(any key)" ); fflush( stdout ); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
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
