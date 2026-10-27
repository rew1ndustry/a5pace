/* aspsh DIR: a terminal shell for a running aspace. a status line, the recent events, and a few keys:
     space  safe / live        r  sync again         l  learn the gate floor again
     g / G  gate more / less sensitive (3 dB)        a  next automix (off, fade, overlap, swap)
     + / -  automix amount up / down                 e  swap: also within an iteration
     d / u  loop gain down / up 6 dB                  t  list takes
     v      revert (asks for a log point; safe only)
     q      leave this shell   Q  stop aspace
   nothing here is needed to run aspace: it is only a window on the control port. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
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
  printf( "\033[H\033[J aspsh   [space] safe/live  [r] sync  [l] relearn  [g/G] gate  [a] automix  [+/-] n  [e] each  [t] takes  [d/u] loop gain  [v] revert  [q] quit  [Q] stop\n\n" );
  for( int i = n; i > 0; i-- ) printf( "  %s\n", events[( nev - i ) % 8] );
  printf( "\n %s\n", status );
  fflush( stdout );
  }

int main( int argc, char **argv )
  {
  if( argc != 2 ) { fprintf( stderr, "usage: aspsh DIR\n" ); return 2; }
  int cmd = ctlc_connect( argv[1] ), ev = ctlc_connect( argv[1] );
  if( cmd < 0 || ev < 0 ) { fprintf( stderr, "aspsh: %s is not running\n", argv[1] ); return 1; }
  ctlc_cmd( ev, "WATCH", NULL, NULL );
  struct termios raw;
  if( isatty( 0 ) ) { tcgetattr( 0, &saved ); raw = saved; raw.c_lflag &= ~(tcflag_t)( ICANON | ECHO ); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0; tcsetattr( 0, TCSANOW, &raw ); atexit( restore ); }
  int amode = 0, amount = 4, each = 0, live = 1;
  for( ;; )
    {
    ctlc_cmd( cmd, "LINE", keep, NULL );
    live = strstr( status, "SAFE" ) == NULL;
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
          if( k == 'a' ) amode = ( amode + 1 ) % 4;
          if( k == '+' ) amount++;
          if( k == '-' && amount > 1 ) amount--;
          if( k == 'e' ) each = ! each;
          snprintf( c, sizeof c, "AUTOMIX %s %d%s", amode == 1 ? "FADE" : amode == 2 ? "OVERLAP" : amode == 3 ? "SWAP" : "OFF", amount, amode == 3 && each ? " each" : "" );
          if( amode == 3 && each ) snprintf( c, sizeof c, "AUTOMIX SWAP each" );
          break;
        case 't': printf( "\033[H\033[J" ); ctlc_cmd( cmd, "TAKES", show, NULL ); printf( "\n(any key)" ); fflush( stdout ); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
        case 'd': strcpy( c, "DOWN 1" ); break;
        case 'u': strcpy( c, "UP 1" ); break;
        case 'v':
          {
          char l[40] = "";
          restore(); printf( "\nrevert to log point: " ); fflush( stdout );
          if( fgets( l, sizeof l, stdin ) && atoll( l ) > 0 ) snprintf( c, sizeof c, "REVERT %lld", atoll( l ) );
          tcsetattr( 0, TCSANOW, &raw );
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
