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
#include "flacio.h"
#include "schema_sql.h"

struct AspStore
  {
  sqlite3 *db;
  char dir[1024];
  char id[65];
  int readonly;
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

static void fsync_path( const char *path )
  {
  int fd = open( path, O_RDONLY );
  if( fd >= 0 ) { fsync( fd ); close( fd ); }
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
  snprintf( sub, sizeof sub, "%s/blobs", dir );  if( mkdir_p( sub ) ) { seterr( err, "mkdir", sub ); return NULL; }
  snprintf( sub, sizeof sub, "%s/cache", dir );  mkdir_p( sub );
  AspStore *s = opendb( dir, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, err );
  if( ! s ) return NULL;
  char *msg = NULL;
  if( sqlite3_exec( s->db, SCHEMA_SQL, NULL, NULL, &msg ) != SQLITE_OK )
    { seterr( err, "schema", msg ); sqlite3_free( msg ); asp_close( s ); return NULL; }
  asp_sha256_buf( descriptor_json, strlen( descriptor_json ), s->id );
  sqlite3_stmt *q;
  sqlite3_prepare_v2( s->db, "insert into session values(?,?,1,?)", -1, &q, NULL );
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
  return s;
  }

void asp_close( AspStore *s )
  { if( s ) { sqlite3_close( s->db ); free( s ); } }
const char *asp_session_id( AspStore *s ) { return s->id; }
const char *asp_dir( AspStore *s ) { return s->dir; }

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

#define TAKE_COLS "id,iter,off,blob_sha,frames,rate,channels,bits,status"
static void take_row( sqlite3_stmt *q, AspTake *t )
  {
  t->id = sqlite3_column_int64( q, 0 );
  t->iter = sqlite3_column_int64( q, 1 );
  t->off = sqlite3_column_int64( q, 2 );
  snprintf( t->sha, sizeof t->sha, "%s", sqlite3_column_text( q, 3 ) );
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
  sqlite3_prepare_v2( s->db, "select " TAKE_COLS " from takes where iter=? and off=?", -1, &q, NULL );
  sqlite3_bind_int64( q, 1, iter );
  sqlite3_bind_int64( q, 2, off );
  if( sqlite3_step( q ) == SQLITE_ROW ) { take_row( q, out ); rc = 0; }
  sqlite3_finalize( q );
  return rc;
  }

int asp_take_path( AspStore *s, const AspTake *t, char *buf, size_t n )
  { return snprintf( buf, n, "%s/blobs/%.2s/%s.flac", s->dir, t->sha, t->sha ) < (int)n ? 0 : -1; }

int asp_take_export_d32( AspStore *s, int64_t id, const char *out_path )
  {
  AspTake t;
  char p[1200];
  if( asp_take_get( s, id, &t ) || asp_take_path( s, &t, p, sizeof p ) ) return -1;
  return asp_flac_decode_file( p, out_path );
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
  char dst[1200], sha[65], dir[1200];
  unsigned char md5[16];
  uint64_t frames;
  int ch, rt, bits;
  AspTake dummy;
  if( s->readonly ) return -1;
  if( ! out ) out = &dummy;
  if( ! asp_take_find( s, iter, off, &dummy ) ) return -1;           /* takes are never overwritten */
  fsync_path( part );
  if( asp_sha256_file( part, sha ) || asp_flac_info( part, md5, &frames, &ch, &rt, &bits ) ) return -1;
  snprintf( dir, sizeof dir, "%s/blobs/%.2s", s->dir, sha );
  mkdir_p( dir );
  snprintf( dst, sizeof dst, "%s/%s.flac", dir, sha );
  if( ! access( dst, F_OK ) ) unlink( part );                        /* identical blob already held */
  else if( rename( part, dst ) ) return -1;
  fsync_path( dir );

  char hex[33], args[200], st[200];
  for( int i = 0; i < 16; i++ ) sprintf( hex + 2 * i, "%02x", md5[i] );
  if( run( s, "begin immediate" ) ) return -1;
  sqlite3_stmt *q;
  sqlite3_prepare_v2( s->db,
    "insert into takes(iter,off,blob_sha,md5,frames,rate,channels,bits,status,t_start) values(?,?,?,?,?,?,?,?,?,?)",
    -1, &q, NULL );
  sqlite3_bind_int64( q, 1, iter );
  sqlite3_bind_int64( q, 2, off );
  sqlite3_bind_text( q, 3, sha, -1, SQLITE_STATIC );
  sqlite3_bind_text( q, 4, hex, -1, SQLITE_STATIC );
  sqlite3_bind_int64( q, 5, (int64_t)frames );
  sqlite3_bind_int( q, 6, rt );
  sqlite3_bind_int( q, 7, ch );
  sqlite3_bind_int( q, 8, bits );
  sqlite3_bind_text( q, 9, status, -1, SQLITE_STATIC );
  sqlite3_bind_double( q, 10, now() );
  int rc = sqlite3_step( q );
  sqlite3_finalize( q );
  if( rc != SQLITE_DONE ) { run( s, "rollback" ); return -1; }
  int64_t id = sqlite3_last_insert_rowid( s->db );
  snprintf( args, sizeof args, "{\"take\":%lld,\"iter\":%lld,\"off\":%lld}", (long long)id, (long long)iter, (long long)off );
  asp_log( s, "aspace", "take_added", args, tick );
  if( ! state_json ) { snprintf( st, sizeof st, "{\"status\":\"%s\"}", status ); state_json = st; }
  asp_take_set_state( s, id, state_json, "aspace", tick );
  if( run( s, "commit" ) ) { run( s, "rollback" ); return -1; }
  return asp_take_get( s, id, out );
  }

int asp_take_add_d32( AspStore *s, int64_t iter, int64_t off, const char *d32_path,
                      int channels, int rate, const char *status, int64_t tick, AspTake *out )
  {
  char part[1200], dir[1200];
  static int serial;
  AspTake dummy;
  if( s->readonly ) return -1;
  if( ! asp_take_find( s, iter, off, &dummy ) ) return -1;
  snprintf( dir, sizeof dir, "%s/blobs/tmp", s->dir );
  mkdir_p( dir );
  snprintf( part, sizeof part, "%s/%d_%d.part", dir, (int)getpid(), serial++ );
  if( asp_flac_encode_file( d32_path, part, channels, rate ) ) { unlink( part ); return -1; }
  if( asp_take_adopt( s, iter, off, part, status, NULL, tick, out ) ) { unlink( part ); return -1; }
  return 0;
  }

int asp_tmp_path( AspStore *s, char *buf, size_t n )
  {
  static int serial;
  char dir[1200];
  snprintf( dir, sizeof dir, "%s/blobs/tmp", s->dir );
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
