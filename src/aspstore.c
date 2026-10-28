#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include "sqlite3.h"
#include "aspstore.h"
#include "codec.h"
#include "takefile.h"
#include "keeper.h"
#include "plat.h"
#include "schema_sql.h"

struct AspStore
  {
  sqlite3 *db;
  char dir[1024];
  char id[65];
  int readonly;
  unsigned char artist[ASP_ARTIST];      /* who is recording here: takes this store writes carry it */
  char artist_hex[2 * ASP_ARTIST + 1];
  AspKeeper *keeper;
  };

static void seterr( char *err, const char *what, const char *more )
  { if( err ) snprintf( err, ASP_ERR, "%s%s%s", what, more ? ": " : "", more ? more : "" ); }

static double now( void )
  { struct timespec t; clock_gettime( CLOCK_REALTIME, &t ); return t.tv_sec + t.tv_nsec / 1e9; }

static int run( AspStore *s, const char *sql )
  { return sqlite3_exec( s->db, sql, NULL, NULL, NULL ) == SQLITE_OK ? 0 : -1; }

static int mkdir_p( const char *path )
  {
  char tmp[1024], *p;
  snprintf( tmp, sizeof tmp, "%s", path );
  for( p = tmp + 1; *p; p++ )
    if( *p == '/' ) { *p = 0; mkdir( tmp, 0777 ); *p = '/'; }
  return mkdir( tmp, 0777 ) == 0 || errno == EEXIST ? 0 : -1;
  }

static double ms_now( void ) { struct timespec t; clock_gettime( CLOCK_MONOTONIC, &t ); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* the artist: 128 random bits, made the first time aspace runs and kept in $XDG_CONFIG_HOME/aspace/artist (or
   ~/.config/aspace/artist). without create, none made: -1 if there is not one yet */
int asp_artist_local( unsigned char id[ASP_ARTIST], int create )
  {
  char dir[1024], path[1100], line[64];
  const char *x = getenv( "XDG_CONFIG_HOME" ), *h = getenv( "HOME" );
  if( x && *x ) snprintf( dir, sizeof dir, "%s/aspace", x ); else if( h && *h ) snprintf( dir, sizeof dir, "%s/.config/aspace", h ); else return -1;
  snprintf( path, sizeof path, "%s/artist", dir );
  FILE *f = fopen( path, "r" );
  int got = 0;
  if( f )
    {
    if( fgets( line, sizeof line, f ) && strlen( line ) >= 2 * ASP_ARTIST )
      {
      got = 1;
      for( int i = 0; i < ASP_ARTIST && got; i++ )
        { unsigned v; if( sscanf( line + 2 * i, "%2x", &v ) != 1 ) got = 0; else id[i] = (unsigned char)v; }
      }
    fclose( f );
    }
  if( ! got && create )
    {
    FILE *u = fopen( "/dev/urandom", "rb" );
    if( ! u || fread( id, 1, ASP_ARTIST, u ) != ASP_ARTIST ) { if( u ) fclose( u ); return -1; }
    fclose( u );
    mkdir_p( dir );
    if( ! ( f = fopen( path, "w" ) ) ) return -1;
    for( int i = 0; i < ASP_ARTIST; i++ ) fprintf( f, "%02x", id[i] );
    fprintf( f, "\n" );
    fclose( f );
    got = 1;
    }
  return got ? 0 : -1;
  }

static void load_artist( AspStore *s, int create )
  {
  asp_artist_local( s->artist, create );                 /* zero until there is one */
  for( int i = 0; i < ASP_ARTIST; i++ ) sprintf( s->artist_hex + 2 * i, "%02x", s->artist[i] );
  }

static AspStore *opendb( const char *dir, int flags, char *err )
  {
  AspStore *s = calloc( 1, sizeof *s );
  char path[1100];
  snprintf( s->dir, sizeof s->dir, "%s", dir );
  snprintf( path, sizeof path, "%s/session.db", dir );
  s->readonly = ( flags & SQLITE_OPEN_READONLY ) != 0;
  if( sqlite3_open_v2( path, &s->db, flags, NULL ) != SQLITE_OK )
    { seterr( err, "cannot open", sqlite3_errmsg( s->db ) ); sqlite3_close( s->db ); free( s ); return NULL; }
  sqlite3_busy_timeout( s->db, 5000 );
  /* the log is written from the tick loop and must never wait for the disk. NORMAL in WAL mode does not sync at a commit
     (a power cut can lose the last moments of log, never corrupt it); and the log is folded back into the database only
     here, at open and at close, not by a commit in the middle of a pass: a checkpoint on an sd card takes a second */
  if( ! s->readonly ) { sqlite3_exec( s->db, "pragma synchronous=normal; pragma wal_autocheckpoint=0", NULL, NULL, NULL ); sqlite3_wal_checkpoint_v2( s->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL ); }
  load_artist( s, ! s->readonly );
  return s;
  }

AspStore *asp_create( const char *dir, const char *descriptor_json, char err[ASP_ERR] )
  {
  char sub[1100];
  struct stat st;
  if( ! stat( dir, &st ) )
    {
    char cmd[1100]; (void)cmd;
    if( ! S_ISDIR( st.st_mode ) ) { seterr( err, "not a directory", dir ); return NULL; }
    snprintf( sub, sizeof sub, "%s/session.db", dir );
    if( ! stat( sub, &st ) ) { seterr( err, "session already exists", dir ); return NULL; }
    }
  snprintf( sub, sizeof sub, "%s/tmp", dir );  if( mkdir_p( sub ) ) { seterr( err, "mkdir", sub ); return NULL; }
  snprintf( sub, sizeof sub, "%s/cache", dir );  mkdir_p( sub );
  AspStore *s = opendb( dir, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, err );
  if( ! s ) return NULL;
  char *msg = NULL;
  if( sqlite3_exec( s->db, SCHEMA_SQL, NULL, NULL, &msg ) != SQLITE_OK )
    { seterr( err, "schema", msg ); sqlite3_free( msg ); asp_close( s ); return NULL; }
  asp_sha256_buf( descriptor_json, strlen( descriptor_json ), s->id );
  sqlite3_stmt *q;
  sqlite3_prepare_v2( s->db, "insert into session values(?,?,2,?)", -1, &q, NULL );
  sqlite3_bind_text( q, 1, s->id, -1, SQLITE_STATIC );
  sqlite3_bind_text( q, 2, descriptor_json, -1, SQLITE_STATIC );
  sqlite3_bind_double( q, 3, now() );
  int rc = sqlite3_step( q );
  sqlite3_finalize( q );
  if( rc != SQLITE_DONE ) { seterr( err, "session row", sqlite3_errmsg( s->db ) ); asp_close( s ); return NULL; }
  asp_log( s, "store", "session_created", "{}", 0 );
  return s;
  }

AspStore *asp_open( const char *dir, int readonly, char err[ASP_ERR] )
  {
  AspStore *s = opendb( dir, readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, err );
  if( ! s ) return NULL;
  sqlite3_stmt *q;
  if( sqlite3_prepare_v2( s->db, "select id from session limit 1", -1, &q, NULL ) != SQLITE_OK
      || sqlite3_step( q ) != SQLITE_ROW )
    { seterr( err, "not a session", dir ); if( q ) sqlite3_finalize( q ); asp_close( s ); return NULL; }
  snprintf( s->id, sizeof s->id, "%s", (const char *)sqlite3_column_text( q, 0 ) );
  sqlite3_finalize( q );
  if( sqlite3_prepare_v2( s->db, "select schema_version from session limit 1", -1, &q, NULL ) != SQLITE_OK
      || sqlite3_step( q ) != SQLITE_ROW || sqlite3_column_int( q, 0 ) != 2 )
    { seterr( err, "a session of an older aspace (takes are kept in another way now)", dir ); if( q ) sqlite3_finalize( q ); asp_close( s ); return NULL; }
  sqlite3_finalize( q );
  return s;
  }

void asp_close( AspStore *s )
  {
  if( ! s ) return;
  asp_keeper_stop( s->keeper );                                    /* lets it finish what it was given */
  if( ! s->readonly ) sqlite3_wal_checkpoint_v2( s->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL );
  sqlite3_close( s->db );
  free( s );
  }
const char *asp_session_id( AspStore *s ) { return s->id; }
int asp_store_keeper( AspStore *s )
  {
  char db[1100];
  if( s->readonly || s->keeper ) return -1;
  snprintf( db, sizeof db, "%s/session.db", s->dir );
  s->keeper = asp_keeper_start( db, 30000 );
  return s->keeper ? 0 : -1;
  }

int asp_store_poll( AspStore *s )
  {
  int64_t tag[16]; int n, total = 0;
  if( ! s->keeper ) return 0;
  while( ( n = asp_keeper_done( s->keeper, tag, 16 ) ) > 0 )
    for( int i = 0; i < n; i++ )
      {
      char args[60]; snprintf( args, sizeof args, "{\"take\":%lld}", (long long)tag[i] );
      asp_log( s, "store", "take_durable", args, -1 );
      total++;
      }
  return total;
  }

const char *asp_dir( AspStore *s ) { return s->dir; }
const char *asp_artist( AspStore *s ) { return s->artist_hex; }
const unsigned char *asp_artist_id( AspStore *s ) { return s->artist; }

int64_t asp_log( AspStore *s, const char *actor, const char *type, const char *args_json, int64_t tick )
  {
  sqlite3_stmt *q;
  if( sqlite3_prepare_v2( s->db, "insert into log(tick,wall,actor,type,args_json) values(?,?,?,?,?)", -1, &q, NULL ) )
    return -1;
  if( tick < 0 ) sqlite3_bind_null( q, 1 ); else sqlite3_bind_int64( q, 1, tick );
  sqlite3_bind_double( q, 2, now() );
  sqlite3_bind_text( q, 3, actor, -1, SQLITE_STATIC );
  sqlite3_bind_text( q, 4, type, -1, SQLITE_STATIC );
  sqlite3_bind_text( q, 5, args_json ? args_json : "{}", -1, SQLITE_STATIC );
  int rc = sqlite3_step( q );
  sqlite3_finalize( q );
  return rc == SQLITE_DONE ? sqlite3_last_insert_rowid( s->db ) : -1;
  }

int64_t asp_log_last( AspStore *s )
  {
  sqlite3_stmt *q;
  int64_t v = -1;
  sqlite3_prepare_v2( s->db, "select coalesce(max(seq),0) from log", -1, &q, NULL );
  if( sqlite3_step( q ) == SQLITE_ROW ) v = sqlite3_column_int64( q, 0 );
  sqlite3_finalize( q );
  return v;
  }

#define TAKE_COLS "id,iter,off,artist,frames,rate,channels,bits,status"
static void take_row( sqlite3_stmt *q, AspTake *t )
  {
  t->id = sqlite3_column_int64( q, 0 );
  t->iter = sqlite3_column_int64( q, 1 );
  t->off = sqlite3_column_int64( q, 2 );
  snprintf( t->artist, sizeof t->artist, "%s", sqlite3_column_text( q, 3 ) );
  t->frames = sqlite3_column_int64( q, 4 );
  t->rate = sqlite3_column_int( q, 5 );
  t->channels = sqlite3_column_int( q, 6 );
  t->bits = sqlite3_column_int( q, 7 );
  snprintf( t->status, sizeof t->status, "%s", sqlite3_column_text( q, 8 ) );
  }

int asp_take_get( AspStore *s, int64_t id, AspTake *out )
  {
  sqlite3_stmt *q;
  int rc = -1;
  sqlite3_prepare_v2( s->db, "select " TAKE_COLS " from takes where id=?", -1, &q, NULL );
  sqlite3_bind_int64( q, 1, id );
  if( sqlite3_step( q ) == SQLITE_ROW ) { take_row( q, out ); rc = 0; }
  sqlite3_finalize( q );
  return rc;
  }

int asp_take_find( AspStore *s, int64_t iter, int64_t off, AspTake *out )
  {
  sqlite3_stmt *q;
  int rc = -1;
  sqlite3_prepare_v2( s->db, "select " TAKE_COLS " from takes where artist=? and iter=? and off=?", -1, &q, NULL );
  sqlite3_bind_text( q, 1, s->artist_hex, -1, SQLITE_STATIC );
  sqlite3_bind_int64( q, 2, iter );
  sqlite3_bind_int64( q, 3, off );
  if( sqlite3_step( q ) == SQLITE_ROW ) { take_row( q, out ); rc = 0; }
  sqlite3_finalize( q );
  return rc;
  }

/* takes.<first 12 of the artist>/<iter>/<off>.a5t: the folder names the artist, the file name has no prefix; the header has all of it */
int asp_take_path( AspStore *s, const AspTake *t, char *buf, size_t n )
  { return snprintf( buf, n, "%s/takes.%.12s/%lld/%lld.a5t", s->dir, t->artist, (long long)t->iter, (long long)t->off ) < (int)n ? 0 : -1; }

static int to_d32( const int32_t *pcm, unsigned frames, int ch, void *u )
  {
  FILE *f = u;
  unsigned char b[4 * 4096];
  size_t total = (size_t)frames * ch, n = 0;
  while( n < total )
    {
    size_t k = total - n < 4096 ? total - n : 4096;
    for( size_t i = 0; i < k; i++ ) asp_st32( b + 4 * i, (uint32_t)pcm[n + i] );        /* little endian on disk whatever the host */
    if( fwrite( b, 4, k, f ) != k ) return -1;
    n += k;
    }
  return 0;
  }

int asp_take_export_d32( AspStore *s, int64_t id, const char *out_path )
  {
  AspTake t;
  char p[1200];
  if( asp_take_get( s, id, &t ) || asp_take_path( s, &t, p, sizeof p ) ) return -1;
  FILE *f = strcmp( out_path, "-" ) ? fopen( out_path, "wb" ) : stdout;
  if( ! f ) return -1;
  int rc = asp_take_frames( p, -1, to_d32, f );
  if( f == stdout ) fflush( stdout ); else if( fclose( f ) ) rc = -1;
  return rc;
  }

int64_t asp_take_set_state( AspStore *s, int64_t id, const char *state_json, const char *actor, int64_t tick )
  {
  sqlite3_stmt *q;
  int64_t seq = -1;
  sqlite3_prepare_v2( s->db,
    "insert into log(tick,wall,actor,type,args_json) values(?,?,?,'take_state',"
    "json_object('take',?,'state',json(?)))", -1, &q, NULL );
  if( tick < 0 ) sqlite3_bind_null( q, 1 ); else sqlite3_bind_int64( q, 1, tick );
  sqlite3_bind_double( q, 2, now() );
  sqlite3_bind_text( q, 3, actor, -1, SQLITE_STATIC );
  sqlite3_bind_int64( q, 4, id );
  sqlite3_bind_text( q, 5, state_json, -1, SQLITE_STATIC );
  if( sqlite3_step( q ) == SQLITE_DONE ) seq = sqlite3_last_insert_rowid( s->db );
  sqlite3_finalize( q );
  return seq;
  }

int asp_take_adopt( AspStore *s, int64_t iter, int64_t off, const char *part, const char *status,
                    const char *state_json, int64_t tick, AspTake *out )
  {
  char dst[1200], dir[1200], args[200], st[200];
  AspHdr h;
  AspTake dummy, t;
  if( s->readonly ) return -1;
  if( ! out ) out = &dummy;
  if( ! asp_take_find( s, iter, off, &dummy ) ) return -1;           /* takes are never overwritten */
  double t0 = ms_now(), tm[3];
  if( asp_take_header( part, &h ) || ! h.finished || memcmp( h.artist, s->artist, ASP_ARTIST ) ) return -1;
  memset( &t, 0, sizeof t );
  t.iter = iter; t.off = off; snprintf( t.artist, sizeof t.artist, "%s", s->artist_hex );
  if( asp_take_path( s, &t, dst, sizeof dst ) ) return -1;
  snprintf( dir, sizeof dir, "%s", dst ); *strrchr( dir, '/' ) = 0;
  if( mkdir_p( dir ) || ! access( dst, F_OK ) || rename( part, dst ) ) return -1;       /* the part was synced when it was closed */
  tm[0] = ms_now() - t0; t0 = ms_now();
  if( ! s->keeper ) asp_settle_now( dst );                           /* no servant: safe on the disk before there is a row */
  tm[1] = ms_now() - t0; t0 = ms_now();

  if( run( s, "begin immediate" ) ) return -1;
  sqlite3_stmt *q;
  sqlite3_prepare_v2( s->db,
    "insert into takes(artist,iter,off,frames,rate,channels,bits,status,t_start) values(?,?,?,?,?,?,?,?,?)",
    -1, &q, NULL );
  sqlite3_bind_text( q, 1, s->artist_hex, -1, SQLITE_STATIC );
  sqlite3_bind_int64( q, 2, iter );
  sqlite3_bind_int64( q, 3, off );
  sqlite3_bind_int64( q, 4, (int64_t)h.frames );
  sqlite3_bind_int( q, 5, h.rate );
  sqlite3_bind_int( q, 6, h.channels );
  sqlite3_bind_int( q, 7, h.bits );
  sqlite3_bind_text( q, 8, status, -1, SQLITE_STATIC );
  sqlite3_bind_double( q, 9, now() );
  int rc = sqlite3_step( q );
  sqlite3_finalize( q );
  if( rc != SQLITE_DONE ) { run( s, "rollback" ); return -1; }
  int64_t id = sqlite3_last_insert_rowid( s->db );
  snprintf( args, sizeof args, "{\"take\":%lld,\"iter\":%lld,\"off\":%lld}", (long long)id, (long long)iter, (long long)off );
  asp_log( s, "aspace", "take_added", args, tick );
  if( ! state_json ) { snprintf( st, sizeof st, "{\"status\":\"%s\"}", status ); state_json = st; }
  asp_take_set_state( s, id, state_json, "aspace", tick );
  if( run( s, "commit" ) ) { run( s, "rollback" ); return -1; }
  if( s->keeper ) asp_keeper_settle( s->keeper, id, dst );            /* the row is there; the keeper makes the file safe, and says when */
  tm[2] = ms_now() - t0;
  if( tm[0] + tm[1] + tm[2] > 20 )                                    /* where a slow adopt went, ms */
    {
    snprintf( args, sizeof args, "{\"rename\":%.0f,\"settle\":%.0f,\"sql\":%.0f}", tm[0], tm[1], tm[2] );
    asp_log( s, "aspace", "adopt_slow", args, tick );
    }
  return asp_take_get( s, id, out );
  }

int asp_take_add_d32( AspStore *s, int64_t iter, int64_t off, const char *d32_path,
                      int channels, int rate, const char *status, int64_t tick, AspTake *out )
  {
  char part[1200];
  AspTake dummy;
  if( s->readonly ) return -1;
  if( ! asp_take_find( s, iter, off, &dummy ) ) return -1;
  if( asp_tmp_path( s, part, sizeof part ) ) return -1;          /* one counter for every temporary name, so it can never be the input's */
  FILE *f = fopen( d32_path, "rb" );
  AspTakeW *w = f ? asp_tw_open( part, s->artist, channels, rate, 0 ) : NULL;
  if( ! w ) { if( f ) fclose( f ); return -1; }
  int32_t buf[4096 * ASP_CODEC_MAXCH];
  unsigned char raw[sizeof buf];
  size_t got;
  int rc = 0;
  while( ! rc && ( got = fread( raw, 4 * (size_t)channels, 4096, f ) ) > 0 )
    {
    for( size_t i = 0; i < got * channels; i++ ) buf[i] = (int32_t)asp_ld32( raw + 4 * i );   /* d32 is little endian on disk */
    rc = asp_tw_put( w, buf, (unsigned)got );
    }
  fclose( f );
  if( asp_tw_close( w, 0, NULL ) || rc || asp_take_adopt( s, iter, off, part, status, NULL, tick, out ) ) { unlink( part ); return -1; }
  return 0;
  }

int asp_tmp_path( AspStore *s, char *buf, size_t n )
  {
  static int serial;
  char dir[1200];
  snprintf( dir, sizeof dir, "%s/tmp", s->dir );
  mkdir_p( dir );
  return snprintf( buf, n, "%s/%d_%d.part", dir, (int)getpid(), serial++ ) < (int)n ? 0 : -1;
  }

int asp_mix_name( AspStore *s, const char *name, int64_t seq )
  {
  sqlite3_stmt *q;
  sqlite3_prepare_v2( s->db, "insert or replace into mixes values(?,?)", -1, &q, NULL );
  sqlite3_bind_text( q, 1, name, -1, SQLITE_STATIC );
  sqlite3_bind_int64( q, 2, seq );
  int rc = sqlite3_step( q );
  sqlite3_finalize( q );
  if( rc != SQLITE_DONE ) return -1;
  char args[300];
  snprintf( args, sizeof args, "{\"name\":\"%.200s\",\"seq\":%lld}", name, (long long)seq );
  return asp_log( s, "aspace", "mix_named", args, -1 ) < 0 ? -1 : 0;
  }

int64_t asp_mix_seq( AspStore *s, const char *name )
  {
  sqlite3_stmt *q;
  int64_t v = -1;
  sqlite3_prepare_v2( s->db, "select seq from mixes where name=?", -1, &q, NULL );
  sqlite3_bind_text( q, 1, name, -1, SQLITE_STATIC );
  if( sqlite3_step( q ) == SQLITE_ROW ) v = sqlite3_column_int64( q, 0 );
  sqlite3_finalize( q );
  return v;
  }

/* --- replay --- */
typedef struct { int64_t id; char *state; } Ent;
typedef struct { Ent *e; int n, cap; } Map;

static void map_set( Map *m, int64_t id, const char *state )
  {
  for( int i = 0; i < m->n; i++ )
    if( m->e[i].id == id ) { free( m->e[i].state ); m->e[i].state = strdup( state ); return; }
  if( m->n == m->cap ) { m->cap = m->cap ? m->cap * 2 : 64; m->e = realloc( m->e, m->cap * sizeof *m->e ); }
  m->e[m->n].id = id; m->e[m->n].state = strdup( state ); m->n++;
  }
static void map_clear( Map *m )
  { for( int i = 0; i < m->n; i++ ) free( m->e[i].state ); m->n = 0; }

static int fold_into( AspStore *s, int64_t upto, Map *m, int depth )
  {
  sqlite3_stmt *q;
  if( depth > 1000 ) return -1;
  if( sqlite3_prepare_v2( s->db,
      "select seq,type,json_extract(args_json,'$.take'),json_extract(args_json,'$.state'),"
      "json_extract(args_json,'$.to') from log where seq<=? and type in ('take_state','revert') order by seq",
      -1, &q, NULL ) ) return -1;
  sqlite3_bind_int64( q, 1, upto );
  int rc = 0;
  while( sqlite3_step( q ) == SQLITE_ROW )
    {
    int64_t seq = sqlite3_column_int64( q, 0 );
    if( ! strcmp( (const char *)sqlite3_column_text( q, 1 ), "revert" ) )
      {
      int64_t to = sqlite3_column_int64( q, 4 );
      if( to >= seq ) { rc = -1; break; }
      map_clear( m );
      if( fold_into( s, to, m, depth + 1 ) ) { rc = -1; break; }
      }
    else if( sqlite3_column_type( q, 3 ) != SQLITE_NULL )
      map_set( m, sqlite3_column_int64( q, 2 ), (const char *)sqlite3_column_text( q, 3 ) );
    }
  sqlite3_finalize( q );
  return rc;
  }

static int ent_cmp( const void *a, const void *b )
  { int64_t x = ((const Ent *)a)->id, y = ((const Ent *)b)->id; return x < y ? -1 : x > y; }

int asp_fold( AspStore *s, int64_t upto, AspStateCb cb, void *user )
  {
  Map m = { 0 };
  if( upto < 0 ) upto = asp_log_last( s );
  int rc = fold_into( s, upto, &m, 0 );
  if( ! rc )
    {
    qsort( m.e, m.n, sizeof *m.e, ent_cmp );
    for( int i = 0; i < m.n; i++ ) cb( m.e[i].id, m.e[i].state, user );
    }
  map_clear( &m );
  free( m.e );
  return rc;
  }

int64_t asp_revert( AspStore *s, int64_t to_seq, const char *actor )
  {
  char args[64];
  int64_t last = asp_log_last( s );
  if( to_seq < 0 || to_seq > last ) return -1;
  snprintf( args, sizeof args, "{\"to\":%lld}", (long long)to_seq );
  return asp_log( s, actor, "revert", args, -1 );
  }

int64_t asp_log_since_w( AspStore *s, int64_t after, AspLogCbW cb, void *user )
  {
  sqlite3_stmt *q;
  int64_t last = after;
  if( sqlite3_prepare_v2( s->db, "select seq,coalesce(tick,-1),wall,actor,type,args_json from log where seq>? order by seq", -1, &q, NULL ) ) return after;
  sqlite3_bind_int64( q, 1, after );
  while( sqlite3_step( q ) == SQLITE_ROW )
    {
    last = sqlite3_column_int64( q, 0 );
    cb( last, sqlite3_column_int64( q, 1 ), sqlite3_column_double( q, 2 ), (const char *)sqlite3_column_text( q, 3 ), (const char *)sqlite3_column_text( q, 4 ), (const char *)sqlite3_column_text( q, 5 ), user );
    }
  sqlite3_finalize( q );
  return last;
  }

const char *asp_descriptor( AspStore *s )
  {
  static char buf[2048];
  sqlite3_stmt *q;
  buf[0] = 0;
  sqlite3_prepare_v2( s->db, "select descriptor_json from session limit 1", -1, &q, NULL );
  if( sqlite3_step( q ) == SQLITE_ROW ) snprintf( buf, sizeof buf, "%s", (const char *)sqlite3_column_text( q, 0 ) );
  sqlite3_finalize( q );
  return buf;
  }

int64_t asp_log_since( AspStore *s, int64_t after, AspLogCb cb, void *user )
  {
  sqlite3_stmt *q;
  int64_t last = after;
  if( sqlite3_prepare_v2( s->db, "select seq,coalesce(tick,-1),wall,actor,type,args_json from log where seq>? order by seq", -1, &q, NULL ) )
    return after;
  sqlite3_bind_int64( q, 1, after );
  while( sqlite3_step( q ) == SQLITE_ROW )
    {
    last = sqlite3_column_int64( q, 0 );
    cb( last, sqlite3_column_int64( q, 1 ), (const char *)sqlite3_column_text( q, 3 ), (const char *)sqlite3_column_text( q, 4 ),
        (const char *)sqlite3_column_text( q, 5 ), user );
    }
  sqlite3_finalize( q );
  return last;
  }
