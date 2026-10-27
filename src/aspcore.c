#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "aspcore.h"
#include "aspengine.h"
#include "gate.h"
#include "flacio.h"

#define RATE        48000
#define LEVEL_CLIP  0x7FFFFF00LL
#define HANG        10            /* ticks of silence a take keeps after the last sound */
#define BUDGET      48000         /* frames of take a tick may spend on level moves */

typedef struct
  {
  int64_t id, iter, off, len, peak;
  AspPlace now, base;             /* where it is (decided); where it was put when recorded */
  int in, mono, src, overlaps;
  int64_t ready_pass;             /* the first pass it may be faded in */
  } LoopTake;

typedef struct { AspJob *job; int64_t start, span, waited; } Move;

struct AspCore
  {
  AspStore *store;
  AspTank  *tank;
  AspCoreCfg cfg;
  AspGate  *gate;
  AspSync  *sync;
  int64_t  size;

  int      phase, live, gate_ready, gate_relearn;
  int64_t  tick, origin, latency, hush, gone, born;
  uint64_t iter, peak;
  int      newt;

  int      sync_k;
  int32_t  sync_out[2 * ASP_SYNC_FRAMES], sync_in[2 * ASP_SYNC_FRAMES];

  int32_t  cur[2 * ASP_TICK], prev[2 * ASP_TICK];
  int      have_prev;

  /* the take in progress */
  AspFlacEnc *enc;
  char     part[1200];
  int64_t  t_iter, t_sec, t_off, t_frames;
  int      t_wrapped, t_damaged, shh, t_ident;
  int64_t  t_peak[2], t_clips[2], t_act[2];

  /* takes in the loop, which the automix works on */
  LoopTake *lt; int nlt, caplt;
  int64_t  pass, outpos;
  int      swap_flip; int64_t swap_iter, swap_n;
  int      out_shift;               /* how far the last output tick was brought down to fit */

  /* level moves, run in order, a slice a tick */
  Move *mv; int nmv, capmv;
  };

static void logf_( AspCore *c, const char *type, const char *form, ... )
  {
  char buf[600];
  va_list ap;
  va_start( ap, form ); vsnprintf( buf, sizeof buf, form, ap ); va_end( ap );
  asp_log( c->store, "aspace", type, buf, c->tick );
  }

static void gate_say( void *u, const char *line )
  {
  AspCore *c = u;
  char esc[300]; int j = 0;
  for( const char *p = line; *p && j < 280; p++ ) { if( *p == '"' || *p == '\\' ) esc[j++] = '\\'; esc[j++] = *p; }
  esc[j] = 0;
  logf_( c, "gate", "{\"msg\":\"%s\"}", esc );
  }

/* --- the takes in the loop --- */

static void lev_str( int lev, char *b ) { if( lev == ASP_LEV_OFF ) strcpy( b, "-" ); else sprintf( b, "%d", lev ); }

static void state_json( const LoopTake *t, const char *status, char *buf, size_t n )
  {
  char l[8], r[8];
  lev_str( t->now.l, l ); lev_str( t->now.r, r );
  snprintf( buf, n, "{\"status\":\"%s\",\"lev\":[\"%s\",\"%s\"],\"flip\":%d,\"mono\":%d,\"src\":%d,\"len\":%lld,\"gone\":0,\"peak\":%lld}",
            status, l, r, t->now.flip, t->mono, t->src, (long long)t->len, (long long)t->peak );
  }

static LoopTake *lt_add( AspCore *c )
  {
  if( c->nlt == c->caplt ) { c->caplt = c->caplt ? 2 * c->caplt : 32; c->lt = realloc( c->lt, c->caplt * sizeof *c->lt ); }
  memset( c->lt + c->nlt, 0, sizeof *c->lt );
  return c->lt + c->nlt++;
  }

typedef struct { AspCore *c; } Load;
static void load_cb( int64_t id, const char *json, void *u )
  {
  AspCore *c = ( (Load *)u )->c;
  AspTakeState st; AspTake k;
  asp_parse_state( json, &st );
  if( asp_take_get( c->store, id, &k ) ) return;
  LoopTake *t = lt_add( c );
  t->id = id; t->iter = k.iter; t->off = k.off; t->len = st.len >= 0 ? st.len : k.frames;
  t->now = t->base = (AspPlace){ st.l, st.r, st.flip };
  t->in = strcmp( st.status, "off" ) != 0; t->mono = st.mono; t->peak = st.peak;
  if( t->in && t->peak < 0 && asp_take_peak( c->store, id, &t->peak ) ) t->peak = 0;
  }

void asp_core_defaults( AspCoreCfg *c )
  {
  memset( c, 0, sizeof *c );
  asp_sync_defaults( &c->sync );
  c->live = 1; c->feed = 1; c->mono_detect = 1; c->mono_lev = 1;
  }

AspCore *asp_core_open( AspStore *s, AspTank *t, const AspCoreCfg *cfg, char *err, int errn )
  {
  AspCoreCfg d;
  if( ! cfg ) { asp_core_defaults( &d ); cfg = &d; }
  if( ! asp_tank_bound( t, asp_session_id( s ) ) )
    { if( err ) snprintf( err, errn, "the tank belongs to another session" ); return NULL; }
  if( asp_tank_frames( t ) < 4 * ASP_TICK )
    { if( err ) snprintf( err, errn, "the loop is too short" ); return NULL; }
  AspCore *c = calloc( 1, sizeof *c );
  c->store = s; c->tank = t; c->cfg = *cfg; c->size = asp_tank_frames( t );
  c->live = cfg->live; c->iter = asp_tank_iter( t ); c->peak = asp_tank_peak( t ); c->newt = 1;
  c->gate = asp_gate_new( gate_say, c );
  asp_gate_set( c->gate, cfg->gate_db, cfg->ring_db );
  if( cfg->lowcut > 0 ) asp_gate_set_lowcut( c->gate, cfg->lowcut );
  asp_gate_set_strict( c->gate, cfg->strict );
  c->sync = asp_sync_new( &cfg->sync );
  c->phase = 0;
  if( ! asp_tank_legacy( t ) )
    {
    if( asp_tank_dirty( t ) ) logf_( c, "tank_dirty", "{}" );
    asp_tank_set_dirty( t, 1 );
    }
  Load ld = { c };
  asp_fold( s, -1, load_cb, &ld );
  if( cfg->declared_latency > 0 )                       /* declared, not measured: play from the first tick */
    {
    c->latency = cfg->declared_latency; c->origin = 0; c->hush = c->latency / ASP_TICK + 5; c->phase = 1;
    logf_( c, "sync_declared", "{\"frames\":%lld}", (long long)c->latency );
    }
  else logf_( c, "sync_start", "{\"level_db\":%.1f}", asp_sync_level( c->sync ) );
  logf_( c, "core_open", "{\"loop\":%lld,\"iter\":%llu,\"live\":%d,\"takes\":%d}", (long long)c->size, (unsigned long long)c->iter, c->live, c->nlt );
  return c;
  }

/* --- level moves --- */

static void mv_add( AspCore *c, AspJob *j )
  {
  if( ! j ) { logf_( c, "error", "{\"what\":\"level move not made\"}" ); return; }
  if( c->nmv == c->capmv ) { c->capmv = c->capmv ? 2 * c->capmv : 16; c->mv = realloc( c->mv, c->capmv * sizeof *c->mv ); }
  Move *m = c->mv + c->nmv++;
  m->job = j; m->start = asp_job_pos0( j ) % c->size; m->span = asp_job_span( j ); m->waited = 0;
  m->start = ( asp_job_pos0( j ) + 0 ) % c->size;
  }

/* a move starts when the playhead is not in the take and will not be for as long as the move takes, so the
   change is never heard part way; a take that fills the loop waits one pass, then goes */
static int mv_ready( AspCore *c, Move *m )
  {
  int64_t dur = ( m->span + BUDGET - 1 ) / BUDGET * ASP_TICK + ASP_TICK;
  int64_t ph = ( c->outpos + ASP_TICK ) % c->size;
  int64_t inside = ( ph - m->start + c->size ) % c->size, ahead = ( m->start - ph + c->size ) % c->size;
  if( m->span < c->size && inside >= m->span && ahead >= dur ) return 1;
  return m->waited >= 1;
  }

static void mv_run( AspCore *c )
  {
  int64_t budget = BUDGET;
  while( c->nmv && budget > 0 )
    {
    Move *m = c->mv;
    if( ! m->job ) { memmove( c->mv, c->mv + 1, --c->nmv * sizeof *c->mv ); continue; }
    if( asp_job_span( m->job ) > 0 && ! mv_ready( c, m ) ) break;
    int r = asp_job_step( m->job, c->tank, budget );
    budget = 0;
    if( r != 0 )
      {
      if( r < 0 ) logf_( c, "error", "{\"what\":\"level move failed\"}" );
      asp_job_free( m->job );
      memmove( c->mv, c->mv + 1, --c->nmv * sizeof *c->mv );
      budget = BUDGET / 2;                                    /* the next may start with what is left */
      }
    }
  }

static void queue_move( AspCore *c, LoopTake *t, AspPlace from, AspPlace to, int in_from, int in_to )
  {
  AspPlace f = in_from ? from : (AspPlace)ASP_PLACE_OFF, g = in_to ? to : (AspPlace)ASP_PLACE_OFF;
  if( f.l == g.l && f.r == g.r && f.flip == g.flip ) return;
  mv_add( c, asp_job_take( c->store, t->id, 0, -1, f, g ) );
}

/* --- automix --- */

static void change( AspCore *c, LoopTake *t, AspPlace to, int in, const char *why )
  {
  AspPlace from = t->now; int was = t->in;
  char st[300];
  t->now = to; t->in = in;
  state_json( t, in ? "mix" : "off", st, sizeof st );
  asp_take_set_state( c->store, t->id, st, "aspace", c->tick );
  logf_( c, "relevel", "{\"take\":%lld,\"why\":\"%s\",\"lev\":[%d,%d],\"flip\":%d,\"in\":%d}", (long long)t->id, why,
         to.l == ASP_LEV_OFF ? -1 : to.l, to.r == ASP_LEV_OFF ? -1 : to.r, to.flip, in );
  if( c->cfg.feed ) queue_move( c, t, from, to, was, in );
  }

static int overlaps( AspCore *c, const LoopTake *a, const LoopTake *b )
  {
  int64_t sz = c->size, as = a->off * RATE % sz, bs = b->off * RATE % sz;
  if( a->len >= sz || b->len >= sz ) return 1;
  return ( ( bs - as + sz ) % sz ) < a->len || ( ( as - bs + sz ) % sz ) < b->len;
  }

/* overlap: a take goes out of the loop when n newer takes have overlapped it. (fading, when it comes, is an envelope.) */
static void automix_new( AspCore *c, LoopTake *nt )
  {
  if( c->cfg.automix.mode != ASP_AUTOMIX_OVERLAP ) return;
  int n = c->cfg.automix.n < 1 ? 1 : c->cfg.automix.n;
  for( int i = 0; i < c->nlt; i++ )
    {
    LoopTake *t = c->lt + i;
    if( t == nt || ! t->in || ! overlaps( c, t, nt ) ) continue;
    if( ++t->overlaps >= n ) change( c, t, t->now, 0, "overlap" );
    }
  }

void asp_core_automix( AspCore *c, int mode, int n, int each )
  {
  c->cfg.automix.mode = mode; c->cfg.automix.n = n; c->cfg.automix.each = each;
  logf_( c, "automix", "{\"mode\":%d,\"n\":%d,\"each\":%d}", mode, n, each );
  }

/* --- takes --- */

static void take_feed( AspCore *c, const int32_t *pcm, int64_t n )
  {
  if( c->cfg.feed && n > 0 )
    asp_tank_level( c->tank, c->t_sec * RATE + c->t_frames, pcm, n, ASP_LEV_OFF, ASP_LEV_OFF, 0, 0 );
  }

/* frames of a take, from its start, counted so that frame 0 is the start of its second of the loop */
static int take_put( AspCore *c, const int32_t *pcm, int64_t n )
  {
  int rc = 0;
  for( int64_t i = 0; i < n; i += 4096 )
    {
    int64_t k = n - i < 4096 ? n - i : 4096;
    rc |= asp_flac_enc_write( c->enc, pcm + 2 * i, (unsigned)k );
    }
  for( int64_t i = 0; i < n && c->t_ident; i++ ) if( pcm[2*i] != pcm[2*i+1] ) c->t_ident = 0;
  take_feed( c, pcm, n );
  c->t_frames += n;
  return rc;
  }

static void level_scan( AspCore *c, const int32_t *pcm, int64_t n )
  {
  for( int64_t i = 0; i < 2 * n; i++ )
    {
    int64_t v = llabs( (int64_t)pcm[i] );
    if( v > c->t_peak[i & 1] ) c->t_peak[i & 1] = v;
    if( v >= LEVEL_CLIP ) ++c->t_clips[i & 1];
    }
  }

static double level_db( int64_t p ) { return p ? 20 * log10( p / 2147483648.0 ) : -200; }

/* the take is over: decide mono and where it sits, store it, and bring the loop to match */
static void take_end( AspCore *c, const int32_t *last )
  {
  AspTake tk;
  if( last ) { level_scan( c, last, ASP_TICK ); take_put( c, last, ASP_TICK ); }
  int bad = asp_flac_enc_close( c->enc );
  c->enc = NULL;

  /* mono: one channel never rang, or both are the same */
  int mono = 0, src = 0;
  if( c->cfg.mono_detect && ! c->t_damaged )
    {
    int64_t a = c->t_act[0], b = c->t_act[1];
    if( c->t_ident ) { mono = 1; src = 0; }
    else if( a > 0 && b * 20 <= a ) { mono = 1; src = 0; }
    else if( b > 0 && a * 20 <= b ) { mono = 1; src = 1; }
    }
  int swap = c->cfg.automix.mode == ASP_AUTOMIX_SWAP;
  if( swap && c->swap_n++ && ( c->cfg.automix.each || (int64_t)c->t_iter != c->swap_iter ) ) c->swap_flip = ! c->swap_flip;   /* the first take goes left / unflipped */
  c->swap_iter = c->t_iter;

  AspPlace live = { 0, 0, 0 }, fin = { 0, 0, 0 };
  if( mono ) { if( swap ) fin = c->swap_flip ? (AspPlace){ ASP_LEV_OFF, 0, 0 } : (AspPlace){ 0, ASP_LEV_OFF, 0 };
               else fin = (AspPlace){ c->cfg.mono_lev, c->cfg.mono_lev, 0 }; }
  else if( swap ) fin.flip = c->swap_flip;

  LoopTake lt = { 0 };
  lt.iter = c->t_iter; lt.off = c->t_off; lt.len = c->t_frames; lt.mono = mono; lt.src = src;
  lt.peak = c->t_peak[0] > c->t_peak[1] ? c->t_peak[0] : c->t_peak[1];
  lt.now = lt.base = fin; lt.in = c->cfg.feed;
  char st[300];
  state_json( &lt, lt.in ? "mix" : "off", st, sizeof st );

  int32_t *stereo = NULL; int64_t sn = 0; int sch = 0;
  char part2[1200]; part2[0] = 0;
  if( ! bad && mono )                                       /* keep the stereo for the move, store one channel */
    {
    if( asp_pcm_load( c->part, &stereo, &sn, &sch ) == 0 && sch == 2 && ! asp_tmp_path( c->store, part2, sizeof part2 ) )
      {
      AspFlacEnc *e = asp_flac_enc_open( part2, 1, RATE );
      int32_t *m = malloc( (size_t)sn * 4 );
      for( int64_t i = 0; i < sn; i++ ) m[i] = stereo[2*i+src];
      for( int64_t i = 0; e && i < sn; i += 4096 ) asp_flac_enc_write( e, m + i, (unsigned)( sn - i < 4096 ? sn - i : 4096 ) );
      free( m );
      if( e && asp_flac_enc_close( e ) ) part2[0] = 0;
      }
    else part2[0] = 0;
    if( ! part2[0] ) { mono = 0; lt.mono = 0; fin = live; lt.now = lt.base = fin; state_json( &lt, lt.in ? "mix" : "off", st, sizeof st ); free( stereo ); stereo = NULL; }
    }

  const char *use = mono && part2[0] ? part2 : c->part;
  if( bad || asp_take_adopt( c->store, c->t_iter, c->t_off, use, lt.in ? "mix" : "off", st, c->tick, &tk ) )
    {
    logf_( c, "error", "{\"what\":\"take not stored\",\"iter\":%lld,\"off\":%lld,\"part\":\"%s\"}",
           (long long)c->t_iter, (long long)c->t_off, use );
    free( stereo );
    }
  else
    {
    if( use == part2 ) unlink( c->part );
    lt.id = tk.id;
    LoopTake *t = lt_add( c );
    *t = lt;
    t->ready_pass = c->pass + 1;
    if( c->cfg.feed )
      {
      if( mono && stereo )                                  /* the live feed was stereo: swap it for the mono at its place */
        {
        int32_t *m = malloc( (size_t)sn * 4 );
        for( int64_t i = 0; i < sn; i++ ) m[i] = stereo[2*i+src];
        mv_add( c, asp_job_new( c->t_sec * RATE, 0, -1, stereo, sn, 2, live, m, sn, 1, fin ) );
        stereo = NULL;
        }
      else if( fin.l != live.l || fin.r != live.r || fin.flip != live.flip )
        mv_add( c, asp_job_take( c->store, tk.id, 0, -1, live, fin ) );
      }
    free( stereo );
    logf_( c, "end", "{\"take\":%lld,\"iter\":%lld,\"off\":%lld,\"len\":%lld,\"peak_db\":[%.1f,%.1f],\"clips\":[%lld,%lld],\"damaged\":%d,\"mono\":%d,\"src\":%d,\"flip\":%d}",
           (long long)tk.id, (long long)c->t_iter, (long long)c->t_off, (long long)c->t_frames,
           level_db( c->t_peak[0] ), level_db( c->t_peak[1] ), (long long)c->t_clips[0], (long long)c->t_clips[1],
           c->t_damaged, mono, src, fin.flip );
    automix_new( c, t );
    }
  c->gone = c->tick;
  }

static int take_new( AspCore *c, int64_t off, int64_t scan, int wrapped )
  {
  AspTake dummy;
  int64_t sec = off / RATE, mod = off % RATE;
  int tries = 1000;
  while( tries-- && ! asp_take_find( c->store, (int64_t)c->iter, sec, &dummy ) )
    {
    ++c->iter;                                           /* that take exists: the next iteration */
    asp_tank_set_head( c->tank, c->iter, c->peak );
    }
  if( asp_tmp_path( c->store, c->part, sizeof c->part ) ) return -1;
  c->enc = asp_flac_enc_open( c->part, 2, RATE );
  if( ! c->enc ) return -1;
  c->t_iter = (int64_t)c->iter; c->t_sec = sec; c->t_off = sec; c->t_frames = 0;
  c->t_wrapped = wrapped; c->t_damaged = 0; c->shh = 0; c->t_ident = 1;
  memset( c->t_peak, 0, sizeof c->t_peak ); memset( c->t_clips, 0, sizeof c->t_clips ); memset( c->t_act, 0, sizeof c->t_act );
  {
  int32_t z[2 * 4096] = { 0 };                           /* frame 0 of a take is the start of its second */
  for( int64_t n = mod; n > 0; n -= 4096 )
    { int64_t k = n < 4096 ? n : 4096; asp_flac_enc_write( c->enc, z, (unsigned)k ); c->t_frames += k; }
  }
  if( scan < 0 && c->have_prev )                         /* a pre-roll reaches back into the tick before */
    {
    level_scan( c, c->prev + 2 * ( ASP_TICK + scan ), -scan );
    take_put( c, c->prev + 2 * ( ASP_TICK + scan ), -scan );
    scan = 0;
    }
  level_scan( c, c->cur + 2 * scan, ASP_TICK - scan );
  take_put( c, c->cur + 2 * scan, ASP_TICK - scan );
  c->born = c->tick;
  logf_( c, "new", "{\"iter\":%lld,\"off\":%lld,\"wrapped\":%d}", (long long)c->t_iter, (long long)c->t_off, wrapped );
  return 0;
  }

/* --- phases --- */

static void sync_begin( AspCore *c )
  {
  if( c->sync ) asp_sync_free( c->sync );
  c->sync = asp_sync_new( &c->cfg.sync );
  c->sync_k = 0;
  c->phase = 0;
  logf_( c, "sync_start", "{\"level_db\":%.1f}", asp_sync_level( c->sync ) );
  }

static void sync_tick( AspCore *c, const int32_t *in, int32_t *out )
  {
  int k = c->sync_k;
  if( k == 0 ) asp_sync_out( c->sync, c->sync_out );
  memcpy( out, c->sync_out + (size_t)k * 2 * ASP_TICK, sizeof( int32_t ) * 2 * ASP_TICK );
  memcpy( c->sync_in + (size_t)k * 2 * ASP_TICK, in, sizeof( int32_t ) * 2 * ASP_TICK );
  if( k == 4 )
    {
    int r = asp_sync_in( c->sync, c->sync_in );
    const AspSyncResult *res = asp_sync_result( c->sync );
    if( r == 1 )
      {
      c->latency = res->frames;
      c->origin = c->tick + 1;                           /* the loop starts at its top on the next tick */
      c->hush = c->origin + c->latency / ASP_TICK + 5;   /* the sync still in flight comes back first */
      c->phase = 1; c->gate_ready = 0; c->have_prev = 0; c->newt = 1;
      logf_( c, "sync_found", "{\"frames\":%lld,\"inverted\":%d,\"channel\":%d,\"out_db\":%.1f,\"in_peak_db\":%.1f,\"seconds\":%d,\"spread\":%d}",
             (long long)res->frames, res->inverted, res->channel, res->out_db, res->in_peak_db, res->seconds, res->spread );
      return;
      }
    if( r < 0 )
      {
      logf_( c, "sync_failed", "{\"seconds\":%d,\"spread\":%d}", res->seconds, res->spread );
      sync_begin( c );                                   /* start the hunt again from the quiet end */
      return;
      }
    }
  c->sync_k = ( k + 1 ) % 5;
  }

static void play_tick( AspCore *c, const int32_t *in, int32_t *out )
  {
  int64_t rel = c->tick - c->origin, recpos = ( rel * ASP_TICK ) % c->size, scan = ASP_TICK;
  int track = c->enc != NULL;
  memcpy( c->cur, in, sizeof c->cur );

  if( c->gate_relearn && ! track ) { c->gate_relearn = 0; c->gate_ready = 0; logf_( c, "gate_reset", "{}" ); }
  if( c->tick > c->hush )
    {
    if( ! c->gate_ready ) { asp_gate_reset( c->gate ); c->gate_ready = 1; logf_( c, "gate", "{\"msg\":\"learning\"}" ); }
    asp_gate_switch( c->gate, c->cur, c->have_prev ? c->prev : NULL, ASP_TICK, ! track || c->born + 1 >= c->tick );
    scan = asp_gate_scan( c->gate, c->cur, ASP_TICK );
    if( track ) { if( asp_gate_active( c->gate, 0 ) ) c->t_act[0]++; if( asp_gate_active( c->gate, 1 ) ) c->t_act[1]++; }
    }
  else c->gate_ready = 0;

  /* the tick before went whole to the take that just ended; do not write any of it again */
  if( scan < 0 && c->gone && c->gone + 1 == c->tick ) scan = 0;
  int64_t off = ( recpos + c->size + scan ) % c->size;

  if( recpos < ASP_TICK )                                 /* the top of a pass */
    {
    if( c->newt ) { ++c->iter; asp_tank_set_head( c->tank, c->iter, c->peak ); }
    asp_tank_set_seq( c->tank, (uint64_t)asp_log_last( c->store ) );
    logf_( c, "top", "{\"iter\":%llu,\"peak\":%llu,\"take\":%d}", (unsigned long long)c->iter, (unsigned long long)c->peak, track );
    c->newt = track;
    }

  if( c->live && c->tick > c->hush && scan < ASP_TICK )
    {
    if( track ) { level_scan( c, c->cur, ASP_TICK ); take_put( c, c->cur, ASP_TICK ); }
    else if( ! take_new( c, off, scan, ( recpos + scan ) < 0 ) ) { c->newt = 1; if( asp_gate_active( c->gate, 0 ) ) c->t_act[0]++; if( asp_gate_active( c->gate, 1 ) ) c->t_act[1]++; }
    c->shh = 0;
    }
  else if( track && ++c->shh < HANG ) { level_scan( c, c->cur, ASP_TICK ); take_put( c, c->cur, ASP_TICK ); }
  else if( track ) take_end( c, c->cur );

  /* the loop goes out ahead of the recording by the round trip, so what is played against it lands back in place */
  {
  int64_t outpos = ( recpos + c->latency ) % c->size, mix[2 * ASP_TICK];
  c->outpos = outpos;
  if( outpos < ASP_TICK )
    {
    ++c->pass;
    logf_( c, "wrap", "{\"iter\":%llu,\"pass\":%lld}", (unsigned long long)c->iter, (long long)c->pass );
    for( int i = 0; i < c->nmv; i++ ) c->mv[i].waited++;
    }
  mv_run( c );
  if( asp_tank_get( c->tank, outpos, ASP_TICK, mix ) ) memset( out, 0, sizeof( int32_t ) * 2 * ASP_TICK );
  else c->peak = asp_mix_out( c->peak, out, ASP_TICK, mix, c->cfg.gain, &c->out_shift );
  }
  memcpy( c->prev, c->cur, sizeof c->prev );
  c->have_prev = 1;
  }

int asp_core_tick( AspCore *c, const int32_t *in, int32_t *out )
  {
  if( c->phase == 0 ) sync_tick( c, in, out );
  else play_tick( c, in, out );
  c->tick++;
  return 0;
  }

/* --- control --- */

void asp_core_safe( AspCore *c, int on )
  {
  if( on == ! c->live ) return;
  c->live = ! on;
  if( on && c->enc ) take_end( c, NULL );
  logf_( c, on ? "safe_on" : "safe_off", "{}" );
  }

void asp_core_resync( AspCore *c )
  {
  if( c->enc ) { c->t_damaged = 1; take_end( c, NULL ); }
  logf_( c, "resync", "{}" );
  sync_begin( c );
  }

void asp_core_edge( AspCore *c, int starve, int64_t frames )
  {
  logf_( c, starve ? "edge_starve" : "edge_drop", "{\"frames\":%lld}", (long long)frames );
  if( c->phase == 1 )                                    /* the alignment is gone: no take is trusted, hunt again */
    {
    if( c->enc ) { c->t_damaged = 1; take_end( c, NULL ); }
    sync_begin( c );
    }
  }

void asp_core_gate( AspCore *c, double gate_db, double ring_db )
  {
  asp_gate_set( c->gate, gate_db, ring_db );
  logf_( c, "gate_set", "{\"gate\":%.1f,\"ring\":%.1f}", asp_gate_margin( c->gate ), asp_gate_ring( c->gate ) );
  }
void asp_core_gate_options( AspCore *c, double lowcut, int strict )
  {
  if( lowcut >= 0 ) asp_gate_set_lowcut( c->gate, lowcut );
  if( strict >= 0 ) asp_gate_set_strict( c->gate, strict );
  logf_( c, "gate_set", "{\"lowcut\":%.0f,\"strict\":%d}", asp_gate_lowcut( c->gate ), asp_gate_strict( c->gate ) );
  }
void asp_core_gate_relearn( AspCore *c ) { c->gate_relearn = 1; }

void asp_core_status( const AspCore *c, AspCoreStatus *st )
  {
  int in = 0;
  for( int i = 0; i < c->nlt; i++ ) in += c->lt[i].in;
  st->phase = c->phase; st->tick = c->tick; st->origin = c->origin; st->latency = c->latency; st->hush = c->hush;
  st->iter = c->iter; st->live = c->live; st->take_open = c->enc != NULL;
  st->take_iter = c->t_iter; st->take_off = c->t_off; st->take_frames = c->t_frames;
  st->sync_level_db = c->sync ? asp_sync_level( c->sync ) : 0;
  st->loop_takes = in; st->jobs = c->nmv; st->automix = c->cfg.automix;
  st->lowcut = asp_gate_lowcut( c->gate ); st->strict = asp_gate_strict( c->gate ); st->gate_db = asp_gate_margin( c->gate ); st->ring_db = asp_gate_ring( c->gate ); st->gate_learnt = asp_gate_learnt( c->gate );
  st->log_seq = asp_log_last( c->store ); st->loop_frames = c->size; st->peak = (int64_t)c->peak;
  }

int64_t asp_core_revert( AspCore *c, int64_t seq )
  {
  if( c->live ) return -1;
  if( c->enc ) take_end( c, NULL );
  for( int i = 0; i < c->nmv; i++ ) asp_job_free( c->mv[i].job );
  c->nmv = 0;
  int64_t r = asp_revert( c->store, seq, "aspace" );
  if( r < 0 ) return -1;
  if( asp_rebuild( c->tank, c->store, -1 ) ) { logf_( c, "error", "{\"what\":\"rebuild after revert failed\"}" ); return -1; }
  c->nlt = 0;
  Load ld = { c };
  asp_fold( c->store, -1, load_cb, &ld );
  logf_( c, "reverted", "{\"to\":%lld,\"seq\":%lld,\"takes\":%d}", (long long)seq, (long long)r, c->nlt );
  return r;
  }

int asp_core_close( AspCore *c )
  {
  if( c->enc ) take_end( c, NULL );
  for( int i = 0; i < c->nmv; i++ ) asp_job_free( c->mv[i].job );   /* the log says where they were going: a rebuild finishes them */
  if( c->nmv ) logf_( c, "moves_dropped", "{\"n\":%d}", c->nmv );
  if( ! asp_tank_legacy( c->tank ) )
    {
    asp_tank_set_head( c->tank, c->iter, c->peak );
    asp_tank_set_seq( c->tank, (uint64_t)asp_log_last( c->store ) );
    /* the tank only matches the log if every move finished */
    asp_tank_set_dirty( c->tank, c->nmv != 0 );
    }
  logf_( c, "core_close", "{}" );
  asp_gate_free( c->gate );
  asp_sync_free( c->sync );
  free( c->lt ); free( c->mv );
  free( c );
  return 0;
  }
