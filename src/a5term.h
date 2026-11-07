/* a5term: the terminal for a5hell. size, raw mode, alternate screen, colours (with the config file), key decoding.
   the only file that uses termios or ioctl, so a console port is one file. */
#ifndef A5TERM_H
#define A5TERM_H

enum { K_NONE = 256, K_ESC, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_BS, K_PGUP, K_PGDN, K_HOME, K_END };
enum { C_TEXT, C_LIVE, C_SAFE, C_LOW, C_MID, C_HIGH, C_N };    /* colour roles: grey, red, green, then quiet / dialled in / peaking */

typedef struct
  {
  int green_below_db, red_above_db;     /* quiet below the first, peaking above the second, dialled in between */
  int paint;                            /* paint a black background */
  int dim;                              /* normal intensity, grey dimmed */
  int col[C_N];                         /* SGR foreground code per role */
  } TermCfg;

void term_config( int dim_flag );       /* defaults, then ~/.local/etc/a5hell.cfg (or $A5HELL_CFG); dim_flag forces dim */
const TermCfg *term_cfg( void );
int term_open( void );                  /* raw mode, alternate screen, painted; restored at exit. 0 if stdin/stdout is not a terminal */
void term_close( void );
void term_size( int *cols, int *rows ); /* now: TIOCGWINSZ, else $COLUMNS / $LINES, else 80x24 */
int term_resized( void );               /* 1 once after each SIGWINCH */
const char *term_sgr( int role );       /* the escape that sets that role's colours (and the black background) */
int term_key( int *shift );             /* one key if there is one, else K_NONE; shift set for shift+arrows */
int term_wait( int fd2, int ms );       /* wait for stdin (and fd2 if >= 0) */
void term_cls( void );                  /* clear the screen in the text colours */
#endif
