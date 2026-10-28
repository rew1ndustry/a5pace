/* a5recover: find the takes a run left behind when it died, and put them back.
     a5recover DIR [--dry]
   a run that dies (killed, power cut) leaves two kinds of take, both found from the log and the tmp folder:
     - a take that had ended and been handed on but whose row was never made: its file is at its place or still at its part. if it
       decodes whole it is restored under its own id and slot, with the state the log gives it, so the loop gets it back;
     - a take being recorded: its part has no finished header. what decodes of it is written again as a take of its own (marked damaged,
       kept out of the loop: state off) in its slot, and the part is moved to tmp/recovered/.
   empty spare files are removed. nothing else is deleted; the loop file is dropped (it is a cache) when a take is put back, so the next
   start rebuilds it. the space must not be running. exit 0 when nothing was lost. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "aspstore.h"
#include "takefile.h"
#include "asptank.h"

typedef struct { int64_t id, iter, off; char part[200]; int row; } Ev;
typedef struct { Ev *e; int n, cap; } List;

static int64_t jnum( const char *j, const char *key, int64_t d )
  {
  char k[64]; snprintf( k, sizeof k, "\"%s\":", key );
  const char *p = strstr( j, k );
  return p ? atoll( p + strlen( k ) ) : d;
  }
static void jstr( const char *j, const char *key, char *out, size_t n )
  {
  char k[64]; snprintf( k, sizeof k, "\"%s\":\"", key );
  const char *p = strstr( j, k );
  out[0] = 0;
  if( p ) { p += strlen( k ); size_t l = strcspn( p, "\"" ); if( l >= n ) l = n - 1; memcpy( out, p, l ); out[l] = 0; }
  }

typedef struct { List pend, news, gone; int64_t maxid; } Seen;
static void add( List *l, int64_t id, int64_t iter, int64_t off, const char *part )
  {
  if( l->n == l->cap ) { l->cap = l->cap ? 2 * l->cap : 32; l->e = realloc( l->e, l->cap * sizeof *l->e ); }
  l->e[l->n].id = id; l->e[l->n].iter = iter; l->e[l->n].off = off; l->e[l->n].row = 0;
  snprintf( l->e[l->n].part, sizeof l->e[0].part, "%s", part ? part : "" ); l->n++;
  }
static void ev_cb( int64_t seq, int64_t tick, const char *actor, const char *type, const char *args, void *u )
  {
  Seen *s = u; char part[200];
  (void)seq; (void)tick; (void)actor;
  jstr( args, "part", part, sizeof part );
  if( ! strcmp( type, "take_added" ) || ! strcmp( type, "take_pending" ) ) { int64_t id = jnum( args, "take", 0 ); if( id > s->maxid ) s->maxid = id; }
  if( ! strcmp( type, "take_pending" ) ) add( &s->pend, jnum( args, "take", 0 ), jnum( args, "iter", 0 ), jnum( args, "off", 0 ), part );
  else if( ! strcmp( type, "new" ) ) add( &s->news, 0, jnum( args, "iter", 0 ), jnum( args, "off", 0 ), part );
  else if( ! strcmp( type, "discarded" ) ) add( &s->gone, 0, jnum( args, "iter", 0 ), jnum( args, "off", 0 ), "" );
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

static int nop( const int32_t *pcm, unsigned frames, int ch, void *u ) { (void)pcm; (void)frames; (void)ch; (void)u; return 0; }
static int whole( const char *path ) { return asp_take_frames( path, -1, nop, NULL ) == 0; }
static int have( const char *path ) { return access( path, F_OK ) == 0; }

int main( int argc, char **argv )
  {
  const char *dir = NULL; int dry = 0;
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--dry" ) ) dry = 1;
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { fprintf( stderr, "usage: a5recover DIR [--dry]\n" ); return 2; }
    }
  if( ! dir ) { fprintf( stderr, "usage: a5recover DIR [--dry]\n" ); return 2; }
  if( running( dir ) ) { fprintf( stderr, "a5recover: %s is running; stop it first\n", dir ); return 2; }
  char err[ASP_ERR];
  AspStore *s = asp_open( dir, 0, err );
  if( ! s ) { fprintf( stderr, "a5recover: %s\n", err ); return 1; }
  int64_t pid = 0;
  if( asp_store_crashed( s, &pid ) ) printf( "the last run (pid %lld) did not close\n", (long long)pid );
  else printf( "the last run closed cleanly; looking all the same\n" );

  Seen sn; memset( &sn, 0, sizeof sn );
  asp_log_since( s, 0, ev_cb, &sn );
  int restored = 0, salvaged = 0, lost = 0, spares = 0, odd = 0;
  char path[1400];

  /* handed on, row never made */
  for( int i = 0; i < sn.pend.n; i++ )
    {
    Ev *e = sn.pend.e + i; AspTake t;
    if( ! asp_take_get( s, e->id, &t ) ) continue;                       /* it has its row */
    snprintf( t.artist, sizeof t.artist, "%s", asp_artist( s ) ); t.id = 0; t.iter = e->iter; t.off = e->off;
    char at[1400], tmp[1400];
    asp_take_path( s, &t, at, sizeof at );
    snprintf( tmp, sizeof tmp, "%s/tmp/%s", dir, e->part );
    const char *file = have( at ) ? at : have( tmp ) ? tmp : NULL;
    if( ! file ) { printf( "  take %lld (iteration %lld, second %lld): its file is gone\n", (long long)e->id, (long long)e->iter, (long long)e->off ); lost++; continue; }
    if( ! whole( file ) ) { printf( "  take %lld (iteration %lld, second %lld): its file does not decode whole (%s)\n", (long long)e->id, (long long)e->iter, (long long)e->off, file ); lost++; continue; }
    printf( "  take %lld (iteration %lld, second %lld): %s\n", (long long)e->id, (long long)e->iter, (long long)e->off, dry ? "can be put back" : "put back" );
    if( ! dry && asp_take_restore( s, e->id, e->iter, e->off, file, "off" ) ) { printf( "    could not\n" ); lost++; } else restored++;
    }

  /* being recorded when the run died */
  for( int i = 0; i < sn.news.n; i++ )
    {
    Ev *e = sn.news.e + i; AspTake t; int skip = 0;
    if( ! asp_take_find( s, e->iter, e->off, &t ) ) continue;               /* it has a row now */
    for( int j = 0; j < sn.pend.n; j++ ) if( sn.pend.e[j].iter == e->iter && sn.pend.e[j].off == e->off ) skip = 1;
    for( int j = 0; j < sn.gone.n; j++ ) if( sn.gone.e[j].iter == e->iter && sn.gone.e[j].off == e->off ) skip = 1;
    snprintf( path, sizeof path, "%s/tmp/%s", dir, e->part );
    if( skip || ! e->part[0] || ! have( path ) ) continue;
    AspTakeR *r = asp_tr_open( path );
    if( ! r ) { printf( "  iteration %lld, second %lld: %s is not a take\n", (long long)e->iter, (long long)e->off, e->part ); odd++; continue; }
    if( asp_tr_hdr( r )->finished )                                         /* ended, but never adopted */
      { asp_tr_close( r ); printf( "  iteration %lld, second %lld: ended but was not stored: %s\n", (long long)e->iter, (long long)e->off, dry ? "can be stored" : "stored" );
        if( ! dry && asp_take_adopt( s, e->iter, e->off, path, "off", NULL, 0, NULL ) ) { printf( "    could not\n" ); lost++; } else salvaged++; continue; }
    int ch = asp_tr_hdr( r )->channels, rate = asp_tr_hdr( r )->rate;
    char np[1300]; AspHdr h2; int64_t total = 0, pk = 0;
    int32_t *buf = malloc( (size_t)ASP_TICK * ch * 4 );
    AspTakeW *w = NULL;
    if( ! dry && ! asp_tmp_path( s, np, sizeof np ) ) w = asp_tw_open( np, asp_artist_id( s ), ch, rate, asp_tr_hdr( r )->input );
    int n;
    while( ( n = asp_tr_get( r, buf, ASP_TICK ) ) > 0 ) { total += n; if( w && asp_tw_put( w, buf, (unsigned)n ) ) { asp_tw_abort( w ); w = NULL; } }
    free( buf ); asp_tr_close( r );
    if( dry ) { printf( "  iteration %lld, second %lld: was being recorded; %lld frames (%.1f s) can be saved\n", (long long)e->iter, (long long)e->off, (long long)total, total / 48000.0 ); salvaged += total > 0; lost += total == 0; continue; }
    if( ! w || total == 0 ) { if( w ) { asp_tw_abort( w ); unlink( np ); } printf( "  iteration %lld, second %lld: was being recorded; nothing could be saved\n", (long long)e->iter, (long long)e->off ); lost++; continue; }
    if( asp_tw_close( w, 1, &h2 ) ) { unlink( np ); lost++; continue; }
    for( int k = 0; k < h2.channels; k++ ) if( h2.peak[k] > pk ) pk = h2.peak[k];
    char st[300];
    snprintf( st, sizeof st, "{\"status\":\"off\",\"lev\":[\"0\",\"0\"],\"flip\":0,\"mono\":0,\"src\":0,\"len\":%lld,\"gone\":0,\"peak\":%lld,\"recovered\":1}", (long long)total, (long long)pk );
    AspTake k2;
    if( asp_take_adopt( s, e->iter, e->off, np, "off", st, 0, &k2 ) ) { unlink( np ); printf( "  iteration %lld, second %lld: could not be stored\n", (long long)e->iter, (long long)e->off ); lost++; continue; }
    char rd[1300], to[1400]; snprintf( rd, sizeof rd, "%s/tmp/recovered", dir ); mkdir( rd, 0777 );
    snprintf( to, sizeof to, "%s/%s", rd, e->part ); rename( path, to );
    printf( "  iteration %lld, second %lld: was being recorded; %.1f s saved as take %lld (kept out of the loop)\n", (long long)e->iter, (long long)e->off, total / 48000.0, (long long)k2.id );
    salvaged++;
    }

  /* spares made ahead and never used: empty, so nothing is lost */
  {
  char td[1300]; snprintf( td, sizeof td, "%s/tmp", dir );
  DIR *d = opendir( td ); struct dirent *de;
  while( d && ( de = readdir( d ) ) )
    {
    struct stat sb;
    snprintf( path, sizeof path, "%s/%s", td, de->d_name );
    if( ! strncmp( de->d_name, "spare_", 6 ) && ! stat( path, &sb ) && sb.st_size == 0 ) { if( ! dry ) unlink( path ); spares++; }
    }
  if( d ) closedir( d );
  }

  /* rows whose file is not what it should be: told, not mended */
  for( int64_t id = 1; id <= sn.maxid; id++ )
    {
    AspTake t; char at[1400]; AspHdr h;
    if( asp_take_get( s, id, &t ) ) continue;
    asp_take_path( s, &t, at, sizeof at );
    if( ! have( at ) ) { printf( "  take %lld (iteration %lld, second %lld): the row is there but its file is missing\n", (long long)id, (long long)t.iter, (long long)t.off ); lost++; }
    else if( asp_take_header( at, &h ) || ! h.finished ) { printf( "  take %lld (iteration %lld, second %lld): its file is not a finished take\n", (long long)id, (long long)t.iter, (long long)t.off ); lost++; }
    }

  char a[200];
  snprintf( a, sizeof a, "{\"restored\":%d,\"salvaged\":%d,\"lost\":%d,\"spares\":%d}", restored, salvaged, lost, spares );
  if( ! dry ) { asp_log( s, "a5recover", "recovered", a, -1 ); if( restored || salvaged ) { snprintf( path, sizeof path, "%s/cache/loop.d64", dir ); unlink( path ); } }
  printf( "%s: %d put back, %d saved from being recorded, %d lost, %d empty spares %s%s\n", dry ? "dry run" : "done", restored, salvaged, lost, spares, dry ? "found" : "removed",
          ( restored || salvaged ) && ! dry ? "; the loop will be rebuilt at the next start" : "" );
  asp_close( s );
  return lost ? 1 : 0;
  }
