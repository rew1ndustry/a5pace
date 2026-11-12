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
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ctlclient.h"

static const char *dir, *www;
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
  char path[400], host[100] = "";
  if( sscanf( r, "GET %399s ", path ) != 1 ) { say( fd, 400, "only GET\n" ); return; }
  for( char *l = strstr( r, "\r\n" ); l; l = strstr( l + 2, "\r\n" ) )
    if( ! strncasecmp( l + 2, "Host:", 5 ) ) { sscanf( l + 7, " %99[^\r\n]", host ); break; }
  char ok1[40], ok2[40]; snprintf( ok1, sizeof ok1, "127.0.0.1:%d", port ); snprintf( ok2, sizeof ok2, "localhost:%d", port );
  if( strcmp( host, ok1 ) && strcmp( host, ok2 ) ) { say( fd, 403, "wrong host\n" ); return; }
  for( size_t i = 0; i < sizeof FILES / sizeof *FILES; i++ ) if( ! strcmp( path, FILES[i].url ) ) { serve_file( fd, (int)i ); return; }
  char cmd[100];
  if( ! command( path, cmd, sizeof cmd ) ) { say( fd, 404, "no such page\n" ); return; }
  int c = ctlc_connect( dir );
  if( c < 0 ) { say( fd, 502, "the space is not running\n" ); return; }
  Buf b = { 0 };
  int rc = ctlc_cmd( c, cmd, add, &b );
  close( c );
  if( rc < 0 ) say( fd, 502, "the space did not answer\n" );
  else out( fd, rc ? 500 : 200, "text/plain; charset=utf-8", b.b ? b.b : "", b.n );
  free( b.b );
  }

int main( int argc, char **argv )
  {
  char wbuf[1000] = "";
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--port" ) && i + 1 < argc ) port = atoi( argv[++i] );
    else if( ! strcmp( argv[i], "--www" ) && i + 1 < argc ) www = argv[++i];
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else { dir = NULL; break; }
    }
  if( ! dir || port < 1 || port > 65535 ) { fprintf( stderr, "usage: a5web DIR [--port N] [--www DIR]\n" ); return 2; }
  if( ! www ) www = getenv( "A5WEB_DIR" );
  if( ! www ) { const char *h = getenv( "HOME" ); snprintf( wbuf, sizeof wbuf, "%s/.local/etc/a5web", h ? h : "." ); www = wbuf; }
  signal( SIGCHLD, SIG_IGN ); signal( SIGPIPE, SIG_IGN );
  int s = socket( AF_INET, SOCK_STREAM, 0 ), one = 1;
  struct sockaddr_in a;
  setsockopt( s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one );
  memset( &a, 0, sizeof a );
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); a.sin_port = htons( (unsigned short)port );
  if( bind( s, (struct sockaddr *)&a, sizeof a ) || listen( s, 16 ) ) { fprintf( stderr, "a5web: port %d: %s\n", port, strerror( errno ) ); return 1; }
  fprintf( stderr, "a5web: http://127.0.0.1:%d/  (space %s, pages in %s)\n", port, dir, www );
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
