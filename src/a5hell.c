/* a5hell DIR [--live] [--loop] [--dim] [--no-start]: a terminal shell for a running aspace. fills the terminal: the state in
   the corners, what has just happened in words, a few keys; press ? for all of them. if the space is not running it is
   started first (a5start; --no-start to not), safe and in keep mode unless --live / --loop.
     l  LIVE   s  SAFE   h  history: go back to a place     w  wipe the loop     a  mode: al ak a0 a1-9 ap
     u  undo the last take   U  the last iteration   x  export (a5export)   t  mixing
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
#include "a5term.h"

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

static void draw( int live, int amode )
  {
  int w, h, n, rows; char t[300];
  term_size( &w, &h );
  if( term_resized() ) term_cls();
  snprintf( t, sizeof t, "%s    %s", mode_word( amode ), space[0] ? space : "?" );
  corners( 1, w, live, t, "" );
  put( 2, C_TEXT, status, w );
  rows = h - 4; if( rows > EV ) rows = EV;                    /* rows 3 .. h-2: what has just happened, newest last */
  n = nev < rows ? nev : rows;
  for( int i = 0; i < rows; i++ )
    {
    int k = i - ( rows - n );                                 /* a short log sits at the bottom of its rows */
    put( 3 + i, C_TEXT, k >= 0 ? events[( nev - n + k ) % EV] : "", w );
    }
  put( h - 1, C_TEXT, "", w );
  corners( h, w, live, "[l]ive [s]afe [g]ate [a]mode [h]istory [u]ndo [x]port [t]mix [?]", "" );
  printf( "%s", term_sgr( C_TEXT ) );
  fflush( stdout );
  }

static void help( void )
  {
  term_cls(); printf( ""
" a5hell: keys                                                  (any key to return)\n\n"
" recording\n"
"   l / s   LIVE / SAFE    safe: nothing is recorded, the loop keeps playing. red LIVE, green SAFE\n"
"   r       sync again     play the sync tones again and re-measure the round trip\n"
"   g       gate           then  l  learn the noise floor again (nothing playing)\n"
"                          +  more sensitive   -  less sensitive, 3 dB a press\n"
"   f       profile        choose a gate profile made with a5cal\n\n"
" the loop\n"
"   u / U   undo           the last take / every take of the last iteration: out of the loop, and anything\n"
"                          the automix took out because of it comes back. heard from the next start.\n"
"                          no effect in keep mode, where m / M mix or mute the most recent take instead\n"
"   x       export         a5export: a all takes, m the mix, l the loop, into DIR/export\n"
"   w       wipe           y = the recorded takes out of the loop, c = everything\n"
"                          including clicks. nothing is deleted\n"
"   h       history        the moments the loop changed, newest first. up / down to a row,\n"
"                          .  puts the loop back as it was then, from each take's next start,  !  at once\n"
"   t       mixing         iterations x takes: mute, play, gain and pan (below)\n"
"   T       takes          every take and its state, as text\n\n"
" mixing (t; ESC to leave)\n"
"   arrows move, shift+arrows select; the left column is a whole iteration\n"
"   [ITER[.TAKE]] [+N|-N] [/N|\\N] then  .  queue   !  now   m p  mute / play   M P  now\n"
"   123.15-2/20.   take 123.15, gain -2 dB, pan 20%% right, from its next start\n"
"   123.15\\90!    pan 90%% left, at once       123m  mute all of iteration 123\n"
"   123..          toggle every take of 123      m  p  .  on what is selected\n"
"   gain and pan are absolute; a sign or slash with no number keeps the current value.\n"
"   a take given a gain or pan is left alone by the automix. A shows muted iterations.\n\n"
" mode (one at a time; a, then one key)\n"
"   al        loop         every take goes into the loop (the usual)\n"
"   ak        keep         takes are recorded and kept, but not put into the loop\n"
"   a0        hold off     same as al: takes stay in the loop\n"
"   a1 - a9   hold n       a take leaves the loop when n newer takes have overlapped it\n"
"   ap        ping-pong    successive takes swap channels (pan left, then right)\n\n"
" q  leave this shell, aspace carries on       Q  stop aspace\n\n"
" the top line: LIVE or SAFE and the mode, then PLAY or SYNC, time, place in the loop, iteration,\n"
" takes in the loop, REC while a take is open, gate, round trip, profile. below, in words,\n"
" what has just happened. the bottom line: the keys.\n"
" a5hell DIR --live --loop --dim: --live and --loop apply when the shell starts the space\n"
" (safe and keep otherwise); --dim is the quiet colour scheme. colours: ~/.local/etc/a5hell.cfg\n" );
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
      { char pr[240]; if( pretty( b, pr, sizeof pr ) ) snprintf( events[nev++ % EV], 220, "%.200s", pr ); }
      }
    if( p[0].revents & POLLIN )
      {
      int k, sh;
      if( ( k = term_key( &sh ) ) == K_NONE ) continue;
      char c[100] = "";
      switch( k )
        {
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
        case 't': a5mixer( cmd, ev, space ); break;
        case 'T': term_cls(); ctlc_cmd( cmd, "TAKES", show, NULL ); printf( "\n(any key)" ); fflush( stdout ); { char d; while( read( 0, &d, 1 ) != 1 ) usleep( 50000 ); } break;
        case 'u': case 'U':
          if( amode == 1 ) { snprintf( events[nev++ % EV], 220, "keep mode: undo has no effect (m / M mix or mute the most recent take)" ); break; }
          snprintf( c, sizeof c, "UNDO %s", k == 'u' ? "TAKE" : "ITER" );
          break;
        case 'm': case 'M':                                           /* keep mode: mix or mute the most recent take, queued / now */
          if( amode != 1 ) break;
          strcpy( c, k == 'm' ? "LASTMIX" : "LASTMIX NOW" );
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
      if( c[0] ) ctlc_cmd( cmd, c, quiet, NULL );
      }
    }
  return 0;
  }
