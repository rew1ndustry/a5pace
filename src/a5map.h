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
/* the cursor and the selection: a rectangle of rows (iterations) and cells, and the cursor cell in it */
typedef struct { long r1, r2, cur_it; int c1, c2, cur_cell; } MapSel;
/* the geometry of a map whose last row is iteration hi: the width of the label, the cells in a row, ticks and characters a cell */
int  map_geom( int T, int w, long hi, int *lw, int *tpc, int *cpc );
/* the takes (indexes into m->t) that touch the cells of sel; the take being recorded is left out, it cannot be mixed yet */
int  map_affected( const Map *m, const MapSel *sel, int T, int w, long hi, int *idx, int max );
/* draw rows iterations top .. top+rows-1 from screen row r0, w columns wide. cur is the current iteration (its label is bold). sel (may be NULL): the cursor is reversed, the selection too, and every cell of every affected take underlined */
void map_draw( const Map *m, long top, int rows, int r0, int w, int T, long cur, const MapSel *sel );
#endif
