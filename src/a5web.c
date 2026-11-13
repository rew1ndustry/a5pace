/* a5web DIR [--port N] [--www DIR]   the browser shell: a small http server on 127.0.0.1 between a browser and a running aspace.
   It keeps no state: every request opens its own control connection (ctlclient), asks, and answers, so the space and the browser
   cannot wedge each other, and a5hell and a5web run side by side. One child per connection (browsers open idle ones).
   Step 1 is read only: GET /api/state|line|meter|takes, /api/map?from=&to=, /api/maphigh?n=&above= return the control port's
   lines as text/plain. Only whitelisted file names are served (no paths from the request), and the Host header must be the
   loopback address, which shuts out other web pages (dns rebinding). Nothing here can change the space yet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ctlclient.h"
#include "a5mixer.h"

static const char *dir, *www;
static int quiet;
/* a little talk on stderr about what a5web does, unless --quiet; one write a line, so children do not interleave */
static void chat( const char *form, ... )
  {
  if( quiet ) return;
  char b[400]; va_list ap; int k = snprintf( b, sizeof b, "a5web: " );
  va_start( ap, form ); vsnprintf( b + k, sizeof b - (size_t)k - 2, form, ap ); va_end( ap );
  strcat( b, "\n" ); if( write( 2, b, strlen( b ) ) < 0 ) {}
  }
static int port = 5055;

/* what is served: the published name, the file, its type */
static const struct { const char *url, *file, *type; } FILES[] = {
  { "/",           "index.html", "text/html; charset=utf-8" },
  { "/index.html", "index.html", "text/html; charset=utf-8" },
  { "/a5web.js",   "a5web.js",   "text/javascript; charset=utf-8" },
};

static void out( int fd, int code, const char *type, const char *body, size_t n )
  {
  char h[300];
  const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 502 ? "Bad Gateway" : "Error";
  int k = snprintf( h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, msg, type, n );
  if( send( fd, h, k, MSG_NOSIGNAL ) < 0 ) return;
  while( n ) { ssize_t w = send( fd, body, n, MSG_NOSIGNAL ); if( w <= 0 ) return; body += w; n -= (size_t)w; }
  }
static void say( int fd, int code, const char *text ) { out( fd, code, "text/plain; charset=utf-8", text, strlen( text ) ); }

typedef struct { char *b; size_t n, cap; } Buf;
static void add( const char *l, void *u )
  {
  Buf *b = u; size_t k = strlen( l ) + 1;
  if( b->n + k + 1 > b->cap ) { b->cap = ( b->n + k + 1 ) * 2; b->b = realloc( b->b, b->cap ); if( ! b->b ) _exit( 1 ); }
  memcpy( b->b + b->n, l, k - 1 ); b->n += k; b->b[b->n - 1] = '\n'; b->b[b->n] = 0;
  }

/* the control command for a request, or 0. numbers only: a request cannot carry words of its own to the control port */
static int num( const char *q, const char *key, long *v )
  {
  size_t kl = strlen( key );
  for( const char *p = q; p && *p; p = strchr( p, '&' ) ? strchr( p, '&' ) + 1 : NULL )
    if( ! strncmp( p, key, kl ) && p[kl] == '=' )
      {
      char *e; long x = strtol( p + kl + 1, &e, 10 );
      if( e == p + kl + 1 || ( *e && *e != '&' ) || x < 0 || x > 99999999 ) return 0;
      *v = x; return 1;
      }
  return 0;
  }
static int command( const char *path, char *cmd, size_t n )
  {
  char p[400]; snprintf( p, sizeof p, "%s", path );
  char *q = strchr( p, '?' ); if( q ) *q++ = 0;
  long a, b;
  if( ! strcmp( p, "/api/state" ) ) snprintf( cmd, n, "STATE" );
  else if( ! strcmp( p, "/api/line" ) ) snprintf( cmd, n, "LINE" );
  else if( ! strcmp( p, "/api/meter" ) ) snprintf( cmd, n, "METER" );
  else if( ! strcmp( p, "/api/takes" ) ) snprintf( cmd, n, "TAKES" );
  else if( ! strcmp( p, "/api/grid" ) ) snprintf( cmd, n, "GRID" );
  else if( ! strcmp( p, "/api/history" ) ) snprintf( cmd, n, "HISTORY 30" );
  else if( ! strcmp( p, "/api/map" ) )
    {
    if( q && num( q, "from", &a ) && num( q, "to", &b ) ) snprintf( cmd, n, "MAP %ld %ld", a, b );
    else if( q && num( q, "from", &a ) ) snprintf( cmd, n, "MAP %ld", a );
    else snprintf( cmd, n, "MAP" );
    }
  else if( ! strcmp( p, "/api/maphigh" ) && q && num( q, "n", &a ) && num( q, "above", &b ) ) snprintf( cmd, n, "MAP HIGH %ld %ld", a, b );
  else return 0;
  return 1;
  }

static void serve_file( int fd, int i )
  {
  char p[1200]; snprintf( p, sizeof p, "%s/%s", www, FILES[i].file );
  FILE *f = fopen( p, "rb" );
  if( ! f ) { say( fd, 404, "page file missing: install the web files (make install) or use --www DIR\n" ); return; }
  Buf b = { 0 }; char c[4096]; size_t k;
  b.cap = 8192; b.b = malloc( b.cap );
  while( ( k = fread( c, 1, sizeof c, f ) ) > 0 )
    { if( b.n + k > b.cap ) { b.cap = ( b.n + k ) * 2; b.b = realloc( b.b, b.cap ); } memcpy( b.b + b.n, c, k ); b.n += k; }
  fclose( f );
  out( fd, 200, FILES[i].type, b.b, b.n );
  free( b.b );
  }

/* the thresholds a5hell uses ($A5HELL_CFG or ~/.local/etc/a5hell.cfg), as it has them: only the two numbers the page needs, else the defaults */
/* the folder's own name, for the window title when the space has no name of its own */
static void serve_dir( int fd )
  {
  char r[PATH_MAX], o[PATH_MAX + 2]; const char *b;
  if( ! realpath( dir, r ) ) snprintf( r, sizeof r, "%s", dir );
  b = strrchr( r, '/' ); b = b && b[1] ? b + 1 : r;
  snprintf( o, sizeof o, "%s\n", b ); out( fd, 200, "text/plain; charset=utf-8", o, strlen( o ) );
  }

static void serve_cfg( int fd )
  {
  char p[1200], l[200], o[100]; double g = -24, r = -6;
  const char *e = getenv( "A5HELL_CFG" ), *h = getenv( "HOME" );
  if( e ) snprintf( p, sizeof p, "%s", e ); else snprintf( p, sizeof p, "%s/.local/etc/a5hell.cfg", h ? h : "." );
  FILE *f = fopen( p, "r" );
  if( f )
    {
    while( fgets( l, sizeof l, f ) )
      { double v; if( sscanf( l, "green_below_db %lf", &v ) == 1 ) g = v; else if( sscanf( l, "red_above_db %lf", &v ) == 1 ) r = v; }
    fclose( f );
    }
  snprintf( o, sizeof o, "green_below_db %g\nred_above_db %g\n", g, r );
  say( fd, 200, o );
  }

/* one control command, relayed: its lines back as text/plain; a failure of the space is 500, no space 502 */
static int relay( int fd, const char *cmd )                /* 1 if the space answered ok */
  {
  int c = ctlc_connect( dir );
  if( c < 0 ) { say( fd, 502, "the space is not running\n" ); return 0; }
  Buf b = { 0 };
  int rc = ctlc_cmd( c, cmd, add, &b );
  close( c );
  if( rc < 0 ) say( fd, 502, "the space did not answer\n" );
  else out( fd, rc ? 500 : 200, "text/plain; charset=utf-8", b.b ? b.b : "", b.n );
  free( b.b );
  return rc == 0;
  }

/* the events of the space as server-sent events: the control port's WATCH, a line an event; a comment every 15 s finds a client that went */
static void watch( int fd )
  {
  const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  int c = ctlc_connect( dir );
  if( c < 0 ) { say( fd, 502, "the space is not running\n" ); return; }
  if( ctlc_cmd( c, "WATCH", NULL, NULL ) ) { close( c ); say( fd, 502, "the space did not answer\n" ); return; }
  alarm( 0 );
  if( send( fd, h, strlen( h ), MSG_NOSIGNAL ) < 0 ) { close( c ); return; }
  char b[1300], o[1400];
  for( ;; )
    {
    int r = ctlc_readline( c, b, sizeof b, 15000 );
    if( r < 0 ) break;
    int k = r ? snprintf( o, sizeof o, "data: %s\n\n", b ) : snprintf( o, sizeof o, ": still here\n\n" );
    if( send( fd, o, (size_t)k, MSG_NOSIGNAL ) < 0 ) break;
    }
  close( c );
  }

/* query values: percent-decoded into v, up to n-1 bytes; 0 if the key is absent */
static int qstr( const char *q, const char *key, char *v, size_t n )
  {
  size_t kl = strlen( key );
  for( const char *p = q; p && *p; p = strchr( p, '&' ) ? strchr( p, '&' ) + 1 : NULL )
    if( ! strncmp( p, key, kl ) && p[kl] == '=' )
      {
      size_t i = 0;
      for( p += kl + 1; *p && *p != '&' && i < n - 1; p++ )
        {
        if( *p == '%' && isxdigit( (unsigned char)p[1] ) && isxdigit( (unsigned char)p[2] ) ) { char h[3] = { p[1], p[2], 0 }; v[i++] = (char)strtol( h, NULL, 16 ); p += 2; }
        else v[i++] = *p == '+' ? ' ' : *p;
        }
      v[i] = 0; return 1;
      }
  return 0;
  }

/* the writes. none passes a word of the request on: each is one of these and numbers. REVERT, EDGE, EVENT, CLICK, SIM are not here */
static void post( int fd, const char *path )
  {
  char p[400], v[100], w[100], cmd[100]; snprintf( p, sizeof p, "%s", path );
  char *q = strchr( p, '?' ); if( q ) *q++ = 0; else q = p + strlen( p );
  long n;
  if( ! strcmp( p, "/api/do" ) )
    {
    if( ! qstr( q, "c", v, sizeof v ) ) { say( fd, 400, "no action\n" ); return; }
    if( ! strcmp( v, "live" ) ) snprintf( cmd, sizeof cmd, "LIVE" );
    else if( ! strcmp( v, "safe" ) ) snprintf( cmd, sizeof cmd, "SAFE" );
    else if( ! strcmp( v, "sync" ) ) snprintf( cmd, sizeof cmd, "SYNC" );
    else if( ! strcmp( v, "undotake" ) ) snprintf( cmd, sizeof cmd, "UNDO TAKE" );
    else if( ! strcmp( v, "undoiter" ) ) snprintf( cmd, sizeof cmd, "UNDO ITER" );
    else if( ! strcmp( v, "relearn" ) ) snprintf( cmd, sizeof cmd, "RELEARN" );
    else if( ! strcmp( v, "gate_more" ) ) snprintf( cmd, sizeof cmd, "GATE -3" );
    else if( ! strcmp( v, "gate_less" ) ) snprintf( cmd, sizeof cmd, "GATE +3" );
    else if( ! strcmp( v, "gainreset" ) ) snprintf( cmd, sizeof cmd, "GAINRESET" );
    else if( ! strcmp( v, "quit" ) )                       /* the one way out: aspace drains and closes, then a5web (the accept loop, our parent) stops */
      { chat( "the page asks the space to quit: it will drain, and a5web stops" ); if( relay( fd, "QUIT" ) ) kill( getppid(), SIGTERM ); return; }
    else if( ! strcmp( v, "wipe" ) ) snprintf( cmd, sizeof cmd, "WIPE" );
    else if( ! strcmp( v, "wipeall" ) ) snprintf( cmd, sizeof cmd, "WIPE ALL" );
    else if( ! strcmp( v, "mode_loop" ) ) snprintf( cmd, sizeof cmd, "AUTOMIX LOOP" );
    else if( ! strcmp( v, "mode_keep" ) ) snprintf( cmd, sizeof cmd, "AUTOMIX KEEP" );
    else if( ! strcmp( v, "mode_swap" ) ) snprintf( cmd, sizeof cmd, "AUTOMIX SWAP" );
    else if( ! strcmp( v, "mode_swapeach" ) ) snprintf( cmd, sizeof cmd, "AUTOMIX SWAP EACH" );
    else if( ! strcmp( v, "mode_overlap" ) && num( q, "n", &n ) && n >= 1 && n <= 9 ) snprintf( cmd, sizeof cmd, "AUTOMIX OVERLAP %ld", n );
    else if( ! strcmp( v, "restore" ) && num( q, "seq", &n ) ) snprintf( cmd, sizeof cmd, "RESTORE %ld%s", n, qstr( q, "now", w, sizeof w ) && w[0] == '1' ? " NOW" : "" );
    else { say( fd, 400, "no such action\n" ); return; }
    chat( "the page asks the space: %s", cmd ); relay( fd, cmd ); return;
    }
  if( ! strcmp( p, "/api/mix" ) )                          /* s: what was typed (a5mixer's language); t: iter.off,iter.off the takes it is for */
    {
    char s[48], t[700]; MixCmd mc;
    if( ! qstr( q, "s", s, sizeof s ) || strspn( s, "0123456789+-/\\.!mMpP" ) != strlen( s ) ) { say( fd, 400, "bad entry\n" ); return; }
    int r = a5mix_parse( s, &mc );
    if( r == MP_WAIT ) { say( fd, 200, "WAIT\n" ); return; }
    if( r == MP_BAD ) { say( fd, 200, "BAD\n" ); return; }
    int c = ctlc_connect( dir );
    if( c < 0 ) { say( fd, 502, "the space is not running\n" ); return; }
    int done = 0, bad = 0; char l[220];
    if( mc.has_iter ) { a5mix_line( l, sizeof l, mc.iter, mc.has_take ? mc.take : -1, &mc ); bad += ctlc_cmd( c, l, NULL, NULL ) != 0; done++; }
    else if( qstr( q, "t", t, sizeof t ) && strspn( t, "0123456789.," ) == strlen( t ) )
      for( char *e, *k = t; *k && done < 64; k = e )
        {
        long it = strtol( k, &e, 10 ), off;
        if( e == k || *e != '.' ) break;
        off = strtol( e + 1, &e, 10 ); if( *e == ',' ) e++;
        a5mix_line( l, sizeof l, it, off, &mc ); bad += ctlc_cmd( c, l, NULL, NULL ) != 0; done++;
        }
    close( c );
    if( done ) chat( "mix %s on %d take(s)%s", s, done, bad ? ", some refused" : "" );
    if( ! done ) say( fd, 200, "NONE\n" );
    else { char o[60]; snprintf( o, sizeof o, bad ? "ERR %d of %d\n" : "OK %d\n", bad ? bad : done, done ); say( fd, 200, o ); }
    return;
    }
  say( fd, 404, "no such action\n" );
  }

static void handle( int fd )
  {
  char r[4096]; size_t n = 0;
  alarm( 5 );                                              /* a client that stalls is dropped */
  while( n < sizeof r - 1 )
    {
    ssize_t k = recv( fd, r + n, sizeof r - 1 - n, 0 );
    if( k <= 0 ) return;
    n += (size_t)k; r[n] = 0;
    if( strstr( r, "\r\n\r\n" ) ) break;
    }
  char path[400], host[100] = "", origin[120] = "";
  int is_post = 0;
  if( sscanf( r, "GET %399s ", path ) == 1 ) {}
  else if( sscanf( r, "POST %399s ", path ) == 1 ) is_post = 1;
  else { say( fd, 400, "only GET and POST\n" ); return; }
  for( char *l = strstr( r, "\r\n" ); l; l = strstr( l + 2, "\r\n" ) )
    {
    if( ! strncasecmp( l + 2, "Host:", 5 ) ) sscanf( l + 7, " %99[^\r\n]", host );
    else if( ! strncasecmp( l + 2, "Origin:", 7 ) ) sscanf( l + 9, " %119[^\r\n]", origin );
    }
  char ok1[40], ok2[40]; snprintf( ok1, sizeof ok1, "127.0.0.1:%d", port ); snprintf( ok2, sizeof ok2, "localhost:%d", port );
  if( strcmp( host, ok1 ) && strcmp( host, ok2 ) ) { say( fd, 403, "wrong host\n" ); return; }
  if( is_post )                                            /* a write must come from our own page: the browser says where from */
    {
    char o1[60], o2[60]; snprintf( o1, sizeof o1, "http://%s", ok1 ); snprintf( o2, sizeof o2, "http://%s", ok2 );
    if( strcmp( origin, o1 ) && strcmp( origin, o2 ) ) { say( fd, 403, "wrong origin\n" ); return; }
    post( fd, path ); return;
    }
  for( size_t i = 0; i < sizeof FILES / sizeof *FILES; i++ ) if( ! strcmp( path, FILES[i].url ) ) { serve_file( fd, (int)i ); return; }
  if( ! strcmp( path, "/api/cfg" ) ) { serve_cfg( fd ); return; }
  if( ! strcmp( path, "/api/dir" ) ) { serve_dir( fd ); return; }
  if( ! strcmp( path, "/api/watch" ) ) { watch( fd ); return; }
  char cmd[100];
  if( ! command( path, cmd, sizeof cmd ) ) { say( fd, 404, "no such page\n" ); return; }
  relay( fd, cmd );
  }

/* make sure the space is running: if there is no control port, a5start DIR [--live|--safe] does it, as a5hell and a5control --start do */
static void ensure_space( const char **extra, int nextra )
  {
  int c = ctlc_connect( dir );
  if( c >= 0 ) { close( c ); chat( "the space %s is running", dir ); return; }
  chat( "the space %s is not running: starting it with a5start%s", dir, nextra ? " (and your options)" : "" );
  const char *av[8]; int n = 0;
  av[n++] = "a5start"; av[n++] = dir;
  for( int i = 0; i < nextra && n < 6; i++ ) av[n++] = extra[i];
  av[n] = NULL;
  pid_t p = fork();
  if( p == 0 ) { if( quiet ) { if( ! freopen( "/dev/null", "w", stdout ) || ! freopen( "/dev/null", "w", stderr ) ) _exit( 127 ); } else dup2( 2, 1 ); execvp( "a5start", (char *const *)av ); _exit( 127 ); }
  int status = 0;
  if( p < 0 || waitpid( p, &status, 0 ) < 0 ) { fprintf( stderr, "a5web: could not run a5start\n" ); exit( 1 ); }
  if( WIFEXITED( status ) && WEXITSTATUS( status ) == 127 ) { fprintf( stderr, "a5web: a5start was not found on the path (make install puts it in ~/.local/bin)\n" ); exit( 1 ); }
  if( ! WIFEXITED( status ) || WEXITSTATUS( status ) ) { fprintf( stderr, "a5web: a5start could not start %s, so there is nothing to show: stopping (check the path, or use --no-start to serve the page anyway)\n", dir ); exit( 1 ); }
  c = ctlc_connect( dir );
  if( c >= 0 ) { close( c ); chat( "the space is up" ); } else chat( "a5start finished but there is no control port yet: the page will pick it up when there is" );
  }

int main( int argc, char **argv )
  {
  char wbuf[1000] = ""; const char *extra[4]; int nextra = 0, nostart = 0;
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--port" ) && i + 1 < argc ) port = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--www" ) && i + 1 < argc ) www = argv[++i];
    else if( ! strcmp( argv[i], "--quiet" ) ) quiet = 1;
    else if( ! strcmp( argv[i], "--no-start" ) ) nostart = 1;
    else if( ( ! strcmp( argv[i], "--live" ) || ! strcmp( argv[i], "--safe" ) ) && nextra < 4 ) extra[nextra++] = argv[i];
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { dir = NULL; break; }
    }
  if( ! dir || port < 1 || port > 65535 ) { fprintf( stderr, "usage: a5web DIR [--live|--safe] [--no-start] [--quiet] [--port N] [--www DIR]\n" ); return 2; }
  if( ! www ) www = getenv( "A5WEB_DIR" );
  if( ! www ) { const char *h = getenv( "HOME" ); snprintf( wbuf, sizeof wbuf, "%s/.local/etc/a5web", h ? h : "." ); www = wbuf; }
  if( nostart ) chat( "not starting the space (--no-start)" ); else ensure_space( extra, nextra );
  signal( SIGCHLD, SIG_IGN ); signal( SIGPIPE, SIG_IGN );
  int s = socket( AF_INET, SOCK_STREAM, 0 ), one = 1;
  struct sockaddr_in a;
  setsockopt( s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one );
  memset( &a, 0, sizeof a );
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); a.sin_port = htons( (unsigned short)port );
  if( bind( s, (struct sockaddr *)&a, sizeof a ) || listen( s, 16 ) ) { fprintf( stderr, "a5web: port %d: %s\n", port, strerror( errno ) ); return 1; }
  chat( "open http://127.0.0.1:%d/ in the browser  (pages from %s; ctrl-c stops a5web, not the space)", port, www );
  chat( "I will say here what the page does to the space; --quiet turns this off" );
  for( ;; )
    {
    int c = accept( s, NULL, NULL );
    if( c < 0 ) { if( errno == EINTR ) continue; break; }
    pid_t p = fork();
    if( p == 0 ) { close( s ); handle( c ); close( c ); _exit( 0 ); }
    close( c );
    }
  return 0;
  }
