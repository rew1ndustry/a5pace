/* a5rebuild: the last resort. make the loop file new from the store:
     a5rebuild DIR [--tank FILE]
   the core never rebuilds (a tank is never dirty); this is for a loop file that is lost, the wrong size, an old format, or corrupt
   (iterations that neither match nor fall by one at one edge). the space must not be running. exit 0 when done. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "aspstore.h"
#include "aspengine.h"

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
  const char *dir = NULL, *tank = NULL; char tp[1200], err[300] = "";
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--tank" ) && i + 1 < argc ) tank = argv[++i];
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { fprintf( stderr, "usage: a5rebuild DIR [--tank FILE]\n" ); return 2; }
    }
  if( ! dir ) { fprintf( stderr, "usage: a5rebuild DIR [--tank FILE]\n" ); return 2; }
  if( running( dir ) ) { fprintf( stderr, "a5rebuild: %s is running; stop it first\n", dir ); return 2; }
  AspStore *s = asp_open( dir, 0, err );
  if( ! s ) { fprintf( stderr, "a5rebuild: %s\n", err ); return 1; }
  if( tank ) snprintf( tp, sizeof tp, "%s", tank ); else snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
  int rc = asp_rebuild_file( s, tp, err, sizeof err );
  asp_close( s );
  if( rc ) { fprintf( stderr, "a5rebuild: %s\n", err ); return 1; }
  printf( "a5rebuild: %s made new from the store\n", tp );
  return 0;
  }
