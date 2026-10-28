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
#include "settings.h"

#define RATE        48000
#define LEVEL_CLIP  0x7FFFFF00LL
#define HANG        10            /* ticks of silence a take keeps after the last sound */
#define BUDGET      48000         /* frames of take a tick may spend on level moves */

typedef struct
  {
  int64_t id, iter, off, len, peak;
  AspPlace now, base;             /* where it is (decided); where it was put when recorded */
  int in, mono, src, overlaps, gen;
  char gtail[96];                 /* ,"generated":"click","style":"beats" as the store has it, kept through every state rewrite */
  int64_t ready_pass;             /* the first pass it may be faded in */
  } LoopTake;

typedef struct { AspJob *job; int64_t id, s0, S; int now; } Move;   /* s0: where on the loop the take starts; S: that, unrolled, the last time the head passed it */

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
  int      t_feed;                  /* the take in progress is being put into the loop as it comes */
  int      t_fix, t_in;             /* its input: the one channel it is recorded from (mono from the start), or -1; and the input setting then */
  char     profile[64], settings[1100];
  int      started;
  int      out_shift;               /* how far the last output tick was brought down to fit */
  int      mix_now;                 /* the change being made is to be heard at once, not at the take's next start */

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
  char mx[60] = "";
  if( t->now.mixed ) snprintf( mx, sizeof mx, ",\"mixed\":1,\"gdb\":%d,\"pan\":%d", t->now.gdb, t->now.pan );
  snprintf( buf, n, "{\"status\":\"%s\",\"lev\":[\"%s\",\"%s\"],\"flip\":%d,\"mono\":%d,\"src\":%d,\"len\":%lld,\"gone\":0,\"peak\":%lld%s%s}",
            status, l, r, t->now.flip, t->mono, t->src, (long long)t->len, (long long)t->peak, mx, t->gtail );
  }

static LoopTake *lt_add( AspCore *c )
  {
  if( c->nlt == c->caplt ) { c->caplt = c->caplt ? 2 * c->caplt : 32; c->lt = realloc( c->lt, c->caplt * sizeof *c->lt ); }
  memset( c->lt + c->nlt, 0, sizeof *c->lt );
  return c->lt + c->nlt++;
  }

typedef struct { AspCore *c; } Load;
static void apply_text( AspCore *c, const char *text )
  {
  char v[100];
  if( ! asp_settings_get( text, "gate", v, sizeof v ) ) asp_gate_set( c->gate, atof( v ), 0 );
  if( ! asp_settings_get( text, "ring", v, sizeof v ) ) asp_gate_set( c->gate, 0, atof( v ) );
  if( ! asp_settings_get( text, "lowcut", v, sizeof v ) ) asp_gate_set_lowcut( c->gate, atof( v ) );
  if( ! asp_settings_get( text, "strict", v, sizeof v ) ) asp_gate_set_strict( c->gate, atoi( v ) );
  if( ! asp_settings_get( text, "mono", v, sizeof v ) ) c->cfg.mono_detect = atoi( v );
  if( ! asp_settings_get( text, "input", v, sizeof v ) )
    {
    c->cfg.input = ! strcmp( v, "left" ) ? 1 : ! strcmp( v, "right" ) ? 2 : ! strcmp( v, "stereo" ) ? 3 : 0;
    asp_gate_set_channels( c->gate, c->cfg.input == 1 ? 1 : c->cfg.input == 2 ? 2 : 3 );
    }
  if( ! asp_settings_get( text, "minpeak", v, sizeof v ) ) c->cfg.minpeak = atof( v );
  if( ! asp_settings_get( text, "automix", v, sizeof v ) )
    {
    AspCoreCfg t = c->cfg; asp_settings_cfg( &t, text );
    c->cfg.automix = t.automix;
    }
  }

static void load_cb( int64_t id, const char *json, void *u )
  {
  AspCore *c = ( (Load *)u )->c;
  AspTakeState st; AspTake k;
  if( id == 0 )                                            /* the settings live here (they are not a take) */
    {
    const char *s = strstr( json, "\"settings\":\"" ), *p = strstr( json, "\"profile\":\"" );
    if( p ) { p += 11; size_t l = strcspn( p, "\"" ); if( l > 63 ) l = 63; memcpy( c->profile, p, l ); c->profile[l] = 0; }
    if( s ) { s += 12; size_t l = strcspn( s, "\"" ); if( l > 1099 ) l = 1099; memcpy( c->settings, s, l ); c->settings[l] = 0; if( c->started ) apply_text( c, c->settings ); }
    return;
    }
  asp_parse_state( json, &st );
  if( asp_take_get( c->store, id, &k ) ) return;
  LoopTake *t = lt_add( c );
  t->id = id; t->iter = k.iter; t->off = k.off; t->len = st.len >= 0 ? st.len : k.frames;
  t->now = t->base = (AspPlace){ st.l, st.r, st.flip, st.gdb, st.pan, st.mixed };
  t->in = strcmp( st.status, "off" ) != 0; t->mono = st.mono; t->peak = st.peak; t->gen = strstr( json, "\"generated\"" ) != NULL;
  if( t->gen )                                             /* the marker has to survive the state being rewritten */
    {
    const char *g = strstr( json, "\"generated\":\"" ), *y = strstr( json, "\"style\":\"" );
    int gl = g ? (int)strcspn( g + 13, "\"" ) : 0, yl = y ? (int)strcspn( y + 9, "\"" ) : 0;
    if( gl > 12 ) gl = 12; if( yl > 24 ) yl = 24;
    snprintf( t->gtail, sizeof t->gtail, ",\"generated\":\"%.*s\"%s%.*s%s", gl, g ? g + 13 : "", yl ? ",\"style\":\"" : "", yl, y ? y + 9 : "", yl ? "\"" : "" );
    }
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
  asp_gate_set_channels( c->gate, cfg->input == 1 ? 1 : cfg->input == 2 ? 2 : 3 );
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
  c->started = 1;
  logf_( c, "core_open", "{\"loop\":%lld,\"iter\":%llu,\"live\":%d,\"takes\":%d}", (long long)c->size, (unsigned long long)c->iter, c->live, c->nlt );
  return c;
  }

/* --- level moves --- */

/* the first position of the loop the head has not yet read, counted on from the start (the head laps the loop) */
static int64_t unread( const AspCore *c ) { return (int64_t)c->pass * c->size + c->outpos + ASP_TICK; }

/* a take is only changed behind the head. the change starts once the head has played the start of the take, and
   follows it along, a position being touched only after the head has gone past it; so whatever the head is playing
   is whole, and the next time it comes round it plays the new. S is the last time the head passed the take's start */
static int64_t last_start( const AspCore *c, int64_t s0 )
  {
  int64_t u = unread( c ), back = ( ( u - s0 ) % c->size + c->size ) % c->size;
  return u - back;
  }

static void mv_add( AspCore *c, AspJob *j, int64_t id )
  {
  if( ! j ) { logf_( c, "error", "{\"what\":\"level move not made\"}" ); return; }
  if( c->nmv == c->capmv ) { c->capmv = c->capmv ? 2 * c->capmv : 16; c->mv = realloc( c->mv, c->capmv * sizeof *c->mv ); }
  Move *m = c->mv + c->nmv++;
  m->job = j; m->id = id; m->now = c->mix_now;
  m->s0 = asp_job_pos0( j ) % c->size; m->S = last_start( c, m->s0 );
  }

static void mv_run( AspCore *c )
  {
  int64_t budget = BUDGET, head = (int64_t)c->pass * c->size + c->outpos;       /* this tick's position is read after this */
  for( int i = 0; i < c->nmv; )
    {
    Move *m = c->mv + i; int r = 0;
    int64_t lim = m->now ? -1 : head - m->S;
    if( budget > 0 && ( lim != 0 ) && ( lim > 0 || m->now ) )
      {
      int64_t before = asp_job_cursor( m->job );
      r = asp_job_step_to( m->job, c->tank, budget, lim );
      budget -= asp_job_cursor( m->job ) - before;
      }
    if( r != 0 )
      {
      if( r < 0 ) logf_( c, "error", "{\"what\":\"level move failed\"}" );
      asp_job_free( m->job );
      memmove( c->mv + i, c->mv + i + 1, ( --c->nmv - i ) * sizeof *c->mv );
      continue;
      }
    i++;
    }
  }

int asp_core_pending( const AspCore *c, int64_t id )
  {
  for( int i = 0; i < c->nmv; i++ ) if( c->mv[i].id == id ) return c->mv[i].now ? 2 : 1;
  return 0;
  }

static void queue_move( AspCore *c, LoopTake *t, AspPlace from, AspPlace to, int in_from, int in_to )
  {
  AspPlace f = in_from ? from : (AspPlace)ASP_PLACE_OFF, g = in_to ? to : (AspPlace)ASP_PLACE_OFF;
  for( int i = c->nmv - 1; i >= 0; i-- )                /* not begun yet: it goes to the new place instead */
    {
    Move *m = c->mv + i;
    if( m->id != t->id ) continue;
    if( asp_job_cursor( m->job ) == 0 )
      {
      int r = asp_job_retarget( m->job, g );
      if( r >= 0 )
        {
        if( c->mix_now ) m->now = 1;
        if( r == 1 ) { asp_job_free( m->job ); memmove( c->mv + i, c->mv + i + 1, ( --c->nmv - i ) * sizeof *c->mv ); }
        else m->S = last_start( c, m->s0 );
        return;
        }
      }
    break;
    }
  if( asp_place_eq( f, g ) ) return;
  mv_add( c, asp_job_take( c->store, t->id, 0, -1, f, g ), t->id );
  }

/* --- automix --- */

static void change( AspCore *c, LoopTake *t, AspPlace to, int in, const char *why )
  {
  AspPlace from = t->now; int was = t->in;
  char st[300];
  t->now = to; t->in = in;
  state_json( t, in ? "mix" : "off", st, sizeof st );
  asp_take_set_state( c->store, t->id, st, "aspace", c->tick );
  logf_( c, "relevel", "{\"take\":%lld,\"iter\":%lld,\"off\":%lld,\"why\":\"%s\",\"lev\":[%d,%d],\"flip\":%d,\"in\":%d,\"mixed\":%d,\"gdb\":%d,\"pan\":%d}", (long long)t->id, (long long)t->iter, (long long)t->off, why,
         to.l == ASP_LEV_OFF ? -1 : to.l, to.r == ASP_LEV_OFF ? -1 : to.r, to.flip, in, to.mixed, to.gdb, to.pan );
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
    if( t == nt || ! t->in || t->now.mixed || ! overlaps( c, t, nt ) ) continue;   /* a take with a mix of its own is left alone */
    if( ++t->overlaps >= n ) change( c, t, t->now, 0, "overlap" );
    }
  }

void asp_core_settings( AspCore *c, const char *profile, const char *text )
  {
  char st[1300];
  snprintf( c->profile, sizeof c->profile, "%s", profile ? profile : "" );
  snprintf( c->settings, sizeof c->settings, "%s", text ? text : "" );
  apply_text( c, c->settings );
  snprintf( st, sizeof st, "{\"profile\":\"%s\",\"settings\":\"%s\"}", c->profile, c->settings );
  asp_take_set_state( c->store, 0, st, "aspace", c->tick );
  logf_( c, "settings", "{\"profile\":\"%s\",\"settings\":\"%s\"}", c->profile, c->settings );
  }
const char *asp_core_profile( const AspCore *c ) { return c->profile; }
const char *asp_core_settings_text( const AspCore *c ) { return c->settings; }

/* a click made by a5click for a space that is running: the flac (the loop long, mono) was written under blobs/tmp. it is
   kept as a take of its own; in puts it into the loop as well (behind the head, like any other change) */
int64_t asp_core_click_add( AspCore *c, int64_t *iter, const char *part, const char *style, int ll, int lr, int in )
  {
  char path[1300], st[500]; AspTake k; uint64_t fr = 0; int ch = 0, rate = 0, bits = 0;
  snprintf( path, sizeof path, "%s/blobs/tmp/%s", asp_dir( c->store ), part );
  if( strchr( part, '/' ) || asp_flac_info( path, NULL, &fr, &ch, &rate, &bits ) || (int64_t)fr != c->size || ch != 1 ) return -1;
  if( *iter >= 0 )                                          /* the next free generated iteration */
    for( *iter = -1; ; (*iter)-- )
      {
      int used = 0;
      for( int i = 0; i < c->nlt; i++ ) if( c->lt[i].iter == *iter && c->lt[i].off == 0 ) used = 1;
      if( ! used ) break;
      }
  LoopTake lt; memset( &lt, 0, sizeof lt );
  lt.iter = *iter; lt.len = c->size; lt.mono = 1; lt.peak = -1; lt.gen = 1; lt.in = in;
  lt.now = lt.base = (AspPlace){ ll, lr, 0, 0, 0, 0 };
  snprintf( lt.gtail, sizeof lt.gtail, ",\"generated\":\"click\",\"style\":\"%.20s\"", style );
  state_json( &lt, in ? "mix" : "off", st, sizeof st );
  if( asp_take_adopt( c->store, *iter, 0, path, in ? "mix" : "off", st, c->tick, &k ) ) return -1;
  if( asp_take_peak( c->store, k.id, &lt.peak ) ) lt.peak = 0;
  lt.id = k.id;
  LoopTake *t = lt_add( c ); *t = lt;
  logf_( c, "click_added", "{\"take\":%lld,\"style\":\"%s\",\"in\":%d}", (long long)k.id, style, in );
  if( in && c->cfg.feed ) queue_move( c, t, (AspPlace)ASP_PLACE_OFF, t->now, 0, 1 );
  return k.id;
  }

/* every click a5click made goes out of the loop (they stay in the store as takes) */
int asp_core_click_clear( AspCore *c )
  {
  int n = 0;
  for( int i = 0; i < c->nlt; i++ )
    {
    LoopTake *t = c->lt + i;
    if( ! t->in || ! strstr( t->gtail, "\"click\"" ) ) continue;
    change( c, t, t->now, 0, "clear" ); n++;
    }
  logf_( c, "click_cleared", "{\"takes\":%d}", n );
  return n;
  }

int asp_core_wipe( AspCore *c, int all )
  {
  int n = 0;
  for( int i = 0; i < c->nlt; i++ )
    {
    LoopTake *t = c->lt + i;
    if( ! t->in || ( t->gen && ! all ) ) continue;
    change( c, t, t->now, 0, "wipe" );
    n++;
    }
  logf_( c, "wipe", "{\"takes\":%d,\"all\":%d}", n, all );
  return n;
  }

/* a person's change to the mix of takes: gain and pan are absolute; a mute takes the take out of the loop (its mix is kept
   and can still be changed); play puts it back at its mix. a take with an explicit gain or pan is then left alone by the automix */
int asp_core_mix( AspCore *c, int64_t iter, int64_t off, const AspMixCmd *m )
  {
  int hit = 0;
  c->mix_now = m->now;
  for( int i = 0; i < c->nlt; i++ )
    {
    LoopTake *t = c->lt + i;
    if( t->iter != iter || ( off >= 0 && t->off != off ) ) continue;
    hit++;
    AspPlace to = t->now; int in = t->in;
    if( m->set_gain || m->set_pan )
      {
      int g, p;
      asp_place_to_mix( t->now, t->mono, &g, &p );
      if( m->set_gain == 1 ) g = m->gdb;                    /* 2: keep what it has, now as a mix of its own */
      if( m->set_pan == 1 ) p = m->pan;
      g = g < ASP_GDB_MIN ? ASP_GDB_MIN : g > ASP_GDB_MAX ? ASP_GDB_MAX : g; p = p < -100 ? -100 : p > 100 ? 100 : p;
      to = (AspPlace){ 0, 0, 0, g, p, 1 };
      }
    if( m->act == ASP_MIX_MUTE ) in = 0; else if( m->act == ASP_MIX_PLAY ) in = 1; else if( m->act == ASP_MIX_TOGGLE ) in = ! t->in;
    if( asp_place_eq( to, t->now ) && in == t->in && ! m->now ) continue;
    change( c, t, to, in, "mix" );
    }
  c->mix_now = 0;
  return hit;
  }

int asp_core_takes( const AspCore *c, void (*cb)( void *, const AspCoreTake * ), void *u )
  {
  for( int i = 0; i < c->nlt; i++ )
    {
    const LoopTake *t = c->lt + i; AspCoreTake o;
    o.id = t->id; o.iter = t->iter; o.off = t->off; o.len = t->len; o.in = t->in; o.mono = t->mono; o.mixed = t->now.mixed;
    asp_place_to_mix( t->now, t->mono, &o.gdb, &o.pan );
    o.pending = asp_core_pending( c, t->id );
    cb( u, &o );
    }
  return c->nlt;
  }

void asp_core_automix( AspCore *c, int mode, int n, int each )
  {
  c->cfg.automix.mode = mode; c->cfg.automix.n = n; c->cfg.automix.each = each;
  logf_( c, "automix", "{\"mode\":%d,\"n\":%d,\"each\":%d}", mode, n, each );
  }

/* --- takes --- */

static void take_feed( AspCore *c, const int32_t *pcm, int64_t n, int lev )
  {
  if( c->t_feed && n > 0 )
    asp_tank_level( c->tank, c->t_sec * RATE + c->t_frames, pcm, n, ASP_LEV_OFF, ASP_LEV_OFF, lev, lev );
  }

/* frames of a take, from its start, counted so that frame 0 is the start of its second of the loop */
static int take_put( AspCore *c, const int32_t *pcm, int64_t n )
  {
  int rc = 0;
  if( c->t_fix >= 0 )                              /* one channel, recorded as mono and put in the middle as it comes: no stereo, no conversion */
    {
    int32_t m[4096], d[2 * 4096];
    for( int64_t i = 0; i < n; i += 4096 )
      {
      int64_t k = n - i < 4096 ? n - i : 4096;
      for( int64_t j = 0; j < k; j++ ) d[2*j] = d[2*j+1] = m[j] = pcm[2 * ( i + j ) + c->t_fix];
      rc |= asp_flac_enc_write( c->enc, m, (unsigned)k );
      if( c->t_feed ) asp_tank_level( c->tank, c->t_sec * RATE + c->t_frames + i, d, k, ASP_LEV_OFF, ASP_LEV_OFF, c->cfg.mono_lev, c->cfg.mono_lev );
      }
    c->t_frames += n;
    return rc;
    }
  for( int64_t i = 0; i < n; i += 4096 )
    {
    int64_t k = n - i < 4096 ? n - i : 4096;
    rc |= asp_flac_enc_write( c->enc, pcm + 2 * i, (unsigned)k );
    }
  for( int64_t i = 0; i < n && c->t_ident; i++ ) if( pcm[2*i] != pcm[2*i+1] ) c->t_ident = 0;
  take_feed( c, pcm, n, 0 );
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

  /* not worth keeping: a bump (told the instrument's lowest note, most of the take lies below it), or a take that never
     reaches the quietest level worth keeping. it is not stored, and it is taken out of the loop again */
  double share = asp_gate_event_share( c->gate, 1 );
  double pkdb = level_db( c->t_peak[0] > c->t_peak[1] ? c->t_peak[0] : c->t_peak[1] );
  int is_bump = c->cfg.lowcut > 30 && asp_gate_lowcut( c->gate ) > 30 && share < asp_gate_dominance( c->gate );
  int is_quiet = c->cfg.minpeak < 0 && pkdb < c->cfg.minpeak;
  if( ! bad && ( is_bump || is_quiet ) )
    {
    int32_t *sp = NULL; int64_t n2 = 0; int ch2 = 0;
    if( c->t_feed && asp_pcm_load( c->part, &sp, &n2, &ch2 ) == 0 && ch2 == ( c->t_fix >= 0 ? 1 : 2 ) )   /* it was fed into the loop as it came: take it out again */
      mv_add( c, asp_job_new( c->t_sec * RATE, 0, -1, sp, n2, ch2, c->t_fix >= 0 ? (AspPlace){ c->cfg.mono_lev, c->cfg.mono_lev, 0 } : (AspPlace){ 0, 0, 0 }, malloc( 4 ), 0, ch2, ASP_PLACE_OFF ), -1 );
    else free( sp );
    unlink( c->part );
    logf_( c, "discarded", "{\"iter\":%lld,\"off\":%lld,\"len\":%lld,\"share\":%.3f,\"why\":\"%s\",\"peak_db\":[%.1f,%.1f]}",
           (long long)c->t_iter, (long long)c->t_off, (long long)c->t_frames, share, is_bump ? "bump" : "quiet", level_db( c->t_peak[0] ), level_db( c->t_peak[1] ) );
    c->gone = c->tick;
    return;
    }

  /* mono: one channel never rang, or both are the same */
  int mono = 0, src = 0;
  if( c->t_fix >= 0 ) { mono = 1; src = c->t_fix; }          /* told which channel: mono from the start */
  else if( c->t_in == 3 ) mono = 0;                          /* told it is stereo */
  else if( c->cfg.mono_detect && ! c->t_damaged )
    {
    int64_t a = c->t_act[0], b = c->t_act[1];
    if( c->t_ident ) { mono = 1; src = 0; }
    else if( a > 0 && b * 20 <= a ) { mono = 1; src = 0; }
    else if( b > 0 && a * 20 <= b ) { mono = 1; src = 1; }
    }
  int swap = c->cfg.automix.mode == ASP_AUTOMIX_SWAP;
  if( swap && c->swap_n++ && ( c->cfg.automix.each || (int64_t)c->t_iter != c->swap_iter ) ) c->swap_flip = ! c->swap_flip;   /* the first take goes left / unflipped */
  c->swap_iter = c->t_iter;

  AspPlace live = c->t_fix >= 0 ? (AspPlace){ c->cfg.mono_lev, c->cfg.mono_lev, 0 } : (AspPlace){ 0, 0, 0 }, fin = { 0, 0, 0 };
  if( mono ) { if( swap ) fin = c->swap_flip ? (AspPlace){ ASP_LEV_OFF, 0, 0 } : (AspPlace){ 0, ASP_LEV_OFF, 0 };
               else fin = (AspPlace){ c->cfg.mono_lev, c->cfg.mono_lev, 0 }; }
  else if( swap ) fin.flip = c->swap_flip;

  LoopTake lt = { 0 };
  lt.iter = c->t_iter; lt.off = c->t_off; lt.len = c->t_frames; lt.mono = mono; lt.src = src;
  lt.peak = c->t_peak[0] > c->t_peak[1] ? c->t_peak[0] : c->t_peak[1];
  lt.now = lt.base = fin; lt.in = c->t_feed;
  char st[300];
  state_json( &lt, lt.in ? "mix" : "off", st, sizeof st );

  int32_t *stereo = NULL; int64_t sn = 0; int sch = 0;
  char part2[1200]; part2[0] = 0;
  if( ! bad && mono && c->t_fix < 0 )                       /* keep the stereo for the move, store one channel */
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
    if( c->t_feed )
      {
      if( mono && stereo )                                  /* the live feed was stereo: swap it for the mono at its place */
        {
        int32_t *m = malloc( (size_t)sn * 4 );
        for( int64_t i = 0; i < sn; i++ ) m[i] = stereo[2*i+src];
        mv_add( c, asp_job_new( c->t_sec * RATE, 0, -1, stereo, sn, 2, live, m, sn, 1, fin ), -1 );
        stereo = NULL;
        }
      else if( fin.l != live.l || fin.r != live.r || fin.flip != live.flip )
        mv_add( c, asp_job_take( c->store, tk.id, 0, -1, live, fin ), tk.id );
      }
    free( stereo );
    logf_( c, "end", "{\"take\":%lld,\"iter\":%lld,\"off\":%lld,\"len\":%lld,\"peak_db\":[%.1f,%.1f],\"clips\":[%lld,%lld],\"damaged\":%d,\"mono\":%d,\"src\":%d,\"flip\":%d,\"in\":%d}",
           (long long)tk.id, (long long)c->t_iter, (long long)c->t_off, (long long)c->t_frames,
           level_db( c->t_peak[0] ), level_db( c->t_peak[1] ), (long long)c->t_clips[0], (long long)c->t_clips[1],
           c->t_damaged, mono, src, fin.flip, c->t_feed );
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
  c->t_in = c->cfg.input;
  c->t_fix = c->t_in == 1 ? 0 : c->t_in == 2 ? 1 : -1;
  c->enc = asp_flac_enc_open( c->part, c->t_fix >= 0 ? 1 : 2, RATE );
  if( ! c->enc ) return -1;
  c->t_iter = (int64_t)c->iter; c->t_sec = sec; c->t_off = sec; c->t_frames = 0;
  c->t_wrapped = wrapped; c->t_damaged = 0; c->shh = 0; c->t_ident = 1;
  c->t_feed = c->cfg.feed && c->cfg.automix.mode != ASP_AUTOMIX_KEEP;     /* keep: stored, not put into the loop */
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
  asp_gate_event_share( c->gate, 1 );
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
  st->input = c->cfg.input; st->lowcut = asp_gate_lowcut( c->gate ); st->strict = asp_gate_strict( c->gate ); st->gate_db = asp_gate_margin( c->gate ); st->ring_db = asp_gate_ring( c->gate ); st->gate_learnt = asp_gate_learnt( c->gate );
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
