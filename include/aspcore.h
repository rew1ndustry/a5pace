/* aspcore: the tick loop. a pure function over buffers: asp_core_tick takes one tick of input
   (ASP_TICK stereo frames of int32) and returns one tick of output. no devices, no clocks, no
   threads; the edge (aspface, an adapter, a file reader) calls it once per tick, in lockstep.
     phase SYNC  plays the sync tone and hunts the round trip (sync.h)
     phase PLAY  plays the loop ahead by the latency, gates the input, records takes into the
                 store (streamed to flac) and feeds them into the tank as they arrive
   everything that happens is logged to the store. see docs/EDGE.md and docs/DATA.md */
#ifndef ASPCORE_H
#define ASPCORE_H
#include "aspstore.h"
#include "asptank.h"
#include "sync.h"

typedef struct AspCore AspCore;

/* automix: what happens to takes as the loop runs. one at a time. */
enum { ASP_AUTOMIX_OFF = 0,
       ASP_AUTOMIX_FADE,       /* the loop gain steps down n * 6 dB each loop, until the loop is at the noise floor, and stays */
       ASP_AUTOMIX_OVERLAP,    /* a take goes out of the loop when n newer takes have overlapped it */
       ASP_AUTOMIX_SWAP };     /* successive takes swap channels (mono takes: hard left, hard right) */
typedef struct { int mode, n, each, restore; } AspAutomix;   /* each: SWAP also within an iteration; restore: FADE goes back to full gain when a take starts */

typedef struct
  {
  AspSyncCfg sync;
  double gate_db, ring_db;     /* gate margin, ring rule; 0 keeps the defaults (18, 7.5) */
  int    live;                 /* record takes (default 1); 0 starts safe */
  int    feed;                 /* put each take into the loop as it is recorded (default 1) */
  int    mono_detect;          /* record a take from one channel (or two identical ones) as mono (default 1) */
  AspAutomix automix;
  } AspCoreCfg;

typedef struct
  {
  int      phase;              /* 0 sync, 1 play */
  int64_t  tick, origin, latency, hush;
  uint64_t iter;
  int      live, take_open;
  int64_t  take_iter, take_off, take_frames;
  double   sync_level_db;
  int      loop_takes, jobs, gain_steps;     /* takes in the loop; level moves still being made */
  AspAutomix automix;
  double   gate_db, ring_db; int gate_learnt;
  int64_t  log_seq, loop_frames, peak;
  } AspCoreStatus;

void     asp_core_defaults( AspCoreCfg *c );
AspCore *asp_core_open( AspStore *s, AspTank *t, const AspCoreCfg *c, char *err, int errn );
int      asp_core_close( AspCore *c );             /* ends a take in progress, clears the tank's dirty flag */
int      asp_core_tick( AspCore *c, const int32_t *in, int32_t *out );
void     asp_core_status( const AspCore *c, AspCoreStatus *st );

/* control events, all logged */
void     asp_core_safe( AspCore *c, int on );      /* SAFE: no takes are recorded; an open one is ended */
void     asp_core_resync( AspCore *c );            /* SYNC: hunt the round trip again */
void     asp_core_edge( AspCore *c, int starve, int64_t frames );   /* the edge dropped or starved samples */
void     asp_core_gate( AspCore *c, double gate_db, double ring_db );
/* go back to how things were at a log point: only while safe. appends a revert event, rebuilds the loop from the
   store (which takes a moment: an edge that is not read meanwhile will report a drop). returns the revert's seq */
int64_t  asp_core_revert( AspCore *c, int64_t seq );
/* the loop gain, in steps of 6 dB down (relative adds to where it is). whenever it changes, the next output tick
   fades from the old gain to the new, so it never clicks. kept in the store, so a revert restores it */
void     asp_core_gain( AspCore *c, int steps, int relative );
void     asp_core_automix( AspCore *c, int mode, int n, int each );
void     asp_core_gate_relearn( AspCore *c );      /* learn the floor again, once no take is open */
#endif
