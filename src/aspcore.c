#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "aspcore.h"
#include "gate.h"
#include "flacio.h"

#define RATE        48000
#define LEVEL_CLIP  0x7FFFFF00LL
#define HANG        10            /* ticks of silence a take keeps after the last sound */

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
  int      t_wrapped, t_damaged, shh;
  int64_t  t_peak[2], t_clips[2];
  };

static void logf_( AspCore *c, const char *type, const char *form, ... )
  {
  char buf[400];
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

void asp_core_defaults( AspCoreCfg *c )
  {
  memset( c, 0, sizeof *c );
  asp_sync_defaults( &c->sync );
  c->live = 1; c->feed = 1;
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
  c->sync = asp_sync_new( &cfg->sync );
  c->phase = 0;
  if( ! asp_tank_legacy( t ) )
    {
    if( asp_tank_dirty( t ) ) logf_( c, "tank_dirty", "{}" );
    asp_tank_set_dirty( t, 1 );
    }
  logf_( c, "core_open", "{\"loop\":%lld,\"iter\":%llu,\"live\":%d}", (long long)c->size, (unsigned long long)c->iter, c->live );
  return c;
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

static void take_end( AspCore *c, const int32_t *last )
  {
  char st[300];
  AspTake t;
  if( last ) { level_scan( c, last, ASP_TICK ); take_put( c, last, ASP_TICK ); }
  int bad = asp_flac_enc_close( c->enc );
  c->enc = NULL;
  snprintf( st, sizeof st,
    "{\"status\":\"%s\",\"lev\":[\"0\",\"0\"],\"wrapped\":%d,\"len\":%lld,\"gone\":0,\"damaged\":%d}",
    c->cfg.feed ? "mix" : "off", c->t_wrapped, (long long)c->t_frames, c->t_damaged );
  if( bad || asp_take_adopt( c->store, c->t_iter, c->t_off, c->part, c->cfg.feed ? "mix" : "off", st, c->tick, &t ) )
    {
    logf_( c, "error", "{\"what\":\"take not stored\",\"iter\":%lld,\"off\":%lld,\"part\":\"%s\"}",
           (long long)c->t_iter, (long long)c->t_off, c->part );
    }
  else
    logf_( c, "end", "{\"take\":%lld,\"iter\":%lld,\"off\":%lld,\"len\":%lld,\"peak_db\":[%.1f,%.1f],\"clips\":[%lld,%lld],\"damaged\":%d}",
           (long long)t.id, (long long)c->t_iter, (long long)c->t_off, (long long)c->t_frames,
           level_db( c->t_peak[0] ), level_db( c->t_peak[1] ), (long long)c->t_clips[0], (long long)c->t_clips[1],
           c->t_damaged );
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
  c->t_wrapped = wrapped; c->t_damaged = 0; c->shh = 0;
  memset( c->t_peak, 0, sizeof c->t_peak ); memset( c->t_clips, 0, sizeof c->t_clips );
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
    else if( ! take_new( c, off, scan, ( recpos + scan ) < 0 ) ) c->newt = 1;
    c->shh = 0;
    }
  else if( track && ++c->shh < HANG ) { level_scan( c, c->cur, ASP_TICK ); take_put( c, c->cur, ASP_TICK ); }
  else if( track ) take_end( c, c->cur );

  /* the loop goes out ahead of the recording by the round trip, so what is played against it lands back in place */
  {
  int64_t outpos = ( recpos + c->latency ) % c->size, mix[2 * ASP_TICK];
  if( outpos < ASP_TICK ) logf_( c, "wrap", "{\"iter\":%llu}", (unsigned long long)c->iter );
  if( asp_tank_get( c->tank, outpos, ASP_TICK, mix ) ) memset( out, 0, sizeof( int32_t ) * 2 * ASP_TICK );
  else c->peak = asp_mix_out( c->peak, out, ASP_TICK, mix, c->cfg.gain );
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
void asp_core_gate_relearn( AspCore *c ) { c->gate_relearn = 1; }

void asp_core_status( const AspCore *c, AspCoreStatus *st )
  {
  st->phase = c->phase; st->tick = c->tick; st->origin = c->origin; st->latency = c->latency; st->hush = c->hush;
  st->iter = c->iter; st->live = c->live; st->take_open = c->enc != NULL;
  st->take_iter = c->t_iter; st->take_off = c->t_off; st->take_frames = c->t_frames;
  st->sync_level_db = c->sync ? asp_sync_level( c->sync ) : 0;
  }

int asp_core_close( AspCore *c )
  {
  if( c->enc ) take_end( c, NULL );
  if( ! asp_tank_legacy( c->tank ) )
    {
    asp_tank_set_head( c->tank, c->iter, c->peak );
    asp_tank_set_seq( c->tank, (uint64_t)asp_log_last( c->store ) );
    asp_tank_set_dirty( c->tank, 0 );
    }
  logf_( c, "core_close", "{}" );
  asp_gate_free( c->gate );
  asp_sync_free( c->sync );
  free( c );
  return 0;
  }
