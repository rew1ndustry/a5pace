/* ctlsrv: the control service. a line protocol on 127.0.0.1 (TCP_NODELAY on every socket).
   a client sends a line; the reply is any number of lines ending in "OK" or "ERR text".
   WATCH turns the connection into a stream of "@tick type json" event lines. posix sockets; the
   windows version is a winsock shim of the same few calls. */
#ifndef CTLSRV_H
#define CTLSRV_H
typedef struct CtlSrv CtlSrv;
typedef void (*CtlHandler)( void *user, int client, const char *line );
CtlSrv *ctl_open( int port, int *chosen );                  /* port 0: any free port */
void    ctl_poll( CtlSrv *s, CtlHandler h, void *user );    /* accept and read, never blocks */
void    ctl_say( CtlSrv *s, int client, const char *form, ... );
void    ctl_ok( CtlSrv *s, int client );
void    ctl_err( CtlSrv *s, int client, const char *form, ... );
void    ctl_watch( CtlSrv *s, int client, int on );
int     ctl_watchers( CtlSrv *s );
void    ctl_broadcast( CtlSrv *s, const char *line );
void    ctl_close( CtlSrv *s );
#endif
