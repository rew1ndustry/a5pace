#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "ctlsrv.h"

#define MAXC 8
#define LINE 1024

typedef struct { int fd, len, watch; char buf[LINE]; } Client;
struct CtlSrv { int fd; Client c[MAXC]; };

static void nodelay( int fd )
  { int one = 1; setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one ); }

CtlSrv *ctl_open( int port, int *chosen )
  {
  int fd = socket( AF_INET, SOCK_STREAM, 0 ), one = 1;
  struct sockaddr_in a;
  if( fd < 0 ) return NULL;
  setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one );
  memset( &a, 0, sizeof a );
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); a.sin_port = htons( (unsigned short)port );
  if( bind( fd, (struct sockaddr *)&a, sizeof a ) || listen( fd, 4 ) ) { close( fd ); return NULL; }
  socklen_t n = sizeof a;
  getsockname( fd, (struct sockaddr *)&a, &n );
  if( chosen ) *chosen = ntohs( a.sin_port );
  fcntl( fd, F_SETFL, O_NONBLOCK );
  CtlSrv *s = calloc( 1, sizeof *s );
  s->fd = fd;
  for( int i = 0; i < MAXC; i++ ) s->c[i].fd = -1;
  return s;
  }

static void drop( Client *c ) { if( c->fd >= 0 ) close( c->fd ); c->fd = -1; c->len = 0; c->watch = 0; }

static void put( Client *c, const char *buf, size_t n )
  {
  while( n && c->fd >= 0 )
    {
    ssize_t w = send( c->fd, buf, n, MSG_NOSIGNAL );
    if( w < 0 ) { if( errno == EINTR ) continue; if( errno == EAGAIN ) { struct pollfd p = { c->fd, POLLOUT, 0 }; if( poll( &p, 1, 200 ) > 0 ) continue; } drop( c ); return; }
    buf += w; n -= (size_t)w;
    }
  }

void ctl_say( CtlSrv *s, int i, const char *form, ... )
  {
  char b[1400];
  va_list ap;
  va_start( ap, form ); int n = vsnprintf( b, sizeof b - 1, form, ap ); va_end( ap );
  if( n < 0 ) return;
  if( n > (int)sizeof b - 2 ) n = sizeof b - 2;
  b[n++] = '\n';
  put( s->c + i, b, (size_t)n );
  }
void ctl_ok( CtlSrv *s, int i ) { ctl_say( s, i, "OK" ); }
void ctl_err( CtlSrv *s, int i, const char *form, ... )
  {
  char b[1000]; va_list ap;
  va_start( ap, form ); vsnprintf( b, sizeof b, form, ap ); va_end( ap );
  ctl_say( s, i, "ERR %s", b );
  }
void ctl_watch( CtlSrv *s, int i, int on ) { s->c[i].watch = on; }
int ctl_watchers( CtlSrv *s ) { int n = 0; for( int i = 0; i < MAXC; i++ ) n += s->c[i].fd >= 0 && s->c[i].watch; return n; }
void ctl_broadcast( CtlSrv *s, const char *line )
  {
  for( int i = 0; i < MAXC; i++ ) if( s->c[i].fd >= 0 && s->c[i].watch ) ctl_say( s, i, "%s", line );
  }

void ctl_poll( CtlSrv *s, CtlHandler h, void *user )
  {
  int fd;
  while( ( fd = accept( s->fd, NULL, NULL ) ) >= 0 )
    {
    int i;
    for( i = 0; i < MAXC && s->c[i].fd >= 0; i++ ) {}
    if( i == MAXC ) { close( fd ); continue; }
    nodelay( fd );
    fcntl( fd, F_SETFL, O_NONBLOCK );
    s->c[i].fd = fd; s->c[i].len = 0; s->c[i].watch = 0;
    }
  for( int i = 0; i < MAXC; i++ )
    {
    Client *c = s->c + i;
    if( c->fd < 0 ) continue;
    ssize_t n = recv( c->fd, c->buf + c->len, LINE - 1 - c->len, 0 );
    if( n == 0 ) { drop( c ); continue; }
    if( n < 0 ) { if( errno != EAGAIN && errno != EINTR ) drop( c ); continue; }
    c->len += (int)n;
    for( ;; )
      {
      char *nl = memchr( c->buf, '\n', (size_t)c->len );
      if( ! nl ) { if( c->len >= LINE - 1 ) c->len = 0; break; }
      *nl = 0;
      if( nl > c->buf && nl[-1] == '\r' ) nl[-1] = 0;
      char line[LINE]; snprintf( line, sizeof line, "%s", c->buf );
      c->len -= (int)( nl - c->buf ) + 1;
      memmove( c->buf, nl + 1, (size_t)c->len );
      if( line[0] ) h( user, i, line );
      if( c->fd < 0 ) break;
      }
    }
  }

void ctl_close( CtlSrv *s ) { for( int i = 0; i < MAXC; i++ ) drop( s->c + i ); close( s->fd ); free( s ); }
