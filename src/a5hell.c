/* a5hell DIR [--live] [--loop] [--dim] [--no-start]: a terminal shell for a running aspace. fills the terminal: the state in
   the corners, what has just happened in words, a few keys; press ? for all of them. if the space is not running it is
   started first (a5start; --no-start to not), safe and in keep mode unless --live / --loop.
     l  LIVE   s  SAFE   h  history: go back to a place     w  wipe the loop     a  mode: al ak a0 a1-9 ap
     u  undo the last take   U  the last iteration   x  export (a5export)   arrows  the cursor on the map
     r  sync again   g  gate menu (l learn, + -)   f  profile   ?  help   q  leave   Q  stop aspace
   the shell has no safe/live state of its own: it shows aspace's, and l / s ask aspace to change it.
   nothing here is needed to run aspace: it is only a window on the control port. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include "ctlclient.h"
#include "a5mixer.h"
#include <ctype.h>
#include "a5term.h"
#include "a5map.h"
#include <time.h>

#define EV 64
static char status[400], events[EV][220], space[100]; static int nev;
static void keep_state( const char *l, void *u ) { snprintf( (char *)u, 1500, "%s", l ); }
static void keep( const char *l, void *u ) { (void)u; snprintf( status, sizeof status, "%s", l ); }
static void show( const char *l, void *u ) { (void)u; printf( "\r\033[K%s\n", l ); }
static void quiet( const char *l, void *u ) { (void)u; if( ! strncmp( l, "ERR", 3 ) ) snprintf( events[nev++ % EV], 220, "%.200s", l ); }

/* the middle of the screen: the log as it happens, in words, and only what matters */
static double jfld( const char *j, const char *key, double d ) { char q[48]; snprintf( q, sizeof q, "\"%s\":", key ); const char *p = strstr( j, q ); return p ? atof( p + strlen( q ) ) : d; }
static int pretty( const char *line, char *out, size_t n )
  {
  const char *p = line, *type, *json; char t[40];
  if( *p != '@' ) { snprintf( out, n, "%s", line ); return 1; }
  p = strchr( p, ' ' ); if( ! p ) return 0; type = ++p;
  json = strchr( type, ' ' ); if( ! json ) json = ""; 
  snprintf( t, sizeof t, "%.*s", (int)( json - type > 39 ? 39 : json - type ), type );
  if( ! strcmp( t, "end" ) ) { const char *pk = strstr( json, "\"peak_db\":[" ); double a = pk ? atof( pk + 11 ) : 0, b = pk && strchr( pk + 11, ',' ) ? atof( strchr( pk + 11, ',' ) + 1 ) : a;
    snprintf( out, n, "recorded: iteration %d at %ds, %.1f s long, peak %.0f dB%s", (int)jfld( json, "iter", 0 ), (int)jfld( json, "off", 0 ), jfld( json, "len", 0 ) / 48000.0, a > b ? a : b, jfld( json, "mono", 0 ) > 0 ? ", mono" : "" ); }
  else if( ! strcmp( t, "discarded" ) ) snprintf( out, n, "ignored a %s (%.1f s)", strstr( json, "\"bump\"" ) ? "bump" : "sound too quiet to keep", jfld( json, "len", 0 ) / 48000.0 );
  else if( ! strcmp( t, "wipe" ) ) snprintf( out, n, "wiped %d take(s) out of the loop", (int)jfld( json, "takes", 0 ) );
  else if( ! strcmp( t, "reverted" ) ) snprintf( out, n, "went back to log point %d: %d take(s) in the loop", (int)jfld( json, "to", 0 ), (int)jfld( json, "takes", 0 ) );
  else if( ! strcmp( t, "sync_found" ) ) snprintf( out, n, "synced: round trip %.0f ms", jfld( json, "frames", 0 ) / 48.0 );
  else if( ! strcmp( t, "sync_start" ) || ! strcmp( t, "resync" ) ) snprintf( out, n, "syncing..." );
  else if( ! strcmp( t, "sync_failed" ) ) snprintf( out, n, "could not sync: check the loopback and the levels" );
  else if( ! strcmp( t, "safe_on" ) ) snprintf( out, n, "safe: nothing is being recorded" );
  else if( ! strcmp( t, "safe_off" ) ) snprintf( out, n, "live: recording" );
  else if( ! strcmp( t, "settings" ) ) { const char *pf = strstr( json, "\"profile\":\"" ); snprintf( out, n, "profile %.*s", pf ? (int)strcspn( pf + 11, "\"" ) : 0, pf ? pf + 11 : "" ); }
  else if( ! strcmp( t, "automix" ) ) snprintf( out, n, "mode: %s", jfld( json, "mode", 0 ) == 1 ? "overlap" : jfld( json, "mode", 0 ) == 2 ? "swap" : jfld( json, "mode", 0 ) == 3 ? "keep (recorded, not put into the loop)" : "loop" );
  else if( ! strcmp( t, "gate" ) ) { if( strstr( json, "learnt" ) ) snprintf( out, n, "the gate has learnt the noise floor" ); else return 0; }
  else if( ! strcmp( t, "edge_drop" ) || ! strcmp( t, "edge_starve" ) ) snprintf( out, n, "the sound card dropped samples: syncing again" );
  else if( ! strcmp( t, "error" ) ) snprintf( out, n, "error: %.200s", json );
  else if( ! strcmp( t, "undo" ) ) snprintf( out, n, "undone: %d take(s) out, %d back in", (int)jfld( json, "takes", 0 ), (int)jfld( json, "back", 0 ) );
  else if( ! strcmp( t, "restore" ) ) snprintf( out, n, "put back as at log point %d: %d take(s) changed", (int)jfld( json, "to", 0 ), (int)jfld( json, "changed", 0 ) );
  else if( ! strcmp( t, "click_added" ) ) snprintf( out, n, "click track added" );
  else return 0;                                                  /* wrap, top, new, relevel ...: not for this screen */
  return 1;
  }

/* one screen line: row r (1 based), painted in a role, padded with the background to the edge */
static void put( int r, int role, const char *t, int w )
  { printf( "\033[%d;1H%s%.*s\033[K", r, term_sgr( role ), w, t ); }
/* the same with the state word in the corners: LIVE red, SAFE green, the rest grey */
static void corners( int r, int w, int live, const char *left, const char *right )
  {
  const char *word = live ? "LIVE" : "SAFE"; int role = live ? C_LIVE : C_SAFE, rl = (int)strlen( right );
  printf( "\033[%d;1H%s%s%s %.*s\033[K", r, term_sgr( role ), word, term_sgr( C_TEXT ), w - 6, left );
  printf( "\033[%d;%dH%s%s", r, w - rl - 4, term_sgr( C_TEXT ), right );
  printf( "%s %s", term_sgr( role ), word );
  }

static const char *mode_word( int m ) { return m == 2 ? "hold" : m == 3 ? "ping-pong" : m == 1 ? "keep" : "loop"; }

/* the map: the iterations shown end at mbottom (the current one while following); MAP is asked for them and some before, because
   a take that began earlier runs into these rows. refreshed about once a second, and at once when the view or the takes change */
static Map map; static long mbottom, mcur; static int mfollow = 1, loopT = 380, mdirty = 1; static long mtop_seen = -2, mbot_seen = -2; static double mlast;
#define MAP_BACK 60
static double ms_now( void ) { struct timespec t; clock_gettime( CLOCK_MONOTONIC, &t ); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void map_update( int cmd, int h )
  {
  static char st[1500]; int rows = h - 5; if( rows < 1 ) rows = 1;
  st[0] = 0; ctlc_cmd( cmd, "STATE", keep_state, st );
  if( st[0] ) { double lf = jfld( st, "loop_frames", 0 ); mcur = (long)jfld( st, "iter", 0 ); if( lf >= 9600 ) loopT = (int)( lf / 9600 ); }
  if( mfollow || mbottom > mcur ) { mbottom = mcur; mfollow = 1; }
  long top = mbottom - rows + 1;
  if( ! mdirty && top == mtop_seen && mbottom == mbot_seen && ms_now() - mlast < 1000 ) return;
  char c[80]; long from = top - MAP_BACK; if( from < 0 ) from = 0;
  snprintf( c, sizeof c, "MAP %ld %ld", from, mbottom );
  map_clear( &map ); ctlc_cmd( cmd, c, map_line, &map );
  mtop_seen = top; mbot_seen = mbottom; mdirty = 0; mlast = ms_now();
  }

/* the cursor: a place on the map (an iteration and a cell); the anchor is where a selection began. entry is a command being typed */
static long cur_it, anc_it; static int cur_cell, anc_cell, have_cur; static char entry[64]; static int nentry;
static int geom_cells( int w, int *tpc ) { int lw, cpc; return map_geom( loopT, w, mbottom, &lw, tpc, &cpc ); }
static MapSel the_sel( void )
  {
  MapSel s;
  s.r1 = cur_it < anc_it ? cur_it : anc_it; s.r2 = cur_it < anc_it ? anc_it : cur_it;
  s.c1 = cur_cell < anc_cell ? cur_cell : anc_cell; s.c2 = cur_cell < anc_cell ? anc_cell : cur_cell;
  s.cur_it = cur_it; s.cur_cell = cur_cell;
  return s;
  }
/* the cursor starts on the newest take (the last in MAP's order is not always the newest: take the highest iteration, then offset) */
static void cursor_start( int w )
  {
  int best = -1, tpc;
  for( int i = 0; i < map.n; i++ )
    if( map.t[i].st != 2 && ( best < 0 || map.t[i].iter > map.t[best].iter || ( map.t[i].iter == map.t[best].iter && map.t[i].off > map.t[best].off ) ) ) best = i;
  geom_cells( w, &tpc );
  cur_it = mbottom; cur_cell = 0;
  if( best >= 0 )
    for( int k = 0; k < map.t[best].n; k++ )
      if( map.t[best].pk[k] ) { long a = map.t[best].off * 5 + k; cur_it = map.t[best].iter + a / loopT; cur_cell = (int)( ( a % loopT ) / tpc ); break; }
  anc_it = cur_it; anc_cell = cur_cell; have_cur = 1;
  }
static void map_scroll( int key, int h );
static void cursor_move( int key, int shift, int w, int h )
  {
  int tpc, cells = geom_cells( w, &tpc ), rows = h - 5;
  if( key == K_UP && cur_it > 0 ) cur_it--;
  else if( key == K_DOWN && cur_it < mcur ) cur_it++;
  else if( key == K_LEFT && cur_cell > 0 ) cur_cell--;
  else if( key == K_RIGHT && cur_cell < cells - 1 ) cur_cell++;
  if( cur_cell >= cells ) cur_cell = cells - 1;
  if( ! shift ) { anc_it = cur_it; anc_cell = cur_cell; }
  if( rows < 1 ) rows = 1;
  if( cur_it < mbottom - rows + 1 ) { mfollow = 0; mbottom = cur_it + rows - 1; if( mbottom > mcur ) mbottom = mcur; }
  if( cur_it > mbottom ) mbottom = cur_it;
  if( mbottom >= mcur ) { mbottom = mcur; mfollow = 1; }
  mdirty = 1;
  }

/* a typed command (a5mix_parse's language) on what the cursor or selection touches; or on the iteration it names */
static char mix_reply[220];
static void say( const char *l, void *u ) { (void)u; snprintf( mix_reply, sizeof mix_reply, "%s", l ); }
static void mix_run( int cmd, const MixCmd *c, int w )
  {
  char b[200]; int n = 0;
  if( c->has_iter ) { a5mix_line( b, sizeof b, c->iter, c->has_take ? c->take : -1, c ); mix_reply[0] = 0; ctlc_cmd( cmd, b, say, NULL ); n = mix_reply[0] ? 1 : 0; }
  else
    {
    int idx[256], k; MapSel sel = the_sel(); k = map_affected( &map, &sel, loopT, w, mbottom, idx, 256 );
    for( int i = 0; i < k; i++ ) { a5mix_line( b, sizeof b, map.t[idx[i]].iter, map.t[idx[i]].off, c ); ctlc_cmd( cmd, b, say, NULL ); n++; }
    if( ! k ) { snprintf( events[nev++ % EV], 220, "nothing there to mix: move the cursor onto a take" ); return; }
    }
  snprintf( events[nev++ % EV], 220, "mix: %d take(s) %s", n, c->now ? "changed at once" : "queued for their next start" );
  }

static void map_scroll( int key, int h )
  {
  int rows = h - 5; if( rows < 1 ) rows = 1;
  if( key == K_PGUP ) { mfollow = 0; mbottom -= rows - 1; }
  else if( key == K_PGDN ) { mbottom += rows - 1; if( mbottom >= mcur ) mfollow = 1; }
  else if( key == K_HOME ) { mfollow = 0; mbottom = rows - 1; }
  else if( key == K_END ) mfollow = 1;
  if( ! mfollow && mbottom < rows - 1 ) mbottom = rows - 1;
  if( mbottom > mcur ) mbottom = mcur;
  if( mbottom < 0 ) mbottom = 0;
  mdirty = 1;
  }

/* the meters: rms of the last tick of what comes in and goes out, -60 .. 0 dB, with a mark where the level peaked a moment ago */
static double mlev[4] = { -200, -200, -200, -200 }, mhold[4] = { -200, -200, -200, -200 };
static void meter_line( const char *l, void *u ) { (void)u; sscanf( l, "%lf %lf %lf %lf", mlev, mlev + 1, mlev + 2, mlev + 3 ); }
static void meter_update( int cmd )
  {
  ctlc_cmd( cmd, "METER", meter_line, NULL );
  for( int i = 0; i < 4; i++ ) { mhold[i] -= 3; if( mlev[i] > mhold[i] ) mhold[i] = mlev[i]; }
  }
static void meter_bar( int bw, double lev, double hold )
  {
  const TermCfg *tc = term_cfg(); int hp = (int)( ( hold + 60 ) / 60 * bw + 0.5 );
  for( int i = 0; i < bw; i++ )
    {
    double db = -60 + 60.0 * ( i + 1 ) / bw;
    int role = db < tc->green_below_db ? C_LOW : db > tc->red_above_db ? C_HIGH : C_MID, on = lev >= db - 60.0 / bw / 2;
    printf( "%s%c", term_sgr( on || i == hp - 1 ? role : C_TEXT ), on ? '#' : i == hp - 1 ? '|' : '.' );
    }
  }
static void meters( int r, int w )
  {
  int bw = ( w - 24 ) / 4; if( bw > 30 ) bw = 30; if( bw < 3 ) bw = 3;
  printf( "\033[%d;1H%s input  L ", r, term_sgr( C_TEXT ) ); meter_bar( bw, mlev[0], mhold[0] );
  printf( "%s  R ", term_sgr( C_TEXT ) ); meter_bar( bw, mlev[1], mhold[1] );
  printf( "%s    output  L ", term_sgr( C_TEXT ) ); meter_bar( bw, mlev[2], mhold[2] );
  printf( "%s  R ", term_sgr( C_TEXT ) ); meter_bar( bw, mlev[3], mhold[3] );
  printf( "%s\033[K", term_sgr( C_TEXT ) );
  }

static void draw( int live, int amode )
  {
  int w, h, rows; char t[300];
  term_size( &w, &h );
  if( term_resized() ) { term_cls(); mdirty = 1; }
  snprintf( t, sizeof t, "%s    %s%s", mode_word( amode ), space[0] ? space : "?", mfollow ? "" : "    (scrolled: End follows)" );
  corners( 1, w, live, t, "" );
  put( 2, C_TEXT, status, w );
  rows = h - 5; if( rows < 1 ) rows = 1;                       /* rows 3 .. h-3: the map, the latest iteration at the bottom */
  { MapSel sel = the_sel(); map_draw( &map, mbottom - rows + 1, rows, 3, w, loopT, mcur, have_cur ? &sel : NULL ); }
  put( h - 2, C_TEXT, nev ? events[( nev - 1 ) % EV] : "", w );   /* the last thing that happened, in words */
  if( nentry ) { snprintf( t, sizeof t, " > %s", entry ); put( h - 2, C_TEXT, t, w ); }       /* a command being typed */
  meters( h - 1, w );
  corners( h, w, live, "[l]ive [s]afe [g]ate [a]mode [h]istory [u]ndo [x]port [?]", "" );
  printf( "%s", term_sgr( C_TEXT ) );
  fflush( stdout );
  }

static void help( void )
  {
  term_cls();
  printf(
" a5hell: keys                                        (any key to return)\n"
"\n"
" l / s   LIVE / SAFE (record / do not)    g  gate: l learn, + more, - less\n"
" a       mode: l loop, k keep, 0-9 hold, p ping-pong     r  sync again\n"
" u / U   undo last take / iteration       f  gate profile   w  wipe loop\n"
" h       history: up/down, . next start, ! now, esc\n"
" x       export: a all, m mix, l loop     T  takes as text\n"
" PgUp PgDn Home End  scroll (End follows)      q  leave    Q  stop aspace\n"
"\n"
" the map: arrows move the cursor, shift+arrows select, esc clears.\n"
" the takes under it are underlined (all along, later rows too); keys:\n"
"   m p   mute / play, from the next start     M P   the same at once\n"
"   .     switch, next start                   !     switch at once\n"
"   [ITER[.TAKE]] [+N|-N] [/N|\\N] then . ! m p M P\n"
"   e.g.  -2/20.   +3M   \\90!   123.15m   123..\n"
"   gain (dB) and pan (%% right, \\ left) are absolute; alone, a sign keeps\n"
"   the value. a take with its own gain or pan is left to you by the automix.\n"
"   keep mode: p puts a take in the loop.\n"
"\n"
" = in the loop, - out of it; green quiet, yellow right, red peaking.\n"
" colours: ~/.local/etc/a5hell.cfg.  a5hell DIR --live --loop --dim\n" );
  fflush( stdout );
  }

/* a prompt on the bottom line; the key pressed, or 0 after ten seconds. */
static int ask( const char *q )
  {
  int w, h, k, sh;
  term_size( &w, &h );
  printf( "\033[%d;1H%s%.*s\033[K", h, term_sgr( C_TEXT ), w - 1, q ); fflush( stdout );
  for( int i = 0; i < 100; i++ ) { if( ( k = term_key( &sh ) ) != K_NONE ) return k; usleep( 100000 ); }
  return 0;
  }

int main( int argc, char **argv )
  {
  const char *dir = NULL; int nostart = 0, live_flag = 0, loop_flag = 0, dim = 0, started = 0;
  for( int i = 1; i < argc; i++ )
    {
    if( ! strcmp( argv[i], "--no-start" ) ) nostart = 1;
    else if( ! strcmp( argv[i], "--live" ) ) live_flag = 1;
    else if( ! strcmp( argv[i], "--loop" ) ) loop_flag = 1;
    else if( ! strcmp( argv[i], "--dim" ) ) dim = 1;
    else if( argv[i][0] != '-' && ! dir ) dir = argv[i];
    else dir = NULL, i = argc;
    }
  if( ! dir ) { fprintf( stderr, "usage: a5hell DIR [--live] [--loop] [--dim] [--no-start]\n  --live, --loop: how a space is started by the shell (safe and keep otherwise)\n" ); return 2; }
  if( ! isatty( 0 ) || ! isatty( 1 ) ) { fprintf( stderr, "a5hell: needs a terminal\n" ); return 1; }
  int cmd = ctlc_connect( dir ), ev;
  if( cmd < 0 && ! nostart )                                      /* not running: start it (how is a5start's business) */
    {
    char line[1400];
    fprintf( stderr, "a5hell: %s is not running; starting it\n", dir );
    snprintf( line, sizeof line, "a5start '%s'%s", dir, live_flag ? " --live" : "" );
    if( system( line ) ) { fprintf( stderr, "a5hell: could not start it\n" ); return 1; }
    cmd = ctlc_connect( dir ); started = 1;
    }
  ev = cmd >= 0 ? ctlc_connect( dir ) : -1;
  if( cmd < 0 || ev < 0 ) { fprintf( stderr, "a5hell: %s is not running\n", dir ); return 1; }
  ctlc_cmd( ev, "WATCH", NULL, NULL );
  if( started ) ctlc_cmd( cmd, loop_flag ? "AUTOMIX LOOP" : "AUTOMIX KEEP", NULL, NULL );   /* only a space this shell started is told */
  { char st[1500] = ""; ctlc_cmd( cmd, "STATE", keep_state, st ); const char *n = strstr( st, "\"name\":\"" ); if( n ) { n += 8; size_t l = strcspn( n, "\"" ); if( l > 99 ) l = 99; memcpy( space, n, l ); space[l] = 0; } }
  term_config( dim );
  term_open();
  int amode = 0, amount = 4, each = 0, live = 0, xfd = -1; pid_t xpid = 0; (void)each; (void)amount;
  for( ;; )
    {
    ctlc_cmd( cmd, "LINE", keep, NULL );
    live = strstr( status, "SAFE" ) == NULL;
    { const char *a = strstr( status, "automix " );                 /* follow what aspace says, not what this shell last sent */
      if( a ) { a += 8; if( ! strncmp( a, "overlap ", 8 ) ) { amode = 2; amount = atoi( a + 8 ); } else if( ! strncmp( a, "swap", 4 ) ) { amode = 3; each = strncmp( a + 4, " each", 5 ) == 0; } else if( ! strncmp( a, "keep", 4 ) ) amode = 1; else amode = 0; } }
    { int ww, hh; term_size( &ww, &hh ); map_update( cmd, hh ); meter_update( cmd ); if( ! have_cur ) cursor_start( ww ); }
    draw( live, amode );
    struct pollfd p[3] = { { 0, POLLIN, 0 }, { ev, POLLIN, 0 }, { xfd, POLLIN, 0 } };
    if( poll( p, 3, 250 ) <= 0 ) continue;
    if( xfd >= 0 && ( p[2].revents & ( POLLIN | POLLHUP ) ) )          /* the export reports, then ends */
      {
      char b[400]; ssize_t k = read( xfd, b, sizeof b - 1 );
      if( k > 0 ) { b[k] = 0; for( char *q = b; *q; q++ ) if( *q == '\n' ) *q = ' '; snprintf( events[nev++ % EV], 220, "export: %.200s", b ); }
      else { close( xfd ); xfd = -1; if( xpid ) waitpid( xpid, NULL, 0 ); xpid = 0; }
    }
    if( p[1].revents & POLLIN )
      {
      char b[1200];
      if( ctlc_readline( ev, b, sizeof b, 10 ) < 0 ) break;
      mdirty = 1;
      { char pr[240]; if( pretty( b, pr, sizeof pr ) ) snprintf( events[nev++ % EV], 220, "%.200s", pr ); }
      }
    if( p[0].revents & POLLIN )
      {
      int k, sh;
      if( ( k = term_key( &sh ) ) == K_NONE ) continue;
      char c[100] = "";
      { /* the map: arrows move the cursor (shift selects), digits + - / \\ start a command, . ! m p M P end it (alone, they act on what is under the cursor) */
        int ww, hh; term_size( &ww, &hh );
        if( k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT ) { cursor_move( k, sh, ww, hh ); continue; }
        if( k == K_ESC ) { nentry = 0; entry[0] = 0; anc_it = cur_it; anc_cell = cur_cell; mdirty = 1; continue; }
        if( k == K_BS && nentry ) { entry[--nentry] = 0; continue; }
        if( k < 256 && ( isdigit( k ) || strchr( "+-/\\", k ) || strchr( ".!mMpP", k ) ) && nentry < (int)sizeof entry - 2 )
          {
          MixCmd mc; int r;
          entry[nentry++] = (char)k; entry[nentry] = 0;
          r = a5mix_parse( entry, &mc );
          if( r == MP_DONE ) mix_run( cmd, &mc, ww );
          else if( r == MP_BAD ) snprintf( events[nev++ % EV], 220, "not understood: %s", entry );
          if( r != MP_WAIT ) { nentry = 0; entry[0] = 0; mdirty = 1; }
          continue;
          }
      }
      switch( k )
        {
        case K_PGUP: case K_PGDN: case K_HOME: case K_END: { int ww, hh; term_size( &ww, &hh ); map_scroll( k, hh ); break; }
        case 'l': strcpy( c, "LIVE" ); break;
        case 's': strcpy( c, "SAFE" ); break;
        case 'r': strcpy( c, "SYNC" ); break;
        case 'g':
          {
          int d = ask( " gate:  l learn the floor again (when nothing is playing)   + more sensitive   - less sensitive (3 dB)" );
          if( d == 'l' ) strcpy( c, "RELEARN" );
          else if( d == '+' || d == '=' ) strcpy( c, "GATE -3" );  /* a lower margin hears more */
          else if( d == '-' ) strcpy( c, "GATE +3" );
          break;
          }
        case '?': help(); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
        case 'a':
          {
          /* a, then one key: l loop, k keep, 0 hold off (= loop), 1-9 hold n iterations (overlap), p ping-pong panning (swap) */
          char d = 0; int w = 0;
          term_cls(); printf( " mode:  l loop   k keep   0 hold off   1-9 hold n iterations   p ping-pong pan   (now: %s)", amode == 2 ? "hold" : amode == 3 ? "ping-pong" : amode == 1 ? "keep" : "loop" ); fflush( stdout );
          while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
          if( d == 'l' || d == '0' ) strcpy( c, "AUTOMIX LOOP" );
          else if( d == 'k' ) strcpy( c, "AUTOMIX KEEP" );
          else if( d == 'p' ) strcpy( c, "AUTOMIX SWAP" );
          else if( d >= '1' && d <= '9' ) snprintf( c, sizeof c, "AUTOMIX OVERLAP %d", d - '0' );
          break;
          }
        case 'T': term_cls(); ctlc_cmd( cmd, "TAKES", show, NULL ); printf( "\n(any key)" ); fflush( stdout ); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
        case 'u': case 'U':
          if( amode == 1 ) { snprintf( events[nev++ % EV], 220, "keep mode: undo has no effect (p / m put a take in or out of the loop)" ); break; }
          snprintf( c, sizeof c, "UNDO %s", k == 'u' ? "TAKE" : "ITER" );
          break;
        case 'x':
          {
          char d = 0; int w = 0;
          term_cls(); printf( " export:  a all takes   m the mix (takes in the loop)   l the loop as it sounds   anything else cancels" ); fflush( stdout );
          while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
          const char *what = d == 'a' ? "all" : d == 'm' ? "mix" : d == 'l' ? "loop" : NULL;
          if( ! what ) break;
          if( xfd >= 0 ) { snprintf( events[nev++ % EV], 220, "an export is still running" ); break; }
          int pf[2];
          if( pipe( pf ) ) break;
          xpid = fork();
          if( xpid == 0 )                                             /* the real work is a5export, which runs at low priority */
            {
            dup2( pf[1], 1 ); dup2( pf[1], 2 ); close( pf[0] ); close( pf[1] ); close( ev ); close( cmd );
            execlp( "a5export", "a5export", dir, what, (char *)NULL );
            printf( "could not run a5export\n" ); _exit( 127 );
            }
          close( pf[1] );
          if( xpid < 0 ) { close( pf[0] ); xpid = 0; break; }
          xfd = pf[0];
          snprintf( events[nev++ % EV], 220, "exporting (%s)...", what );
          break;
          }
        case 'f':
          {
          /* the profiles are made elsewhere (calibration); here one is picked from the list */
          char names[20][80]; int np = 0;
          term_cls(); printf( " profiles:\n\n" );
          { int fd = cmd; char b[300]; snprintf( b, sizeof b, "PROFILES\n" ); if( send( fd, b, strlen( b ), 0 ) > 0 )
              { int r; while( ( r = ctlc_readline( fd, b, sizeof b, 3000 ) ) > 0 && strcmp( b, "OK" ) && strncmp( b, "ERR", 3 ) )
                  { if( np < 20 ) { snprintf( names[np], 80, "%.79s", b ); names[np][strcspn( names[np], "\t" )] = 0; printf( "  %d  %s\n", ++np, b ); } } } }
          if( ! np ) printf( "  (none; a5pace profiles install)\n" );
          printf( "\n number to choose, anything else to cancel: " ); fflush( stdout );
          { char d; int w = 0; while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
            if( d >= '1' && d < '1' + np ) snprintf( c, sizeof c, "PROFILE %s", names[d - '1'] ); }
          break;
          }
        case 'w':
          {
          char d = 0; int w = 0;
          term_cls(); printf( " wipe the loop?  y = the recorded takes   c = everything, clicks too   anything else cancels  (a revert can bring them back)" ); fflush( stdout );
          while( read( 0, &d, 1 ) != 1 && w++ < 100 ) usleep( 100000 );
          if( d == 'y' ) strcpy( c, "WIPE" ); else if( d == 'c' ) strcpy( c, "WIPE ALL" );
          break;
          }
        case 'h':
          {
          /* the places to return to, newest first. up / down to a row, . to put the loop back as it was then (each take's change
             waits for its next start), ! at once. nothing is rebuilt and it works live; esc leaves */
          char rows[40][300]; long long seqs[40]; int nr = 0, r, cur = 0, done = 0;
          { char b[400]; snprintf( b, sizeof b, "HISTORY 30\n" ); if( send( cmd, b, strlen( b ), 0 ) > 0 )
              while( ( r = ctlc_readline( cmd, b, sizeof b, 3000 ) ) > 0 && strcmp( b, "OK" ) && strncmp( b, "ERR", 3 ) )
                if( nr < 40 ) { char *t1 = strchr( b, '\t' ); if( ! t1 ) continue; seqs[nr] = atoll( t1 + 1 ); char *t2 = strchr( t1 + 1, '\t' ); snprintf( rows[nr], 300, "%.*s  %s", (int)( t1 - b ), b, t2 ? t2 + 1 : "" ); nr++; } }
          while( ! done )
            {
            term_cls(); printf( " history (newest first), the loop as it was after each:\n\n" );
            for( int i = 0; i < nr; i++ ) printf( "%s %s%s\n", i == cur ? "\033[7m>" : " ", rows[i], i == cur ? "\033[0m" : "" );
            if( ! nr ) printf( "  (nothing yet)\n" );
            printf( "\n up/down choose    .  put the loop back to this, from each take's next start    !  at once    esc  leave" ); fflush( stdout );
            char d = 0; 
            while( read( 0, &d, 1 ) != 1 ) usleep( 50000 );
            if( d == '\033' )
              {
              char e1 = 0, e2 = 0; usleep( 30000 );
              if( read( 0, &e1, 1 ) == 1 && e1 == '[' && read( 0, &e2, 1 ) == 1 ) { if( e2 == 'A' && cur > 0 ) cur--; else if( e2 == 'B' && cur < nr - 1 ) cur++; }
              else done = 1;
              }
            else if( ( d == '.' || d == '!' ) && nr )
              {
              snprintf( c, sizeof c, "RESTORE %lld%s", seqs[cur], d == '!' ? " NOW" : "" );
              snprintf( events[nev++ % EV], 220, "putting the loop back as at row %d%s", cur + 1, d == '!' ? " (now)" : " (from each take's next start)" );
              done = 1;
              }
            else if( d == 'q' ) done = 1;
            }
          break;
          }
        case 'Q': ctlc_cmd( cmd, "QUIT", NULL, NULL ); return 0;
        case 'q': return 0;
        }
      mdirty = 1;
      if( c[0] ) ctlc_cmd( cmd, c, quiet, NULL );
      }
    }
  return 0;
  }
