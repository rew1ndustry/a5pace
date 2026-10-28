#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "a5mixer.h"
#include "ctlclient.h"

int a5mix_parse( const char *s, MixCmd *c )
  {
  const char *p = s; int ops = 0;
  memset( c, 0, sizeof *c );
  if( isdigit( (unsigned char)*p ) )
    {
    c->has_iter = 1; c->iter = strtol( p, (char **)&p, 10 );
    if( *p == '.' ) { p++; if( isdigit( (unsigned char)*p ) ) { c->has_take = 1; c->take = strtol( p, (char **)&p, 10 ); } }
    }
  for( ; ; )
    {
    if( ! *p ) return MP_WAIT;
    if( *p == '+' || *p == '-' || *p == '/' || *p == '\\' )
      {
      char o = *p++; int have = isdigit( (unsigned char)*p ); long v = have ? strtol( p, (char **)&p, 10 ) : 0;
      if( ! *p ) return MP_WAIT;                                  /* the number may not be finished */
      if( o == '+' || o == '-' ) { c->set_gain = have ? 1 : 2; c->gdb = (int)( o == '-' ? -v : v ); }
      else { c->set_pan = have ? 1 : 2; c->pan = (int)( o == '\\' ? -v : v ); }
      ops++;
      continue;
      }
    if( ! strchr( ".!mMpP", *p ) || p[1] ) return MP_BAD;
    c->now = *p == '!' || *p == 'M' || *p == 'P';
    c->act = *p == 'm' || *p == 'M' ? 1 : *p == 'p' || *p == 'P' ? 2 : ops ? 0 : 3;
    return MP_DONE;
    }
  }

typedef struct { long iter, off, id, len; int in, mixed, gdb, pan, pending, mono; } GTake;
static GTake gt[4096]; static int ngt;
typedef struct { long iter; int first, n; } GRow;
static GRow rows[2048]; static int nrows;
static int showall;
static long sticky[2048]; static int nsticky;      /* a row that has been shown stays while the screen is up, so a mute does not make it vanish */

static void grid_line( const char *l, void *u )
  {
  GTake t; (void)u;
  if( sscanf( l, "%ld %ld %ld %d %d %d %d %d %d %ld", &t.iter, &t.off, &t.id, &t.in, &t.mixed, &t.gdb, &t.pan, &t.pending, &t.mono, &t.len ) == 10 && ngt < 4096 )
    gt[ngt++] = t;
  }
static int by_take( const void *a, const void *b )
  { const GTake *x = a, *y = b; return x->iter != y->iter ? ( x->iter < y->iter ? -1 : 1 ) : x->off < y->off ? -1 : x->off > y->off; }

static void fetch( int cmd )
  {
  ngt = 0; nrows = 0;
  ctlc_cmd( cmd, "GRID", grid_line, NULL );
  qsort( gt, ngt, sizeof *gt, by_take );
  for( int i = 0; i < ngt; )
    {
    int j = i, show = 0;
    while( j < ngt && gt[j].iter == gt[i].iter ) { show |= gt[j].in || gt[j].mixed || gt[j].pending; j++; }
    { int known = 0; for( int k = 0; k < nsticky; k++ ) known |= sticky[k] == gt[i].iter;
      if( show && ! known && nsticky < 2048 ) sticky[nsticky++] = gt[i].iter;
      show |= known; }
    if( ( show || showall ) && nrows < 2048 ) rows[nrows++] = (GRow){ gt[i].iter, i, j - i };
    i = j;
    }
  }

static int key_wait( int ms ) { struct pollfd p = { 0, POLLIN, 0 }; return poll( &p, 1, ms ) > 0; }
enum { K_NONE = 256, K_ESC, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_BS };
static int getkey( int *shift )
  {
  unsigned char k; *shift = 0;
  if( read( 0, &k, 1 ) != 1 ) return K_NONE;
  if( k == 127 || k == 8 ) return K_BS;
  if( k != 27 ) return k;
  if( ! key_wait( 40 ) ) return K_ESC;
  if( read( 0, &k, 1 ) != 1 || ( k != '[' && k != 'O' ) ) return K_ESC;
  int fin = 0;
  for( ; ; )
    {
    if( ! key_wait( 40 ) || read( 0, &k, 1 ) != 1 ) return K_NONE;
    if( k == ';' ) { if( key_wait( 40 ) && read( 0, &k, 1 ) == 1 && k == '2' ) *shift = 1; continue; }
    if( isalpha( k ) ) { fin = k; break; }
    }
  return fin == 'A' ? K_UP : fin == 'B' ? K_DOWN : fin == 'C' ? K_RIGHT : fin == 'D' ? K_LEFT : K_NONE;
  }

#define CW 14                       /* a cell */
#define LW 11                       /* the iteration column */

static long cur_iter, anc_iter; static int cur_col, anc_col, hscroll, started;
static char msg[200], ent[64]; static int nent;

static int row_of( long iter ) { for( int i = 0; i < nrows; i++ ) if( rows[i].iter == iter ) return i; return -1; }
static int row_near( long iter ) { int best = nrows - 1; for( int i = 0; i < nrows; i++ ) if( rows[i].iter >= iter ) { best = i; break; } return best; }

static void sel_range( int *r1, int *r2, int *c1, int *c2 )
  {
  int a = row_of( anc_iter ), b = row_of( cur_iter );
  if( a < 0 ) a = b; if( b < 0 ) b = a;
  *r1 = a < b ? a : b; *r2 = a < b ? b : a;
  *c1 = anc_col < cur_col ? anc_col : cur_col; *c2 = anc_col < cur_col ? cur_col : anc_col;
  }
static int in_sel( int r, int col )
  {
  int r1, r2, c1, c2; sel_range( &r1, &r2, &c1, &c2 );
  if( r < r1 || r > r2 ) return 0;
  return c1 == 0 ? 1 : col >= c1 && col <= c2;
  }

static void cell( const GTake *t, char *b, size_t n )
  {
  char mx[40] = "";
  if( t->mixed )
    {
    int o = snprintf( mx, sizeof mx, "%+d", t->gdb );
    if( t->mono ) snprintf( mx + o, sizeof mx - o, t->pan < 0 ? "L%d" : t->pan > 0 ? "R%d" : "C", t->pan < 0 ? -t->pan : t->pan );
    }
  snprintf( b, n, "%3ld%c%c%s%s", t->off, t->in ? ' ' : 'm', t->mixed ? '*' : ' ', mx, t->pending == 2 ? "!" : t->pending ? "~" : "" );
  }

static void draw( const char *space )
  {
  struct winsize ws; int W = 100, H = 30;
  if( ioctl( 1, TIOCGWINSZ, &ws ) == 0 && ws.ws_col > 20 && ws.ws_row > 8 ) { W = ws.ws_col; H = ws.ws_row; }
  int vc = ( W - LW ) / CW; if( vc < 1 ) vc = 1;
  int cr = row_of( cur_iter ), body = H - 6, top = 0;
  if( cr < 0 ) cr = 0;
  if( cur_col >= 1 ) { if( cur_col - 1 < hscroll ) hscroll = cur_col - 1; if( cur_col - 1 >= hscroll + vc ) hscroll = cur_col - vc; }
  if( hscroll < 0 ) hscroll = 0;
  if( cr >= body ) top = cr - body + 1;
  printf( "\033[H\033[J mixing %s    iteration x takes   m mute  p play  . toggle  ! now   A %s   ESC back\n", space, showall ? "hide muted" : "show all" );
  printf( " * has a mix of its own   m muted   ~ waits for the take's next start   ! at once        e.g. 12.3-2/20.  12m  12..\n\n" );
  for( int i = top; i < nrows && i < top + body; i++ )
    {
    const GRow *r = rows + i; char lab[24];
    snprintf( lab, sizeof lab, "%ld (%d)", r->iter, r->n );
    printf( "%s%s%-*s\033[0m", in_sel( i, 0 ) ? "\033[7m" : "", i == cr && cur_col == 0 ? "\033[1m" : "", LW, lab );
    for( int k = hscroll; k < r->n && k < hscroll + vc; k++ )
      {
      char b[48]; const GTake *t = gt + r->first + k;
      cell( t, b, sizeof b );
      printf( "%s%s%s%-*s\033[0m", t->in ? "" : "\033[2m", in_sel( i, k + 1 ) ? "\033[7m" : "", i == cr && cur_col == k + 1 ? "\033[1;4m" : "", CW, b );
      }
    if( r->n > hscroll + vc ) printf( ">" );
    printf( "\n" );
    }
  if( ! nrows ) printf( "  (no takes in the loop; A shows all)\n" );
  printf( "\033[%d;1H\033[K %s\n\033[K > %s", H - 1, msg, ent );
  fflush( stdout );
  }

static void say_line( const char *l, void *u ) { (void)u; snprintf( msg, sizeof msg, "%s", l ); }

static void send_mix( int cmd, long iter, long off, const MixCmd *c )
  {
  char b[200]; int o = snprintf( b, sizeof b, "MIX %ld ", iter );
  o += off < 0 ? snprintf( b + o, sizeof b - o, "*" ) : snprintf( b + o, sizeof b - o, "%ld", off );
  if( c->set_gain ) o += c->set_gain == 2 ? snprintf( b + o, sizeof b - o, " g" ) : snprintf( b + o, sizeof b - o, " g%d", c->gdb );
  if( c->set_pan ) o += c->set_pan == 2 ? snprintf( b + o, sizeof b - o, " p" ) : snprintf( b + o, sizeof b - o, " p%d", c->pan );
  if( c->act ) o += snprintf( b + o, sizeof b - o, c->act == 1 ? " MUTE" : c->act == 2 ? " PLAY" : " TOGGLE" );
  if( c->now ) snprintf( b + o, sizeof b - o, " NOW" );
  msg[0] = 0;
  ctlc_cmd( cmd, b, say_line, NULL );
  }

static void run( int cmd, const MixCmd *c )
  {
  msg[0] = 0;
  if( c->has_iter ) { send_mix( cmd, c->iter, c->has_take ? c->take : -1, c ); if( ! msg[0] ) snprintf( msg, sizeof msg, "no such take" ); return; }
  int r1, r2, c1, c2, n = 0; sel_range( &r1, &r2, &c1, &c2 );
  if( r1 < 0 ) return;
  for( int r = r1; r <= r2; r++ )
    {
    if( c1 == 0 ) { send_mix( cmd, rows[r].iter, -1, c ); n++; continue; }
    for( int k = c1; k <= c2 && k <= rows[r].n; k++ ) { send_mix( cmd, rows[r].iter, gt[rows[r].first + k - 1].off, c ); n++; }
    }
  if( ! n ) snprintf( msg, sizeof msg, "nothing selected" );
  }

void a5mixer( int cmd, int ev, const char *space )
  {
  nsticky = 0; fetch( cmd );
  if( ! started && nrows ) { cur_iter = anc_iter = rows[nrows - 1].iter; cur_col = anc_col = 0; started = 1; }
  nent = 0; ent[0] = 0; msg[0] = 0;
  for( ; ; )
    {
    if( cur_iter != 0 || nrows ) { int r = row_of( cur_iter ); if( r < 0 && nrows ) { r = row_near( cur_iter ); cur_iter = rows[r].iter; if( cur_col > rows[r].n ) cur_col = rows[r].n; } if( row_of( anc_iter ) < 0 && nrows ) anc_iter = cur_iter; }
    draw( space );
    struct pollfd p[2] = { { 0, POLLIN, 0 }, { ev, POLLIN, 0 } };
    if( poll( p, 2, 250 ) <= 0 ) { fetch( cmd ); continue; }
    if( p[1].revents & POLLIN ) { char b[1200]; if( ctlc_readline( ev, b, sizeof b, 10 ) < 0 ) return; }
    if( ! ( p[0].revents & POLLIN ) ) continue;
    int shift, k = getkey( &shift );
    if( k == K_NONE ) continue;
    if( k == K_ESC ) { if( nent ) { nent = 0; ent[0] = 0; continue; } printf( "\033[H\033[J" ); return; }
    int cr = row_of( cur_iter ); if( cr < 0 ) cr = 0;
    if( k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT )
      {
      if( k == K_UP && cr > 0 ) cr--;
      if( k == K_DOWN && cr < nrows - 1 ) cr++;
      if( nrows )
        {
        if( k == K_LEFT && cur_col > 0 ) cur_col--;
        if( k == K_RIGHT && cur_col < rows[cr].n ) cur_col++;
        if( cur_col > rows[cr].n ) cur_col = rows[cr].n;
        cur_iter = rows[cr].iter;
        if( ! shift ) { anc_iter = cur_iter; anc_col = cur_col; }
        }
      continue;
      }
    if( k == K_BS ) { if( nent ) ent[--nent] = 0; continue; }
    if( k == 'A' && ! nent ) { showall = ! showall; fetch( cmd ); continue; }
    if( k < 256 && ( isdigit( k ) || strchr( ".!mMpP+-/\\", k ) ) && nent < (int)sizeof ent - 2 )
      {
      ent[nent++] = (char)k; ent[nent] = 0;
      MixCmd c; int r = a5mix_parse( ent, &c );
      if( r == MP_WAIT ) continue;
      if( r == MP_DONE ) { run( cmd, &c ); fetch( cmd ); }
      else snprintf( msg, sizeof msg, "not understood: %s", ent );
      nent = 0; ent[0] = 0;
      }
    }
  }
