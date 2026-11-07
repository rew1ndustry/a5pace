/* a5peaks DIR: work out the levels (a byte a 0.2 s tick, see asppeaks.h) of the takes that have none, and keep them in the
   session. aspace does this for each new take as it ends; this is for takes recorded before that, or restored by a5recover.
   both channels count (the louder), so a take recorded from one input only may show the other channel's noise. the space may
   be running (it is the one writer: stop it first if you can). prints one line at the end; exit 0 when every take has levels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspstore.h"
#include "takefile.h"
#include "asptank.h"
#include "asppeaks.h"

int main( int argc, char **argv )
  {
  char err[ASP_ERR]; AspStore *s; AspPk pk = { 0 }; int64_t ids[64], after = 0; int done = 0, bad = 0, n;
  if( argc != 2 ) { fprintf( stderr, "usage: a5peaks DIR\n" ); return 2; }
  if( ! ( s = asp_open( argv[1], 0, err ) ) ) { fprintf( stderr, "a5peaks: %s\n", err ); return 1; }
  static int32_t buf[2 * 4096], st[2 * 4096];
  while( ( n = asp_peaks_need( s, after, ids, 64 ) ) > 0 )
    for( int i = 0; i < n; i++ )
      {
      AspTake t; char path[1200]; AspTakeR *r; int64_t pos = 0; int got; after = ids[i];
      if( asp_take_get( s, ids[i], &t ) || asp_take_path( s, &t, path, sizeof path ) || ! ( r = asp_tr_open( path ) ) )
        { fprintf( stderr, "a5peaks: take %lld cannot be read\n", (long long)ids[i] ); bad++; continue; }
      int chans = asp_tr_hdr( r )->channels;
      asp_pk_reset( &pk );
      while( ( got = asp_tr_get( r, buf, 4096 ) ) > 0 )
        {
        const int32_t *p = buf;
        if( chans == 1 ) { for( int k = 0; k < got; k++ ) st[2*k] = st[2*k+1] = buf[k]; p = st; }
        asp_pk_add( &pk, p, got, pos, -1 ); pos += got;
        }
      asp_tr_close( r );
      if( got < 0 || ! pk.n || asp_peaks_set( s, ids[i], pk.v, pk.n ) ) { fprintf( stderr, "a5peaks: take %lld: no levels\n", (long long)ids[i] ); bad++; }
      else done++;
      }
  printf( "a5peaks: levels made for %d take(s)%s\n", done, bad ? ", some failed" : "" );
  asp_pk_free( &pk ); asp_close( s );
  return bad != 0;
  }
