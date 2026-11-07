/* a5map: the map of a5hell. one row an iteration, the whole loop across; a take is drawn where it was played, running on into
   the next rows if it runs past the loop's end. levels come from the control port's MAP (see docs/SHELL.md). */
#ifndef A5MAP_H
#define A5MAP_H
typedef struct { long iter, off, frames; int st, n, cap; unsigned char *pk; } MapTake;   /* st: 0 out of the loop, 1 in, 2 being recorded */
typedef struct { MapTake *t; int n, cap; } Map;

void map_clear( Map *m );
void map_line( const char *line, void *map );       /* a MAP reply line (a ctlc_cmd callback) */
/* cells: ticks per cell and characters per cell for a loop of T ticks in w columns; returns the cells in a row */
int  map_scale( int T, int w, int *ticks_per_cell, int *chars_per_cell );
/* the cursor and the selection: a rectangle of iterations and cells (cell -1 is the label: a whole iteration), and the cursor in it */
typedef struct { long r1, r2, cur_it; int c1, c2, cur_cell; } MapSel;
/* what is on screen: nmain iterations from top, then, if npin, a blank row and the pinned iterations (sorted). all of it can be edited */
typedef struct { long top; int nmain; const long *pin; int npin; } MapView;
int  map_view_rows( const MapView *v );
int  map_iter_ok( const MapView *v, long it );
/* the geometry of a map whose widest label is that of iteration wide: label width, cells in a row, ticks and characters a cell */
int  map_geom( int T, int w, long wide, int *lw, int *tpc, int *cpc );
/* the takes (indexes into m->t) the selection touches; the take being recorded is left out, it cannot be mixed yet */
int  map_affected( const Map *m, const MapSel *sel, const MapView *v, int T, int w, long wide, int *idx, int max );
/* the cells where takes begin in iteration it, sorted; and the next iteration up (dir > 0) or down that has a take in it */
int  map_starts( const Map *m, const MapView *v, int T, int w, long wide, long it, int *out, int max );
long map_step( const Map *m, const MapView *v, int T, long it, int dir );
/* draw the view from screen row r0, w columns wide. cur: the iteration being played (bold label). sel (may be NULL): the cursor
   is a reversed block, the selection reversed, every cell of every affected take underlined */
void map_draw( const Map *m, const MapView *v, int r0, int w, int T, long cur, const MapSel *sel, long wide );
#endif
