/* gate: ported from asp4 aspace.c (the sections tuned by the player) with the globals made state.
   decides when sound starts: band-by-band floor learning, a click rule, a ring rule, and SWITCH
   (dc steps from an electric violin's switch). pcm is interleaved stereo int32.
   to find the rationale in asp4: grep GATE and SWITCH in aspace.c. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gate.h"
#include "asptank.h"

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
# define M_SQRT1_2 0.70710678118654752440
#endif
#define HZ 48000
typedef int32_t s4_t;

static int popcount( unsigned v ) { int n = 0; while( v ) { n += v & 1; v >>= 1; } return n; }


#define GATE_WIN	1024
#define GATE_HOP	256
#define GATE_BANDS	32
#define GATE_INIT	94		/* frames, half a second to learn */
#define GATE_HANG	47		/* frames, quarter second held open */
#define GATE_PRE	2400		/* samples kept before, 50ms */
#define GATE_SPREAD	4.0		/* times a band's deviation */
#define GATE_CLICK	40.0		/* dB, peak over rms above 1kHz, alone */
#define GATE_BLK	192		/* samples, 4ms: the ring blocks */
#define GATE_LITN	3		/* of the last 4 blocks must be lit */
#define GATE_LITOF	4
#define GATE_ADAPT	0.3		/* seconds, floor time constant */
#define GATE_RISE	1.0		/* dB a second, floor creep when stuck */

#define SWITCH_HZ	15		/* dc is what is below this */
#define SWITCH_ON	-28.0		/* dBFS of dc that mutes */
#define SWITCH_OVER	2.0		/* times the rest of the signal */
#define SWITCH_REST	0.02		/* seconds, rest of signal decay */
#define SWITCH_OFF	-50.0		/* dBFS of dc that unmutes, */
#define SWITCH_CALM	4800		/* held for these samples, 100ms */
#define SWITCH_PRE	4800		/* samples muted before detection */
#define SWITCH_MAX	( 3 * HZ )	/* samples muted at most */
#define SWITCH_FADE	960		/* samples faded back in, 20ms */

typedef struct
  {
  double lo[4][2], hi[2][2];		/* biquad states */
  float  ring[GATE_WIN];		/* last samples, above 30Hz */
  int    pos, fill;
  float  last[GATE_BANDS];		/* previous frame band power */
  float  floor[GATE_BANDS], var[GATE_BANDS];
  char   was[GATE_BANDS];
  double click, sum;			/* rms above 1kHz, hop sum */
  double ybg, blk;			/* power above 30Hz, learnt; this block's sum */
  int    blkn, lit;			/* samples in the block; the last 4 blocks, a bit each */
  double heard; int64_t count;		/* power while learning, for the log */
  int    frames, hang;
  double dc[2], rest;			/* SWITCH: low pass state, rest */
  int    why;				/* which rules fired since asked: 1 click, 2 ring, 4 two bands, 8 a band twice */
  double lo30[4][2];			/* states of the 30 Hz high pass: the whole signal, to compare the in-range part with */
  double evin, evtot;			/* power above the lowcut, and above 30 Hz, while the gate was held open (one take's worth) */
  double blk30, hopy, hop30;		/* power above 30 Hz: this ring block; above the lowcut and above 30 Hz over this hop */
  int    mute, calm, fade, deaf;	/* SWITCH: samples */
  } GateChan;

typedef struct Gate
  {
  GateChan chan[2];
  int64_t seen;				/* samples fed since reset */
  char ready, learnt;
  double gate, ring;			/* dB: the margin; a block that rings, over the floor */
  double lowcut;			/* Hz: nothing below the instrument's lowest note may open the gate */
  double lob[4][5];			/* the high pass for that, per gate */
  int    firstband;			/* the first band that may count */
  int    strict;			/* 1: one band alone, twice running, no longer opens the gate */
  double dom;				/* with a lowcut: the share of the signal's power that must lie above it to open the gate */
  void (*say)( void *, const char * );
  void *user;
  } Gate;

static void gsay( Gate *g, const char *form, ... );

static double gateLoB[4][5], gateHiB[2][5], gateDcB[5];
static float  gateWin[GATE_WIN], gateCos[GATE_WIN/2], gateSin[GATE_WIN/2];
static int    gateEdge[GATE_BANDS+1], gateBands = 0;

/* rbj cookbook high pass; cascades with these q make butterworths */
static void gateBiquad( double *b, double hz, double q )
  {
  double w = 2 * M_PI * hz / HZ, a = sin( w ) / ( 2 * q ), c = cos( w );
  double a0 = 1 + a;
  b[0] = ( ( 1 + c ) / 2 ) / a0;
  b[1] = -( 1 + c ) / a0;
  b[2] = ( ( 1 + c ) / 2 ) / a0;
  b[3] = ( -2 * c ) / a0;
  b[4] = ( 1 - a ) / a0;
  }

/* rbj cookbook low pass, for SWITCH */
static void gateLowBiquad( double *b, double hz, double q )
  {
  double w = 2 * M_PI * hz / HZ, a = sin( w ) / ( 2 * q ), c = cos( w );
  double a0 = 1 + a;
  b[0] = ( ( 1 - c ) / 2 ) / a0;
  b[1] = ( 1 - c ) / a0;
  b[2] = ( ( 1 - c ) / 2 ) / a0;
  b[3] = ( -2 * c ) / a0;
  b[4] = ( 1 - a ) / a0;
  }

static double gateFilter( double *b, double *z, double x )
  {
  double y = b[0] * x + z[0];
  z[0] = b[1] * x - b[3] * y + z[1];
  z[1] = b[2] * x - b[4] * y;
  return( y );
  }

static void gateSetup( void )
  {
  static double lq[4] = { 0.50980, 0.60134, 0.89998, 2.56292 };
  static double hq[2] = { 0.54120, 1.30656 };
  int i, e = 1;
  if( gateBands ) return;
  for( i = 0; i < 4; ++i ) gateBiquad( gateLoB[i], 30, lq[i] );
  for( i = 0; i < 2; ++i ) gateBiquad( gateHiB[i], 1000, hq[i] );
  gateLowBiquad( gateDcB, SWITCH_HZ, M_SQRT1_2 );
  for( i = 0; i < GATE_WIN; ++i ) gateWin[i] = 0.5 - 0.5 * cos( 2 * M_PI * i / GATE_WIN );
  for( i = 0; i < GATE_WIN/2; ++i )
    {
    gateCos[i] = cos( 2 * M_PI * i / GATE_WIN );
    gateSin[i] = -sin( 2 * M_PI * i / GATE_WIN );
    }
  /* bin 0 is below 30Hz, bands at least 3 bins, else 30% wider */
  gateEdge[0] = e;
  while( e < GATE_WIN/2 && gateBands < GATE_BANDS )
    {
    int next = e * 13 / 10;
    if( next < e + 3 ) next = e + 3;
    if( next > GATE_WIN/2 ) next = GATE_WIN/2;
    gateEdge[++gateBands] = e = next;
    }
  }

/* in place radix 2, re and im of GATE_WIN */
static void gateFFT( float *re, float *im )
  {
  int n = GATE_WIN, i, j, k, len;
  for( i = 1, j = 0; i < n; ++i )
    {
    int bit = n >> 1;
    for( ; j & bit; bit >>= 1 ) j ^= bit;
    j ^= bit;
    if( i < j )
      {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
      }
    }
  for( len = 2; len <= n; len <<= 1 )
    {
    int step = n / len;
    for( i = 0; i < n; i += len )
      for( k = 0; k < len / 2; ++k )
	{
	float wr = gateCos[k*step], wi = gateSin[k*step];
	float *ar = re + i + k, *ai = im + i + k;
	float *br = ar + len / 2, *bi = ai + len / 2;
	float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
	*br = *ar - tr; *bi = *ai - ti;
	*ar += tr;      *ai += ti;
	}
    }
  }

/* one analysis frame, returns 1 if any band says sound */
static int gateFrame( Gate *g, GateChan *c )
  {
  float re[GATE_WIN], im[GATE_WIN], dev[GATE_BANDS];
  double a = ( (double)GATE_HOP / HZ ) / GATE_ADAPT;
  double rise = GATE_RISE * GATE_HOP / HZ;
  int i, b, many = 0, again = 0, hit = 0;

  for( i = 0; i < GATE_WIN; ++i )
    {
    re[i] = c->ring[( c->pos + i ) % GATE_WIN] * gateWin[i];
    im[i] = 0;
    }
  gateFFT( re, im );

  for( b = 0; b < gateBands; ++b )
    {
    double p = 0, ls, d, thr;
    int lo = gateEdge[b], hi = gateEdge[b+1];
    for( i = lo; i < hi; ++i ) p += re[i] * re[i] + im[i] * im[i];
    p /= hi - lo;
    /* two frames, about 10ms, steadies the estimate */
    ls = 10 * log10( ( p + c->last[b] ) / 2 + 1e-20 );
    c->last[b] = p;
    /* SWITCH: muted or just unmuted, the window holds silence, which
       is neither sound nor the floor; keep last so it is fresh after */
    if( c->deaf ) { c->was[b] = 0; continue; }

    if( c->frames < GATE_INIT )
      {
      /* floor and var collect sum and sum of squares while learning */
      if( c->frames > 0 ) { c->floor[b] += ls; c->var[b] += ls * ls; }
      continue;
      }

    dev[b] = d = ls - c->floor[b];
    thr = GATE_SPREAD * sqrt( c->var[b] );
    if( thr < g->gate ) thr = g->gate;
    if( d > thr && b >= g->firstband )
      {
      ++many;
      if( c->was[b] ) again = 1;
      c->was[b] = 1;
      }
    else c->was[b] = 0;
    }

  if( c->deaf ) return( 0 );		/* SWITCH: learn nothing */

  if( c->frames < GATE_INIT )
    {
    if( ++c->frames == GATE_INIT )
      for( b = 0; b < gateBands; ++b )
	{
	double n = GATE_INIT - 1, m = c->floor[b] / n;
	c->floor[b] = m;
	c->var[b] = c->var[b] / n - m * m + 0.25;
	}
    return( 0 );
    }

  if( g->lowcut > 30 && c->hopy < g->dom * c->hop30 ) { many = 0; again = 0; }	/* mostly below the instrument's range: a bump */
  if( many >= 2 || ( again && ! g->strict ) ) { c->hang = GATE_HANG; hit = 1; c->why |= ( many >= 2 ? 4 : 0 ) | ( again && ! g->strict ? 8 : 0 ); }

  /* learn only while shut; while open the floor may only fall, and a
     band stuck above it creeps up, so a lasting change of hum frees it */
  for( b = 0; b < gateBands; ++b )
    {
    double d = dev[b];
    if( c->was[b] ) c->floor[b] += rise;
    else if( ! c->hang ) { c->floor[b] += a * d; c->var[b] += a * ( d * d - c->var[b] ); }
    else if( d < 0 ) c->floor[b] += a * d;
    }
  if( ! c->hang ) c->click += a * ( c->sum / GATE_HOP - c->click );
  if( c->hang ) --c->hang;
  return( hit );
  }

static void gateReset( Gate *g )
  {
  gateSetup();
  memset( g, 0, sizeof( *g ) );
  }

/* SWITCH: an electric violin's switch, or a plug, puts a massive dc
   step on its channel, usually clipped at full scale, settling over a
   second or two; it opened the gate and spoiled takes (wrb1 11_14, 12_4,
   2_32, and space16s0t 29_9, which looked like loud playing clipping).

   dc here is the input through a 15Hz low pass. it is a switch when the
   dc is over -28dBFS and over twice the rest of the signal (what is left
   above, peak held for 20ms). real playing peaked at -36dBFS of dc, and
   full scale bass, sine square or saw at 20Hz up, never has dc twice the
   rest; the test is slow by 15 to 70ms, so the mute reaches back 100ms,
   into the previous tick, harmless there unless that tick already opened
   a take, which then keeps the click; 15 to 70ms of a 200ms tick, so up
   to a third of switches (12_4's switch on did)

   the channel is zeroed, in place, before the gate or the take see it,
   until dc stays under -50dBFS for 100ms (3s at most), then faded back
   in over 20ms; the gate learns nothing from the silence, and learns the
   channel's floor again for half a second after it. logs SWITCH.
   only armed while the gate waits, or in a take's first tick, which the
   switch may have opened itself; the player waits for a take to END
   before switching, so takes are never muted (26_29's playing came
   within 1dB of SWITCH_ON). a mute begun carries on regardless.

   to find it in the code: grep SWITCH. to disable: SWITCH_ON 0 */
static void gateSwitchImpl( Gate *g, int32_t *pcm, int32_t *prev, int count, int armed )
  {
  double on = pow( 10, SWITCH_ON / 20 ) * 2147483648.0;
  double off = pow( 10, SWITCH_OFF / 20 ) * 2147483648.0;
  double rel = exp( -1.0 / ( SWITCH_REST * HZ ) );
  int ch, n, k;

  if( SWITCH_ON >= 0 ) return;
  for( ch = 0; ch < 2; ++ch )
    {
    GateChan *c = g->chan + ch;
    for( n = 0; n < count; ++n )
      {
      double x = pcm[n*2+ch], dc = gateFilter( gateDcB, c->dc, x );
      double r = fabs( x - dc );
      c->rest = r > c->rest ? r : c->rest * rel;

      if( armed && ! c->mute && fabs( dc ) > on && fabs( dc ) > SWITCH_OVER * c->rest )
	{
	c->mute = 1;
	c->calm = c->fade = 0;
	for( k = n - SWITCH_PRE; k < n; ++k )
	  if( k >= 0 ) pcm[k*2+ch] = 0;
	  else if( prev ) prev[( count + k )*2+ch] = 0;
	gsay( g, "SWITCH %d muted", ch + 1 );
	}
      if( c->mute )
	{
	pcm[n*2+ch] = 0;
	c->deaf = GATE_WIN + GATE_HOP + SWITCH_FADE;
	c->calm = fabs( dc ) < off ? c->calm + 1 : 0;
	if( c->calm >= SWITCH_CALM || ++c->mute > SWITCH_MAX )
	  {
	  gsay( g, "SWITCH %d unmuted after %.2fs", ch + 1, (double)c->mute / HZ );
	  c->mute = 0;
	  c->fade = SWITCH_FADE;
	  /* the switch changed what the channel hears, a preamp's hiss
	     on or off, so learn its floor again once deaf is over */
	  c->frames = 0;
	  c->click = 0;
	  memset( c->floor, 0, sizeof( c->floor ) );
	  memset( c->var, 0, sizeof( c->var ) );
	  memset( c->was, 0, sizeof( c->was ) );
	  }
	}
      else if( c->fade )
	{
	pcm[n*2+ch] = (int32_t)( pcm[n*2+ch] * (double)( SWITCH_FADE - c->fade ) / SWITCH_FADE );
	--c->fade;
	}
      }
    }
  }

/* feed one tick, stereo, return the offset of the earliest sound found
   in it, which may be before it (negative, from the previous tick) to
   keep what led up to the detection, or ASP_TICK when there is none */
static int64_t gateScanImpl( Gate *g, int32_t *pcm, int count )
  {
  int64_t top = g->seen, best = top + count;
  double click = pow( 10, GATE_CLICK / 10 ), ring = pow( 10, g->ring / 10 );
  int ch, n;

  for( ch = 0; ch < 2; ++ch )
    {
    GateChan *c = g->chan + ch;
    for( n = 0; n < count; ++n )
      {
      double x = pcm[n*2+ch], y = x, h;
      int i;
      for( i = 0; i < 4; ++i ) y = gateFilter( g->lob[i], c->lo[i], y );
      h = y;
      for( i = 0; i < 2; ++i ) h = gateFilter( gateHiB[i], c->hi[i], h );
      double y30 = y;
      if( g->lowcut > 30 ) { y30 = x; for( i = 0; i < 4; ++i ) y30 = gateFilter( gateLoB[i], c->lo30[i], y30 ); }

      if( c->deaf ) --c->deaf;		/* SWITCH */
      if( c->frames >= GATE_INIT && ! c->deaf && h * h > click * c->click )   /* a click so loud it needs no ring */
	{
	int64_t at = top + n - GATE_PRE;
	if( at < best ) best = at;
	c->hang = GATE_HANG;
	c->why |= 1;
	}
      if( c->deaf ) c->blk = c->blkn = c->lit = 0;		/* SWITCH, silence is not the floor */
      else if( c->frames < GATE_INIT )
	{
	c->click += h * h / ( GATE_HOP * GATE_INIT );
	c->ybg += y * y / ( GATE_HOP * GATE_INIT );
	c->heard += y * y; ++c->count;
	}
      else
	{
	if( c->hang ) { c->evin += y * y; c->evtot += y30 * y30; }
	c->sum += h * h;
	c->blk += y * y;
	c->blk30 += y30 * y30;
	c->hopy += y * y;
	c->hop30 += y30 * y30;
	if( ++c->blkn == GATE_BLK )
	  {
	  double p = c->blk / GATE_BLK;
	  int lit = p > ring * c->ybg, j = GATE_LITOF - 1;
	  if( lit && g->lowcut > 30 && p < g->dom * ( c->blk30 / GATE_BLK ) ) lit = 0;	/* a bump: most of it is below the range */
	  c->blk = 0; c->blkn = 0; c->blk30 = 0;
	  c->lit = ( ( c->lit << 1 ) | lit ) & ( ( 1 << GATE_LITOF ) - 1 );
	  if( popcount( c->lit ) >= GATE_LITN )
	    {
	    /* it rings: from the oldest of the blocks that is lit */
	    int64_t at;
	    while( ! ( ( c->lit >> j ) & 1 ) ) --j;
	    at = top + n + 1 - (int64_t)GATE_BLK * ( j + 1 ) - GATE_PRE;
	    if( at < best ) best = at;
	    c->hang = GATE_HANG;
	    c->why |= 2;
	    }
	  else if( ! lit && ! c->hang && p > c->ybg * 1e-3 )
	    c->ybg += ( p - c->ybg ) * ( (double)GATE_BLK / HZ ) / GATE_ADAPT;
	  }
	}

      c->ring[c->pos] = y;
      c->pos = ( c->pos + 1 ) % GATE_WIN;
      if( ++c->fill == GATE_HOP )
	{
	c->fill = 0;
	/* ring holds the newest window, starting at pos */
	if( gateFrame( g, c ) )
	  {
	  /* the hit may come from the previous frame, smoothed in, or
	     from the one before that, for a band found twice running */
	  int64_t at = top + n + 1 - GATE_WIN - GATE_HOP * 2 - GATE_PRE;
	  if( at < best ) best = at;
	  }
	c->sum = 0; c->hopy = c->hop30 = 0;
	}
      }
    }

  g->seen += count;

  /* once both channels have learnt, say what floor they heard */
  if( ! g->learnt && g->chan[0].frames >= GATE_INIT && g->chan[1].frames >= GATE_INIT )
    {
    double f[2];
    for( ch = 0; ch < 2; ++ch )
      {
      GateChan *c = g->chan + ch;
      f[ch] = 10 * log10( ( c->count ? c->heard / c->count : 0 ) / 4.611686e18 + 1e-20 );
      }
    gsay( g, "GATE learnt, floor %.0f %.0f dBFS, margin %.0f dB", f[0], f[1], g->gate );
    g->learnt = 1;
    }

  if( best >= top + count ) return( ASP_TICK );
  best -= top;
  return( best < -ASP_TICK ? -ASP_TICK : best );
  }


static void gsay( Gate *g, const char *form, ... )
  {
  char buf[200];
  va_list ap;
  if( ! g->say ) return;
  va_start( ap, form ); vsnprintf( buf, sizeof buf, form, ap ); va_end( ap );
  g->say( g->user, buf );
  }

AspGate *asp_gate_new( void (*say)( void *, const char * ), void *user )
  {
  Gate *g = calloc( 1, sizeof *g );
  gateReset( g );
  g->gate = 18.0; g->ring = 7.5; g->say = say; g->user = user;
  asp_gate_set_lowcut( g, 30 );
  return g;
  }
void asp_gate_free( AspGate *g ) { free( g ); }
void asp_gate_reset( AspGate *g )
  { double a = g->gate, b = g->ring, l = g->lowcut; int st = g->strict; void (*s)( void *, const char * ) = g->say; void *u = g->user;
    gateReset( g ); g->gate = a; g->ring = b; g->say = s; g->user = u; g->strict = st; asp_gate_set_lowcut( g, l ); }
void asp_gate_set( AspGate *g, double gate_db, double ring_db )
  {
  if( gate_db > 0 ) g->gate = gate_db < 1 ? 1 : ( gate_db > 60 ? 60 : gate_db );
  if( ring_db > 0 ) g->ring = ring_db < 3 ? 3 : ring_db;
  }
double asp_gate_margin( AspGate *g ) { return g->gate; }
double asp_gate_ring( AspGate *g ) { return g->ring; }
int64_t asp_gate_scan( AspGate *g, int32_t *pcm, int count ) { return gateScanImpl( g, pcm, count ); }
void asp_gate_switch( AspGate *g, int32_t *pcm, int32_t *prev, int count, int armed )
  { gateSwitchImpl( g, pcm, prev, count, armed ); }

int asp_gate_active( AspGate *g, int ch ) { return g->chan[ch].hang > 0; }
int asp_gate_learnt( AspGate *g ) { return g->chan[0].frames >= GATE_INIT && g->chan[1].frames >= GATE_INIT; }
/* the learnt floor as an amplitude (rms of the signal above 30 Hz); 0 until learnt */
double asp_gate_floor( AspGate *g, int ch )
  { return g->chan[ch].frames >= GATE_INIT ? sqrt( g->chan[ch].ybg ) : 0; }

/* which rules opened the gate since this was last asked (bits: 1 click, 2 ring, 4 two bands, 8 one band twice) */
int asp_gate_why( AspGate *g )
  { int w = g->chan[0].why | g->chan[1].why; g->chan[0].why = g->chan[1].why = 0; return w; }

/* nothing below this frequency may open the gate (handling bumps, stand knocks, the room): set it just under the
   instrument's lowest fundamental. the take still records everything; only the opening decision ignores it.
   30 is the old behaviour. */
void asp_gate_set_lowcut( AspGate *g, double hz )
  {
  static const double lq[4] = { 0.50980, 0.60134, 0.89998, 2.56292 };
  int b;
  if( hz < 30 ) hz = 30;
  if( hz > 2000 ) hz = 2000;
  g->lowcut = hz; if( g->dom <= 0 ) g->dom = 0.15;
  for( int i = 0; i < 4; ++i ) gateBiquad( g->lob[i], hz, lq[i] );
  for( b = 0; b < gateBands && gateEdge[b+1] * (double)HZ / GATE_WIN <= hz; ++b ) {}
  g->firstband = hz <= 30 ? 0 : b;
  }
double asp_gate_lowcut( AspGate *g ) { return g->lowcut; }

void asp_gate_set_strict( AspGate *g, int on ) { g->strict = on != 0; }
int asp_gate_strict( AspGate *g ) { return g->strict; }

/* of the power the gate heard while it was open, the share that lay above the lowcut (1 with no lowcut); reset starts
   a new count. a take that is mostly below the instrument's range is a bump, not a note. */
double asp_gate_event_share( AspGate *g, int reset )
  {
  double in = g->chan[0].evin + g->chan[1].evin, tot = g->chan[0].evtot + g->chan[1].evtot;
  if( reset ) g->chan[0].evin = g->chan[1].evin = g->chan[0].evtot = g->chan[1].evtot = 0;
  return tot > 0 ? in / tot : 1.0;
  }
double asp_gate_dominance( AspGate *g ) { return g->dom; }
