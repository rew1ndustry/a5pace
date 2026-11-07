#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "a5term.h"

static TermCfg cfg = { -24, -6, 1, 0, { 37, 91, 92, 92, 93, 91 } };
static struct termios saved; static int rawed;
static volatile sig_atomic_t winch;

static const char *const names[C_N] = { "col_text", "col_live", "col_safe", "col_low", "col_mid", "col_high" };

void term_config( int dim_flag )
  {
  char path[600], line[200]; const char *e = getenv( "A5HELL_CFG" ), *h = getenv( "HOME" );
  if( e && *e ) snprintf( path, sizeof path, "%s", e ); else snprintf( path, sizeof path, "%s/.local/etc/a5hell.cfg", h ? h : "" );
  FILE *f = fopen( path, "r" );
  while( f && fgets( line, sizeof line, f ) )
    {
    char k[64]; int v;
    if( line[0] == '#' || sscanf( line, "%63s %d", k, &v ) != 2 ) continue;
    if( ! strcmp( k, "green_below_db" ) ) cfg.green_below_db = v;
    else if( ! strcmp( k, "red_above_db" ) ) cfg.red_above_db = v;
    else if( ! strcmp( k, "paint_background" ) ) cfg.paint = v != 0;
    else if( ! strcmp( k, "dim" ) ) cfg.dim = v != 0;
    else for( int i = 0; i < C_N; i++ ) if( ! strcmp( k, names[i] ) && v >= 30 && v <= 107 ) cfg.col[i] = v;
    }
  if( f ) fclose( f );
  if( dim_flag ) cfg.dim = 1;
  }
const TermCfg *term_cfg( void ) { return &cfg; }

const char *term_sgr( int role )
  {
  static char b[C_N][40]; int c;
  if( role < 0 || role >= C_N ) role = C_TEXT;
  c = cfg.col[role];
  if( cfg.dim && c >= 90 && c <= 97 ) c -= 60;                        /* bright becomes normal intensity */
  snprintf( b[role], sizeof b[role], "\033[0;%s%d%s%sm", cfg.paint ? "40;" : "", c,
            cfg.dim && role == C_TEXT ? ";2" : "", role == C_LIVE || role == C_SAFE ? ";1" : "" );
  return b[role];
  }

static void on_winch( int s ) { (void)s; winch = 1; }
int term_resized( void ) { int r = winch; winch = 0; return r; }

void term_cls( void ) { fputs( term_sgr( C_TEXT ), stdout ); fputs( "\033[H\033[J", stdout ); }

void term_close( void )
  {
  if( ! rawed ) return;
  rawed = 0;
  fputs( "\033[0m\033[?25h\033[?1049l", stdout ); fflush( stdout );
  tcsetattr( 0, TCSANOW, &saved );
  }

int term_open( void )
  {
  struct termios raw; struct sigaction sa;
  if( ! isatty( 0 ) || ! isatty( 1 ) ) return 0;
  tcgetattr( 0, &saved ); raw = saved;
  raw.c_lflag &= ~(tcflag_t)( ICANON | ECHO ); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
  tcsetattr( 0, TCSANOW, &raw );
  memset( &sa, 0, sizeof sa ); sa.sa_handler = on_winch; sigaction( SIGWINCH, &sa, NULL );
  rawed = 1; atexit( term_close );
  fputs( "\033[?1049h\033[?25l", stdout ); term_cls(); fflush( stdout );
  return 1;
  }

void term_size( int *cols, int *rows )
  {
  struct winsize ws; int w = 0, h = 0;
  if( ioctl( 1, TIOCGWINSZ, &ws ) == 0 ) { w = ws.ws_col; h = ws.ws_row; }
  if( w < 1 && getenv( "COLUMNS" ) ) w = atoi( getenv( "COLUMNS" ) );
  if( h < 1 && getenv( "LINES" ) ) h = atoi( getenv( "LINES" ) );
  *cols = w >= 20 ? w : 80; *rows = h >= 6 ? h : 24;
  }

static int key_wait( int ms ) { struct pollfd p = { 0, POLLIN, 0 }; return poll( &p, 1, ms ) > 0; }

int term_wait( int fd2, int ms )
  { struct pollfd p[2] = { { 0, POLLIN, 0 }, { fd2, POLLIN, 0 } }; return poll( p, fd2 >= 0 ? 2 : 1, ms ); }

int term_key( int *shift )
  {
  unsigned char k; int v[2] = { 0, 0 }, part = 0, fin;
  *shift = 0;
  if( read( 0, &k, 1 ) != 1 ) return K_NONE;
  if( k == 127 || k == 8 ) return K_BS;
  if( k != 27 ) return k;
  if( ! key_wait( 40 ) ) return K_ESC;
  if( read( 0, &k, 1 ) != 1 || ( k != '[' && k != 'O' ) ) return K_ESC;
  for( ; ; )
    {
    if( ! key_wait( 40 ) || read( 0, &k, 1 ) != 1 ) return K_NONE;
    if( isdigit( k ) ) { v[part] = v[part] * 10 + ( k - '0' ); continue; }
    if( k == ';' ) { part = 1; continue; }
    if( isalpha( k ) || k == '~' ) { fin = k; break; }
    }
  *shift = v[1] == 2;
  switch( fin )
    {
    case 'A': return K_UP;  case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME; case 'F': return K_END;
    case '~': return v[0] == 5 ? K_PGUP : v[0] == 6 ? K_PGDN : v[0] == 1 || v[0] == 7 ? K_HOME : v[0] == 4 || v[0] == 8 ? K_END : K_NONE;
    }
  return K_NONE;
  }
