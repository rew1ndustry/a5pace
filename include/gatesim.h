/* gatesim: run a stretch of input through the gate and the core's take logic (open on sound, hold ten ticks of
   quiet, close, and judge the event a bump or not), and say which events it would have kept. no store, no audio:
   this is how a setting is tried against a capture (a5tune), and it must agree with the core. */
#ifndef ASPGATESIM_H
#define ASPGATESIM_H
#include <stdint.h>

typedef struct { double gate, ring, lowcut; int strict; } AspGateParams;
typedef struct
  {
  int64_t start, end;          /* frames from the start of the input */
  double  share;               /* of the power while it was open, the share above the lowcut */
  int     kept;                /* 0: judged a bump and not kept */
  double  peak_db;             /* dBFS */
  } AspGateEvent;

void asp_gateparams_default( AspGateParams *p );
/* pcm: interleaved stereo int32. returns the number of events (up to max), kept or not, in order */
int  asp_gatesim( const int32_t *pcm, int64_t frames, const AspGateParams *p, AspGateEvent *ev, int max );
#endif
