#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "ctlclient.h"

int ctlc_connect( const char *dir )
  {
  char p[1200]; int port = 0, pid = 0;
  snprintf( p, sizeof p, "%s/ctl", dir );
  FILE *f = fopen( p, "r" );
  if( ! f ) return -1;
  if( fscanf( f, "%d %d", &port, &pid ) < 1 ) { fclose( f ); return -1; }
  fclose( f );
  int fd = socket( AF_INET, SOCK_STREAM, 0 ), one = 1;
  struct sockaddr_in a;
  if( fd < 0 ) return -1;
  memset( &a, 0, sizeof a );
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); a.sin_port = htons( (unsigned short)port );
  if( connect( fd, (struct sockaddr *)&a, sizeof a ) ) { close( fd ); return -1; }
  setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one );
  return fd;
  }

int ctlc_readline( int fd, char *buf, int n, int timeout_ms )
  {
  int len = 0;
  for( ;; )
    {
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll( &p, 1, timeout_ms );
    if( r == 0 ) return 0;
    if( r < 0 ) return -1;
    char ch;
    ssize_t k = recv( fd, &ch, 1, 0 );
    if( k <= 0 ) return -1;
    if( ch == '\n' ) { buf[len] = 0; return 1; }
    if( len < n - 1 ) buf[len++] = ch;
    }
  }

int ctlc_cmd( int fd, const char *line, void (*out)( const char *, void * ), void *u )
  {
  char b[1500];
  snprintf( b, sizeof b, "%s\n", line );
  if( send( fd, b, strlen( b ), MSG_NOSIGNAL ) < 0 ) return -1;
  for( ;; )
    {
    int r = ctlc_readline( fd, b, sizeof b, 600000 );
    if( r <= 0 ) return -1;
    if( ! strcmp( b, "OK" ) ) return 0;
    if( ! strncmp( b, "ERR", 3 ) ) { if( out ) out( b, u ); return 1; }
    if( out ) out( b, u );
    }
  }
