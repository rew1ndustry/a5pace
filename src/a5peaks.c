/* a5peaks DIR: work out the levels (a byte a 0.2 s tick, see asppeaks.h) of the takes that have none, and keep them in the
   session. aspace does this for each new take as it ends; this is for takes recorded before that, or restored by a5recover.
   both channels count (the louder), so a take recorded from one input only may show the other channel's noise. the space may
   be running (it is the one writer: stop it first if you can). prints one line at the end; exit 0 when every take has levels. */
#include <stdio.h>
#include "aspstore.h"
#include "asppeaks.h"

int main( int argc, char **argv )
  {
  char err[ASP_ERR]; AspStore *s; int done = 0, bad = 0;
  if( argc != 2 ) { fprintf( stderr, "usage: a5peaks DIR\n" ); return 2; }
  if( ! ( s = asp_open( argv[1], 0, err ) ) ) { fprintf( stderr, "a5peaks: %s\n", err ); return 1; }
  asp_peaks_backfill( s, &done, &bad );
  printf( "a5peaks: levels made for %d take(s)%s\n", done, bad ? ", some failed" : "" );
  asp_close( s );
  return bad != 0;
  }
