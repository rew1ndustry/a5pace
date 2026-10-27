/* a5control DIR COMMAND...   send one command to a running aspace and print the reply
   a5control DIR WATCH         print events as they happen: "@tick type json"
   a5control DIR              the one line status
   commands: STATE LINE TAKES SAFE LIVE SYNC RELEARN GATE n|+n|-n RING n AUTOMIX off|fade n|overlap n|swap [each]
             REVERT seq (while SAFE) EDGE drop|starve frames QUIT; with a simulated card: SIM BURST ch amp [ticks],
             SIM DUP 0|1, STEP n (waits until done) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ctlclient.h"

static void show( const char *l, void *u ) { (void)u; puts( l ); }

int main( int argc, char **argv )
  {
  int start = argc > 1 && ! strcmp( argv[1], "--start" );          /* a5control --start DIR CMD...: start the space first if it is not running */
  if( start ) { argv++; argc--; }
  if( argc < 2 ) { fprintf( stderr, "usage: a5control [--start] DIR [COMMAND...]\n" ); return 2; }
  int fd = ctlc_connect( argv[1] );
  if( fd < 0 && start )
    {
    char l[1300]; snprintf( l, sizeof l, "a5start '%s' >&2", argv[1] );
    if( system( l ) == 0 ) fd = ctlc_connect( argv[1] );
    }
  if( fd < 0 ) { fprintf( stderr, "a5control: %s is not running (no control port)\n", argv[1] ); return 1; }
  char line[1000] = "LINE";
  if( argc > 2 ) { line[0] = 0; for( int i = 2; i < argc; i++ ) { if( i > 2 ) strcat( line, " " ); strncat( line, argv[i], sizeof line - strlen( line ) - 2 ); } }
  int rc = ctlc_cmd( fd, line, show, NULL );
  if( rc == 0 && ! strcasecmp( line, "WATCH" ) )
    {
    char b[1200]; int r;
    fflush( stdout );
    while( ( r = ctlc_readline( fd, b, sizeof b, -1 ) ) > 0 ) { puts( b ); fflush( stdout ); }
    }
  close( fd );
  return rc < 0 ? 1 : rc;
  }
