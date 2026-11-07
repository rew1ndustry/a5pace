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
/* draw rows iterations top .. top+rows-1 from screen row r0, w columns wide. cur is the current iteration (its label is bold) */
void map_draw( const Map *m, long top, int rows, int r0, int w, int T, long cur );
#endif
