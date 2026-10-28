#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include "sqlite3.h"
#include "keeper.h"

typedef struct Job { struct Job *next; int64_t tag; int spare; char path[1200], from[1200]; } Job;

struct AspKeeper
  {
  pthread_t th;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  Job *head, *tail;
  int64_t *done; int ndone, capdone;
  char (*ready)[1200]; int nready, capready;                  /* spare files made and not yet taken */
  int stop, checkpoints, period_ms;
  char db[1200];
  };

static void sync_fd_of( const char *path, int dir )
  {
  int fd = open( path, O_RDONLY );
  if( fd < 0 ) return;
  if( dir ) fsync( fd ); else fdatasync( fd );
  close( fd );
  }

static void mkdir_p( const char *path )
  {
  char tmp[1200], *p;
  snprintf( tmp, sizeof tmp, "%s", path );
  for( p = tmp + 1; *p; p++ ) if( *p == '/' ) { *p = 0; mkdir( tmp, 0777 ); *p = '/'; }
  mkdir( tmp, 0777 );
  }

/* the file, the directory it is in, and the one above (a new directory is a new entry there) */
void asp_settle_now( const char *path )
  {
  char d[1200], *s;
  sync_fd_of( path, 0 );
  snprintf( d, sizeof d, "%s", path );
  for( int up = 0; up < 2; up++ )
    {
    if( ! ( s = strrchr( d, '/' ) ) ) break;
    *s = 0;
    sync_fd_of( d[0] ? d : "/", 1 );
    }
  }

static void pause_ms( const char *var )
  {
  const char *v = getenv( var );
  if( v && atoi( v ) > 0 ) { struct timespec t = { atoi( v ) / 1000, ( atoi( v ) % 1000 ) * 1000000L }; nanosleep( &t, NULL ); }
  }

static void *keep_main( void *u )
  {
  AspKeeper *k = u;
  sqlite3 *db = NULL;
  struct timespec last; clock_gettime( CLOCK_MONOTONIC, &last );
  if( k->db[0] && ( sqlite3_open_v2( k->db, &db, SQLITE_OPEN_READWRITE, NULL ) != SQLITE_OK ) ) { sqlite3_close( db ); db = NULL; }
  if( db ) sqlite3_busy_timeout( db, 5000 );
  pthread_mutex_lock( &k->mu );
  for( ;; )
    {
    while( k->head )
      {
      Job *j = k->head;
      k->head = j->next; if( ! k->head ) k->tail = NULL;
      pthread_mutex_unlock( &k->mu );
      if( j->spare )                                           /* an empty file, made ahead of time */
        {
        char d[1200], *sl; int fd;
        snprintf( d, sizeof d, "%s", j->path );
        if( ( sl = strrchr( d, '/' ) ) ) { *sl = 0; mkdir_p( d ); }
        fd = open( j->path, O_WRONLY | O_CREAT | O_EXCL, 0644 );
        if( fd >= 0 )
          {
          close( fd );
          pthread_mutex_lock( &k->mu );
          if( k->nready == k->capready ) { k->capready = k->capready ? 2 * k->capready : 4; k->ready = realloc( k->ready, k->capready * sizeof *k->ready ); }
          snprintf( k->ready[k->nready++], 1200, "%s", j->path );
          pthread_mutex_unlock( &k->mu );
          }
        free( j );
        pthread_mutex_lock( &k->mu );
        continue;
        }
      pause_ms( "ASP_KEEPER_PAUSE_BEFORE_MS" );                /* (for tests: hold a take before it is placed, to kill the run there) */
      if( j->from[0] )                                         /* a take to be put in its place first */
        {
        char d[1200], *sl;
        snprintf( d, sizeof d, "%s", j->path );
        if( ( sl = strrchr( d, '/' ) ) ) { *sl = 0; mkdir_p( d ); }
        if( rename( j->from, j->path ) ) j->tag = -j->tag;       /* told as a negative tag: it did not get there */
        }
      asp_settle_now( j->path );
      if( j->from[0] ) pause_ms( "ASP_KEEPER_PAUSE_AFTER_MS" );  /* (for tests: placed and synced, not yet reported) */                               /* may take seconds on a card; nobody waits for it */
      pthread_mutex_lock( &k->mu );
      if( k->ndone == k->capdone ) { k->capdone = k->capdone ? 2 * k->capdone : 16; k->done = realloc( k->done, k->capdone * sizeof *k->done ); }
      k->done[k->ndone++] = j->tag;
      free( j );
      }
    if( k->stop ) break;
    struct timespec now, until; clock_gettime( CLOCK_MONOTONIC, &now );
    long long due = k->period_ms - ( ( now.tv_sec - last.tv_sec ) * 1000LL + ( now.tv_nsec - last.tv_nsec ) / 1000000 );
    if( db && due <= 0 )
      {
      pthread_mutex_unlock( &k->mu );
      sqlite3_wal_checkpoint_v2( db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL );   /* about a second on a card; the core's commits go on meanwhile */
      pthread_mutex_lock( &k->mu );
      k->checkpoints++;
      clock_gettime( CLOCK_MONOTONIC, &last );
      continue;
      }
    clock_gettime( CLOCK_REALTIME, &until );
    long long wait = db ? due : 60000;
    until.tv_sec += wait / 1000; until.tv_nsec += ( wait % 1000 ) * 1000000L;
    if( until.tv_nsec >= 1000000000L ) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait( &k->cv, &k->mu, &until );
    }
  pthread_mutex_unlock( &k->mu );
  if( db ) sqlite3_close( db );
  return NULL;
  }

AspKeeper *asp_keeper_start( const char *db, int period_ms )
  {
  AspKeeper *k = calloc( 1, sizeof *k );
  if( ! k ) return NULL;
  pthread_mutex_init( &k->mu, NULL ); pthread_cond_init( &k->cv, NULL );
  k->period_ms = period_ms > 0 ? period_ms : 30000;
  if( db ) snprintf( k->db, sizeof k->db, "%s", db );
  if( pthread_create( &k->th, NULL, keep_main, k ) ) { free( k ); return NULL; }
  return k;
  }

void asp_keeper_spare( AspKeeper *k, const char *path )
  {
  Job *j = calloc( 1, sizeof *j );
  if( ! j ) return;
  j->spare = 1; snprintf( j->path, sizeof j->path, "%s", path );
  pthread_mutex_lock( &k->mu );
  if( k->tail ) k->tail->next = j; else k->head = j;
  k->tail = j;
  pthread_cond_signal( &k->cv );
  pthread_mutex_unlock( &k->mu );
  }

int asp_keeper_spare_get( AspKeeper *k, char *path, size_t n )
  {
  int got = 0;
  pthread_mutex_lock( &k->mu );
  if( k->nready > 0 )
    {
    snprintf( path, n, "%s", k->ready[0] );
    memmove( k->ready, k->ready + 1, ( --k->nready ) * sizeof *k->ready );
    got = 1;
    }
  pthread_mutex_unlock( &k->mu );
  return got;
  }

void asp_keeper_place( AspKeeper *k, int64_t tag, const char *from, const char *to )
  {
  Job *j = calloc( 1, sizeof *j );
  if( ! j ) return;
  j->tag = tag; snprintf( j->path, sizeof j->path, "%s", to ); snprintf( j->from, sizeof j->from, "%s", from );
  pthread_mutex_lock( &k->mu );
  if( k->tail ) k->tail->next = j; else k->head = j;
  k->tail = j;
  pthread_cond_signal( &k->cv );
  pthread_mutex_unlock( &k->mu );
  }

void asp_keeper_settle( AspKeeper *k, int64_t tag, const char *path )
  {
  Job *j = calloc( 1, sizeof *j );
  if( ! j ) return;
  j->tag = tag; snprintf( j->path, sizeof j->path, "%s", path );
  pthread_mutex_lock( &k->mu );
  if( k->tail ) k->tail->next = j; else k->head = j;
  k->tail = j;
  pthread_cond_signal( &k->cv );
  pthread_mutex_unlock( &k->mu );
  }

int asp_keeper_done( AspKeeper *k, int64_t *tags, int max )
  {
  int n;
  pthread_mutex_lock( &k->mu );
  n = k->ndone < max ? k->ndone : max;
  memcpy( tags, k->done, n * sizeof *tags );
  memmove( k->done, k->done + n, ( k->ndone - n ) * sizeof *tags );
  k->ndone -= n;
  pthread_mutex_unlock( &k->mu );
  return n;
  }

int asp_keeper_checkpoints( AspKeeper *k )
  { pthread_mutex_lock( &k->mu ); int n = k->checkpoints; pthread_mutex_unlock( &k->mu ); return n; }

void asp_keeper_stop( AspKeeper *k )
  {
  if( ! k ) return;
  pthread_mutex_lock( &k->mu ); k->stop = 1; pthread_cond_signal( &k->cv ); pthread_mutex_unlock( &k->mu );
  pthread_join( k->th, NULL );
  }

void asp_keeper_free( AspKeeper *k )
  { if( k ) { free( k->done ); free( k->ready ); free( k ); } }
