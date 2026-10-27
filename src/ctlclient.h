/* ctlclient: talk to a running aspace. the session dir holds "PORT PID" in ctl. */
#ifndef CTLCLIENT_H
#define CTLCLIENT_H
int  ctlc_connect( const char *dir );                       /* socket, or -1 */
/* send a line; call each reply line (not the final OK/ERR) through out; returns 0 for OK, 1 for ERR, -1 for a broken link */
int  ctlc_cmd( int fd, const char *line, void (*out)( const char *l, void *u ), void *u );
int  ctlc_readline( int fd, char *buf, int n, int timeout_ms );   /* one line; 1 got, 0 timeout, -1 closed */
#endif
