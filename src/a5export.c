/* a5export: take the audio out of a session as flac, whether or not it is running. it only reads the store; it never
   writes to the session, and it runs at the lowest cpu and disk priority so a running loop is not held up.
     a5export DIR [loop|mix|all] [--to FOLDER] [--cleanup] [--dry]
   what:  all    every take (the default), as it was recorded: no gain, no pan, muted ones too
          mix    only the takes that are in the loop
          loop   the loop as it sounds, one file, built from the store (never from the live loop file, so a mix change
                 that is still on its way is not caught half done)
   where: FOLDER (default DIR/export), brought up to date by every call: what has not changed is left alone, what is
          new or changed is rewritten. earlier loop files stay: each is a moment. nothing is deleted.
   --cleanup (mix only): takes that an earlier mix export wrote and that have left the loop since are moved to the system
          trash, or to DIR/trash if there is none to be had. a take that an "all" export also wrote is left alone.
   names: loop_ITER_OFF.flac for a take that is in the loop and mute_ITER_OFF.flac for one that is not (both numbers zero
          padded to 5 digits; a file changes its prefix when the take's state does), mix_ITER.flac for the loop at that
          iteration. generated tracks (clicks, imported files) are numbered in the 90000 series: loop_90001_00000.flac.
          no name from the session or its folder is used. a take whose two channels are identical is written as mono.
   each take is one file, padded with silence to the loop length exactly (starting where it starts in the loop), and a
   take longer than the loop to an exact multiple of it. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include "aspengine.h"
#include "asptank.h"
#include "flacio.h"

#define RATE 48000
#define CHUNK 65536
#define GEN_SERIES 90000        /* generated tracks (clicks, imported files) are numbered 90001, 90002 ... */

static int64_t json_num( const char *j, const char *key, int64_t d )
  {
  char k[64]; snprintf( k, sizeof k, "\"%s\":", key );
  const char *p = strstr( j, k );
  return p ? atoll( p + strlen( k ) ) : d;
  }

typedef struct { int64_t id, iter, off, frames; int ch, in; char sha[65], path[1300]; AspTakeState st; } Tk;
typedef struct { Tk *t; int n, cap; AspStore *s; int mixed_only; } Snap;

static void snap_cb( int64_t id, const char *json, void *u )
  {
  Snap *sn = u; AspTake k; AspTakeState st;
  if( ! id ) return;
  asp_parse_state( json, &st );
  int in = strcmp( st.status, "off" ) != 0;
  if( sn->mixed_only && ! in ) return;
  if( asp_take_get( sn->s, id, &k ) ) return;
  if( sn->n == sn->cap ) { sn->cap = sn->cap ? sn->cap * 2 : 64; sn->t = realloc( sn->t, sn->cap * sizeof *sn->t ); }
  Tk *t = sn->t + sn->n++;
  memset( t, 0, sizeof *t );
  t->id = id; t->iter = k.iter; t->off = k.off; t->frames = k.frames; t->ch = k.channels; t->in = in; t->st = st;
  snprintf( t->sha, sizeof t->sha, "%s", k.sha );
  asp_take_path( sn->s, &k, t->path, sizeof t->path );
  }

static int by_iter( const void *a, const void *b )
  {
  const Tk *x = a, *y = b;
  if( x->iter != y->iter ) return x->iter < y->iter ? -1 : 1;
  return x->off < y->off ? -1 : x->off > y->off;
  }

/* --- what is already there --- */
static uint64_t fnv( uint64_t h, const void *p, size_t n )
  { const unsigned char *c = p; while( n-- ) { h ^= *c++; h *= 1099511628211ULL; } return h; }

static int mkdir_p( const char *d )
  {
  char b[1300]; snprintf( b, sizeof b, "%s", d );
  for( char *p = b + 1; *p; p++ ) if( *p == '/' ) { *p = 0; mkdir( b, 0777 ); *p = '/'; }
  return mkdir( b, 0777 ) && errno != EEXIST ? -1 : 0;
  }

/* the system trash if gio can; otherwise DIR/trash. returns 0 when the file is out of the way */
static int to_trash( const char *file, const char *spaceroot )
  {
  pid_t pid = fork();
  if( pid == 0 )
    {
    int nul = open( "/dev/null", O_WRONLY ); if( nul >= 0 ) { dup2( nul, 1 ); dup2( nul, 2 ); }
    execlp( "gio", "gio", "trash", file, (char *)NULL );
    _exit( 127 );
    }
  int st = 0;
  if( pid > 0 && waitpid( pid, &st, 0 ) == pid && WIFEXITED( st ) && WEXITSTATUS( st ) == 0 ) return 0;
  char d[1400], b[1700]; const char *base = strrchr( file, '/' );
  snprintf( d, sizeof d, "%s/trash", spaceroot ); mkdir( d, 0777 );
  snprintf( b, sizeof b, "%s/%s", d, base ? base + 1 : file );
  for( int n = 1; ! access( b, F_OK ) && n < 1000; n++ ) snprintf( b, sizeof b, "%s/%s.%d", d, base ? base + 1 : file, n );
  return rename( file, b );
  }

typedef struct { char name[120]; uint64_t key; int modes; } Man;
static Man man[4096]; static int nman;


static void man_load( const char *dir )
  {
  char p[1400]; snprintf( p, sizeof p, "%s/.manifest", dir );
  FILE *f = fopen( p, "r" );
  if( ! f ) return;
  char l[200];
  while( nman < 4096 && fgets( l, sizeof l, f ) )
    { unsigned long long k; char n[120]; int m = 1; if( sscanf( l, "%llx %119s %d", &k, n, &m ) >= 2 ) { snprintf( man[nman].name, 120, "%s", n ); man[nman].modes = m; man[nman++].key = k; } }
  fclose( f );
  }

static int up_to_date( const char *dir, const char *name, uint64_t key )
  {
  struct stat sb; char p[1400];
  snprintf( p, sizeof p, "%s/%s", dir, name );
  if( stat( p, &sb ) ) return 0;
  for( int i = 0; i < nman; i++ ) if( ! strcmp( man[i].name, name ) ) return man[i].key == key;
  return 0;
  }

/* writing: to name.part, then renamed over the old one */
typedef struct { AspFlacEnc *e; char part[1400], final[1400]; } Out;
static int out_open( Out *o, const char *dir, const char *name, int ch )
  {
  snprintf( o->final, sizeof o->final, "%s/%s", dir, name );
  snprintf( o->part, sizeof o->part, "%s/%s.part", dir, name );
  o->e = asp_flac_enc_open( o->part, ch, RATE );
  return o->e ? 0 : -1;
  }
static int out_close( Out *o )
  {
  if( asp_flac_enc_close( o->e ) ) { unlink( o->part ); return -1; }
  return rename( o->part, o->final );
  }
static int silence( Out *o, int64_t frames, int ch )
  {
  static int32_t z[CHUNK * 2];
  for( ; frames > 0; frames -= CHUNK )
    if( asp_flac_enc_write( o->e, z, (unsigned)( frames < CHUNK ? frames : CHUNK ) ) ) return -1;
  (void)ch;
  return 0;
  }

typedef struct { Out *o; int och; int same; } Pass;
static int pass_cb( const int32_t *pcm, unsigned frames, int channels, void *u )
  {
  Pass *p = u;
  if( channels == p->och ) return asp_flac_enc_write( p->o->e, pcm, frames ) ? 1 : 0;
  static int32_t b[CHUNK * 2];
  for( unsigned i = 0; i < frames; )
    {
    unsigned n = frames - i < CHUNK ? frames - i : CHUNK;
    if( channels == 1 ) for( unsigned k = 0; k < n; k++ ) b[2*k] = b[2*k+1] = pcm[i+k];      /* mono into stereo */
    else for( unsigned k = 0; k < n; k++ ) b[k] = pcm[2*(i+k)];                               /* identical channels: one of them */
    if( asp_flac_enc_write( p->o->e, b, n ) ) return 1;
    i += n;
    }
  return 0;
  }

/* a stereo take whose channels are the same all the way through is a mono take */
static int same_cb( const int32_t *pcm, unsigned frames, int channels, void *u )
  {
  for( unsigned i = 0; channels == 2 && i < frames; i++ ) if( pcm[2*i] != pcm[2*i+1] ) { *(int *)u = 0; return 1; }
  return 0;
  }
static int is_mono( const Tk *t )
  {
  if( t->ch == 1 ) return 1;
  int same = 1;
  asp_flac_decode_frames( t->path, same_cb, &same );
  return same;
  }

static int64_t padded( int64_t end, int64_t loop ) { return end <= 0 ? loop : ( ( end + loop - 1 ) / loop ) * loop; }

static int export_take( const char *dir, const char *name, const Tk *t, int64_t loop )
  {
  int och = is_mono( t ) ? 1 : 2;
  int64_t st = ( t->off * RATE ) % loop, total = padded( st + t->frames, loop );
  Out o; Pass p = { &o, och };
  if( out_open( &o, dir, name, och ) ) return -1;
  if( silence( &o, st, och ) || asp_flac_decode_frames( t->path, pass_cb, &p ) || silence( &o, total - st - t->frames, och ) )
    { asp_flac_enc_close( o.e ); unlink( o.part ); return -1; }
  return out_close( &o );
  }

static int export_loop( const char *dir, const char *name, const char *tmp, AspStore *s, int64_t loop, int64_t seq )
  {
  char err[ASP_ERR], tp[1400];
  snprintf( tp, sizeof tp, "%s/.loop.tmp", tmp );
  unlink( tp );
  AspTank *t = asp_tank_create( tp, loop, RATE, err, sizeof err );
  if( ! t ) { fprintf( stderr, "a5export: %s\n", err ); return -1; }
  int rc = asp_rebuild( t, s, seq );
  Out o;
  if( ! rc && out_open( &o, dir, name, 2 ) ) rc = -1;
  static int64_t mix[CHUNK * 2]; static int32_t pcm[CHUNK * 2];
  for( int64_t p = 0; ! rc && p < loop; p += CHUNK )
    {
    int64_t n = loop - p < CHUNK ? loop - p : CHUNK;
    if( asp_tank_get( t, p, n, mix ) ) { rc = -1; break; }
    for( int64_t i = 0; i < n * 2; i++ ) pcm[i] = (int32_t)( mix[i] > 2147483647LL ? 2147483647LL : mix[i] < -2147483647LL ? -2147483647LL : mix[i] );
    if( asp_flac_enc_write( o.e, pcm, (unsigned)n ) ) rc = -1;
    }
  asp_tank_close( t ); unlink( tp );
  if( rc ) { if( o.e ) { asp_flac_enc_close( o.e ); unlink( o.part ); } return -1; }
  return out_close( &o );
  }

int main( int argc, char **argv )
  {
  const char *dir = NULL, *to = NULL, *what = NULL; int dry = 0, cleanup = 0;
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--to" ) && i + 1 < argc ) to = argv[++i];
    else if( ! strcmp( argv[i], "--dry" ) ) dry = 1;
    else if( ! strcmp( argv[i], "--cleanup" ) ) cleanup = 1;
    else if( ! strcmp( argv[i], "loop" ) || ! strcmp( argv[i], "mix" ) || ! strcmp( argv[i], "all" ) ) what = argv[i];
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { fprintf( stderr, "a5export: what is %s?\n", argv[i] ); return 2; }
    }
  if( ! what ) dir = NULL;                                        /* the set has to be asked for: no usage, no export */
  if( cleanup && what && strcmp( what, "mix" ) ) { fprintf( stderr, "a5export: --cleanup is for the mix export (a5export DIR mix --cleanup)\n" ); return 2; }
  if( ! dir ) { fprintf( stderr, "usage: a5export DIR loop|mix|all [--to FOLDER] [--cleanup] [--dry]\n"
    "  all   every take, as recorded, one flac each: loop_ITER_OFF.flac, or mute_ when it is out of the loop\n"
    "  mix   only the takes that are in the loop\n"
    "  loop  the loop as it sounds: mix_ITER.flac\n"
    "  --cleanup (mix only) moves takes that have left the loop to the trash;  --dry lists files without writing\n"
    "  files go to DIR/export unless --to says otherwise\n" ); return 2; }
  if( nice( 19 ) < 0 ) {}                                                     /* lowest cpu priority, and idle class for the disk */
#ifdef SYS_ioprio_set
  syscall( SYS_ioprio_set, 1, 0, ( 3 << 13 ) );
#endif
  char err[ASP_ERR], out[1300], tmp[1400];
  AspStore *s = asp_open( dir, 1, err );
  if( ! s ) { fprintf( stderr, "a5export: %s\n", err ); return 1; }
  int64_t loop = json_num( asp_descriptor( s ), "loop", 0 );
  if( ! loop ) { fprintf( stderr, "a5export: no loop length in the session\n" ); return 1; }
  snprintf( out, sizeof out, "%s", to ? to : "" );
  if( ! to ) snprintf( out, sizeof out, "%s/export", dir );
  snprintf( tmp, sizeof tmp, "%s", out );
  int64_t seq = asp_log_last( s );                                 /* one moment for everything that follows */
  int loopmode = ! strcmp( what, "loop" ), mode = loopmode ? 4 : ! strcmp( what, "mix" ) ? 2 : 1;
  Snap sn = { NULL, 0, 0, s, mode == 2 };
  asp_fold( s, seq, snap_cb, &sn );
  qsort( sn.t, sn.n, sizeof *sn.t, by_iter );
  int64_t cur = 0;                                                 /* the iteration the loop is at: counted only when a take was recorded */
  for( int i = 0; i < sn.n; i++ ) if( sn.t[i].iter > cur ) cur = sn.t[i].iter;

  /* the list of files and a key for each: when the key is the same as last time the file is left alone */
  typedef struct { char name[120]; uint64_t key; int t; } Job;
  Job *jobs = calloc( 4096, sizeof *jobs ); int nj = 0;
  if( loopmode )
    {
    uint64_t h = 1469598103934665603ULL; h = fnv( h, &loop, sizeof loop );
    for( int i = 0; i < sn.n; i++ )
      {
      Tk *t = sn.t + i; if( ! t->in ) continue;
      h = fnv( h, t->sha, 64 ); h = fnv( h, &t->st.l, sizeof t->st.l ); h = fnv( h, &t->st.r, sizeof t->st.r ); h = fnv( h, &t->st.flip, sizeof t->st.flip );
      h = fnv( h, &t->st.gdb, sizeof t->st.gdb ); h = fnv( h, &t->st.pan, sizeof t->st.pan ); h = fnv( h, &t->st.mixed, sizeof t->st.mixed ); h = fnv( h, &t->off, sizeof t->off );
      }
    snprintf( jobs[nj].name, 120, "mix_%05lld.flac", (long long)cur ); jobs[nj++].key = h;
    }
  else
    for( int i = 0; i < sn.n && nj < 4096; i++ )
      {
      Tk *t = sn.t + i;
      uint64_t h = 1469598103934665603ULL; h = fnv( h, &loop, sizeof loop ); h = fnv( h, t->sha, 64 ); h = fnv( h, &t->off, sizeof t->off );
      snprintf( jobs[nj].name, 120, "%s_%05lld_%05lld.flac", t->in ? "loop" : "mute", (long long)( t->iter >= 0 ? t->iter : GEN_SERIES - t->iter ), (long long)t->off );
      jobs[nj].key = h; jobs[nj].t = i; nj++;
      }
  if( dry ) { for( int i = 0; i < nj; i++ ) printf( "%s\n", jobs[i].name ); return 0; }
  if( mkdir_p( out ) ) { fprintf( stderr, "a5export: cannot make %s\n", out ); return 1; }
  man_load( out );
  int written = 0, kept = 0, fail = 0, moved = 0, renamed = 0;
  for( int i = 0; i < nj; i++ )
    {
    char alt[120] = ""; int af = -1;                              /* the same take under its other prefix: a state change is a rename */
    if( ! loopmode )
      {
      snprintf( alt, sizeof alt, "%s%s", ! strncmp( jobs[i].name, "loop_", 5 ) ? "mute_" : "loop_", jobs[i].name + 5 );
      for( int k = 0; k < nman; k++ ) if( ! strcmp( man[k].name, alt ) && man[k].key == jobs[i].key ) af = k;
      }
    if( up_to_date( out, jobs[i].name, jobs[i].key ) ) kept++;
    else if( af >= 0 && ! up_to_date( out, jobs[i].name, jobs[i].key ) )
      {
      char a[1400], b[1400]; snprintf( a, sizeof a, "%s/%s", out, alt ); snprintf( b, sizeof b, "%s/%s", out, jobs[i].name );
      if( ! rename( a, b ) ) { snprintf( man[af].name, 120, "%s", jobs[i].name ); renamed++; }
      else af = -1;
      }
    if( af < 0 && ! up_to_date( out, jobs[i].name, jobs[i].key ) )
      {
      int rc = loopmode ? export_loop( out, jobs[i].name, tmp, s, loop, seq ) : export_take( out, jobs[i].name, sn.t + jobs[i].t, loop );
      if( rc ) { fprintf( stderr, "a5export: could not write %s\n", jobs[i].name ); fail++; jobs[i].key = 0; continue; }
      written++;
      }
    int f = -1; for( int k = 0; k < nman; k++ ) if( ! strcmp( man[k].name, jobs[i].name ) ) f = k;
    if( f < 0 ) { f = nman++; snprintf( man[f].name, 120, "%s", jobs[i].name ); man[f].modes = 0; }
    man[f].key = jobs[i].key; man[f].modes |= mode;
    }
  /* --cleanup, mix only: a take this mode wrote before that has left the loop gives up the mix claim, and goes to the
     trash if nothing else (an "all" export) claims it. loop files are moments and stay. */
  if( cleanup )
    for( int k = 0; k < nman; k++ )
      {
      if( ! ( man[k].modes & mode ) ) continue;
      int keep = 0; for( int i = 0; i < nj; i++ ) if( ! strcmp( jobs[i].name, man[k].name ) ) keep = 1;
      if( keep ) continue;
      man[k].modes &= ~mode;
      if( man[k].modes & 1 ) continue;
      char a[1400]; snprintf( a, sizeof a, "%s/%s", out, man[k].name );
      if( ! to_trash( a, dir ) ) moved++;
      man[k].modes = 0;
      }
  { char p[1500]; snprintf( p, sizeof p, "%s/.manifest", out ); FILE *f = fopen( p, "w" );
    if( f ) { for( int k = 0; k < nman; k++ ) if( man[k].modes ) fprintf( f, "%llx %s %d\n", (unsigned long long)man[k].key, man[k].name, man[k].modes ); fclose( f ); } }
  printf( "%s: %d written, %d unchanged, %d renamed (loop_/mute_), in %s\n", what, written, kept, renamed, out );
  if( moved ) printf( "  %d file(s) that left the loop were moved to the trash\n", moved );
  asp_close( s ); free( sn.t ); free( jobs );
  return fail != 0;
  }
