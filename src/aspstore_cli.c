/* aspstore DIR info | takes | log [from] | fold [seq] | revert SEQ | export ID OUT.d32 | name MIX [SEQ] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "aspstore.h"

static void show( int64_t id, const char *st, void *u )
  { (void)u; printf( "take %lld %s\n", (long long)id, st ); }

int main( int argc, char **argv )
  {
  char err[ASP_ERR];
  if( argc < 3 ) { fprintf( stderr, "usage: aspstore DIR info|takes|log [from]|fold [seq]|revert SEQ|export ID OUT.d32|name MIX [SEQ]\n" ); return 2; }
  const char *c = argv[2];
  int ro = ! strcmp( c, "info" ) || ! strcmp( c, "takes" ) || ! strcmp( c, "fold" ) || ! strcmp( c, "export" );
  AspStore *s = asp_open( argv[1], ro, err );
  if( ! s ) { fprintf( stderr, "%s\n", err ); return 1; }
  int rc = 0;
  if( ! strcmp( c, "info" ) )
    printf( "session %s\nlog %lld\n", asp_session_id( s ), (long long)asp_log_last( s ) );
  else if( ! strcmp( c, "fold" ) )
    rc = asp_fold( s, argc > 3 ? atoll( argv[3] ) : -1, show, NULL ) ? 1 : 0;
  else if( ! strcmp( c, "revert" ) && argc > 3 )
    { int64_t q = asp_revert( s, atoll( argv[3] ), "cli" ); printf( "%lld\n", (long long)q ); rc = q < 0; }
  else if( ! strcmp( c, "export" ) && argc > 4 )
    rc = asp_take_export_d32( s, atoll( argv[3] ), argv[4] ) ? 1 : 0;
  else if( ! strcmp( c, "name" ) && argc > 3 )
    {
    if( argc > 4 ) rc = asp_mix_name( s, argv[3], atoll( argv[4] ) ) ? 1 : 0;
    else printf( "%lld\n", (long long)asp_mix_seq( s, argv[3] ) );
    }
  else if( ! strcmp( c, "takes" ) || ! strcmp( c, "log" ) )
    {
    /* a plain listing through the sql interface keeps the cli tiny */
    fprintf( stderr, "use sqlite3 or the python tools for listings\n" ); rc = 2;
    }
  else rc = 2;
  asp_close( s );
  return rc;
  }
