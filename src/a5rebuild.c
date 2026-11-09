/* a5rebuild: the last resort, outside the core (which never rebuilds: a tank is never dirty).
     a5rebuild DIR [--sql] [--tank [FILE]]
   --sql   check session.db (integrity_check, then every table against the schema, tools/schema.sql). the derived tables (peaks,
           moves, peers) that are missing or the wrong shape are made again; the levels of the takes that have none are worked out
           from the take files. a moves list that is readable is kept (it is the resume list). the history (session, takes, log,
           mixes, notes) is never written: if it is damaged or the wrong shape the run stops and says so, restore from a copy.
   --tank  make the loop file new from the store (a lost, wrong-size, old-format or corrupt loop). FILE defaults to cache/loop.d64.
   neither flag = both. the space must not be running. exit 0 when done. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "aspstore.h"
#include "aspengine.h"
#include "asppeaks.h"
#include <sqlite3.h>
#include "schema_sql.h"

/* the columns of a table, as one string */
static void cols( sqlite3 *db, const char *t, char *out, size_t n )
  {
  char q[100]; sqlite3_stmt *st; size_t l = 0;
  out[0] = 0;
  snprintf( q, sizeof q, "pragma table_info(%s)", t );
  if( sqlite3_prepare_v2( db, q, -1, &st, NULL ) ) return;
  while( sqlite3_step( st ) == SQLITE_ROW && l + 80 < n )
    l += snprintf( out + l, n - l, "%s %s;", sqlite3_column_text( st, 1 ), sqlite3_column_text( st, 2 ) );
  sqlite3_finalize( st );
  }

static int derived( const char *t ) { return ! strcmp( t, "peaks" ) || ! strcmp( t, "moves" ) || ! strcmp( t, "peers" ); }

/* 0 = good (and repaired where it could be), -1 = stop */
static int check_sql( const char *dir )
  {
  char path[1200], a[1500], b[1500]; sqlite3 *db = NULL, *ref = NULL; sqlite3_stmt *st; int bad = 0, fixed = 0;
  snprintf( path, sizeof path, "%s/session.db", dir );
  if( sqlite3_open_v2( path, &db, SQLITE_OPEN_READWRITE, NULL ) ) { fprintf( stderr, "a5rebuild: cannot open %s\n", path ); return -1; }
  if( ! sqlite3_prepare_v2( db, "pragma integrity_check", -1, &st, NULL ) )
    {
    while( sqlite3_step( st ) == SQLITE_ROW )
      if( strcmp( (const char *)sqlite3_column_text( st, 0 ), "ok" ) ) { fprintf( stderr, "a5rebuild: integrity: %s\n", sqlite3_column_text( st, 0 ) ); bad = 1; }
    sqlite3_finalize( st );
    }
  if( bad ) { fprintf( stderr, "a5rebuild: session.db is damaged; restore it from a copy (nothing was changed)\n" ); sqlite3_close( db ); return -1; }
  printf( "a5rebuild: session.db integrity ok\n" );
  sqlite3_open( ":memory:", &ref );
  sqlite3_exec( ref, SCHEMA_SQL, NULL, NULL, NULL );
  if( ! sqlite3_prepare_v2( ref, "select name, sql from sqlite_master where type='table' and name not like 'sqlite_%'", -1, &st, NULL ) )
    {
    while( sqlite3_step( st ) == SQLITE_ROW )
      {
      char name[64], ddl[3000];
      snprintf( name, sizeof name, "%s", (const char *)sqlite3_column_text( st, 0 ) ); snprintf( ddl, sizeof ddl, "%s", (const char *)sqlite3_column_text( st, 1 ) );
      cols( ref, name, a, sizeof a ); cols( db, name, b, sizeof b );
      if( ! b[0] )
        {
        if( ! derived( name ) ) { fprintf( stderr, "a5rebuild: table %s is missing: the history cannot be made again, restore from a copy\n", name ); bad = 1; continue; }
        if( sqlite3_exec( db, ddl, NULL, NULL, NULL ) ) { fprintf( stderr, "a5rebuild: cannot make %s\n", name ); bad = 1; continue; }
        printf( "a5rebuild: table %s was missing: made\n", name ); fixed++;
        }
      else if( strcmp( a, b ) )
        {
        if( ! derived( name ) ) { fprintf( stderr, "a5rebuild: table %s is not the shape of this version; not touched: restore from a copy\n", name ); bad = 1; continue; }
        char dr[100]; snprintf( dr, sizeof dr, "drop table %s", name );
        if( sqlite3_exec( db, dr, NULL, NULL, NULL ) || sqlite3_exec( db, ddl, NULL, NULL, NULL ) ) { fprintf( stderr, "a5rebuild: cannot make %s again\n", name ); bad = 1; continue; }
        printf( "a5rebuild: table %s was the wrong shape: made again\n", name ); fixed++;
        }
      }
    sqlite3_finalize( st );
    }
  if( ! bad )
    {
    int v = 0;
    if( ! sqlite3_prepare_v2( db, "select schema_version from session limit 1", -1, &st, NULL ) ) { if( sqlite3_step( st ) == SQLITE_ROW ) v = sqlite3_column_int( st, 0 ); sqlite3_finalize( st ); }
    if( v != 2 && v != 3 ) { fprintf( stderr, "a5rebuild: schema version %d: a session of an older aspace, not touched\n", v ); bad = 1; }
    else if( v == 2 ) { sqlite3_exec( db, "update session set schema_version=3; pragma user_version=3;", NULL, NULL, NULL ); printf( "a5rebuild: schema version 2 brought to 3\n" ); fixed++; }
    }
  sqlite3_close( ref ); sqlite3_close( db );
  if( ! bad && ! fixed ) printf( "a5rebuild: schema is as it should be\n" );
  return bad ? -1 : 0;
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

static const char *HELP =
"usage: a5rebuild DIR [--sql] [--tank [FILE]]\n"
"the last resort when a space's files are damaged. the space must not be running.\n"
"  --sql          check session.db: integrity, then every table against the schema. the derived tables (peaks, moves,\n"
"                 peers) that are missing or the wrong shape are made again, and takes with no levels get them from their\n"
"                 files. the history (session, takes, log, mixes, notes) is never written: if it is damaged, restore a copy.\n"
"  --tank [FILE]  make the loop file new from the store (default DIR/cache/loop.d64): lost, wrong size, old format, corrupt.\n"
"  neither        both. DIR comes first. exit 0 when done, 1 when something could not be mended, 2 for a bad call.\n"
"see docs/REBUILD.md\n";

int main( int argc, char **argv )
  {
  const char *dir = NULL, *tank = NULL; char tp[1200], err[300] = ""; int do_sql = 0, do_tank = 0;
  const char *use = "usage: a5rebuild DIR [--sql] [--tank [FILE]]   (a5rebuild --help)\n";
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--help" ) || ! strcmp( argv[i], "-h" ) ) { fputs( HELP, stdout ); return 0; }
    if( ! strcmp( argv[i], "--sql" ) ) do_sql = 1;
    else if( ! strcmp( argv[i], "--tank" ) ) { do_tank = 1; if( i + 1 < argc && argv[i+1][0] != '-' && dir ) tank = argv[++i]; }
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { fputs( use, stderr ); return 2; }
    }
  if( ! dir ) { fputs( use, stderr ); return 2; }
  if( ! do_sql && ! do_tank ) do_sql = do_tank = 1;
  if( running( dir ) ) { fprintf( stderr, "a5rebuild: %s is running; stop it first\n", dir ); return 2; }
  if( do_sql && check_sql( dir ) ) return 1;
  AspStore *s = asp_open( dir, 0, err );
  if( ! s ) { fprintf( stderr, "a5rebuild: %s\n", err ); return 1; }
  if( do_sql )
    {
    int made = 0, bad = 0; asp_peaks_backfill( s, &made, &bad );
    printf( "a5rebuild: levels made for %d take(s)%s\n", made, bad ? ", some failed" : "" );
    }
  if( do_tank )
    {
    if( tank ) snprintf( tp, sizeof tp, "%s", tank ); else snprintf( tp, sizeof tp, "%s/cache/loop.d64", dir );
    if( asp_rebuild_file( s, tp, err, sizeof err ) ) { fprintf( stderr, "a5rebuild: %s\n", err ); asp_close( s ); return 1; }
    printf( "a5rebuild: %s made new from the store\n", tp );
    }
  asp_close( s );
  return 0;
  }
