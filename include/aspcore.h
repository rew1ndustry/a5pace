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
enum { ASP_AUTOMIX_OFF = 0,        /* plain loop: every take goes into the loop (the default) */
       ASP_AUTOMIX_OVERLAP,    /* a take goes out of the loop when n newer takes have overlapped it */
       ASP_AUTOMIX_SWAP,       /* successive takes swap channels (mono takes: hard left, hard right) */
       ASP_AUTOMIX_KEEP };     /* record and store the takes, but do not put them into the loop */
typedef struct { int mode, n, each; } AspAutomix;   /* each: SWAP also within an iteration */

typedef struct
  {
  AspSyncCfg sync;
  double gate_db, ring_db;     /* gate margin, ring rule; 0 keeps the defaults (18, 7.5) */
  double lowcut;               /* Hz: the instrument's lowest note; nothing below opens the gate. 0 or 30: as asp4 */
  int    strict;               /* 1: one band alone over its threshold no longer opens the gate */
  double minpeak;              /* dBFS (negative), 0 = off: a take that never reaches this is not kept */
  int    live;                 /* record takes (default 1); 0 starts safe */
  int    feed;                 /* put each take into the loop as it is recorded (default 1) */
  int64_t gain;                /* output gain, asp4's my_gain (0) */
  int    mono_detect;          /* record a take from one channel (or two identical ones) as mono (default 1) */
  int    mono_lev;             /* where a mono take sits: level shift on both channels, 1 = centre (default 1) */
  AspAutomix automix;
  int64_t declared_latency;    /* frames, > 0: the round trip is declared, not measured; no sync tone, play from the first tick */
  } AspCoreCfg;

typedef struct
  {
  int      phase;              /* 0 sync, 1 play */
  int64_t  tick, origin, latency, hush;
  uint64_t iter;
  int      live, take_open;
  int64_t  take_iter, take_off, take_frames;
  double   sync_level_db;
  int      loop_takes, jobs;     /* takes in the loop; level moves still being made */
  AspAutomix automix;
  double   gate_db, ring_db, lowcut; int gate_learnt, strict;
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
/* a setup: the gate, ring, lowcut, strict, mono and automix keys of a settings text (settings.h), applied now and kept in
   the store as the state of take 0 (so a revert restores them). profile is the name it came from, for the list */
void     asp_core_settings( AspCore *c, const char *profile, const char *text );
const char *asp_core_profile( const AspCore *c );
const char *asp_core_settings_text( const AspCore *c );
/* take the recorded takes out of the loop (they stay in the store, and a revert can bring them back); all also takes out
   generated ones (the clicks, an imported file). returns how many */
int      asp_core_wipe( AspCore *c, int all );
void     asp_core_automix( AspCore *c, int mode, int n, int each );
void     asp_core_gate_options( AspCore *c, double lowcut, int strict );   /* lowcut < 0 keeps it; strict < 0 keeps it */
void     asp_core_gate_relearn( AspCore *c );      /* learn the floor again, once no take is open */
#endif
