/* aspstore: one session = one directory.
     session.db           sqlite (WAL): session, takes, log, mixes, notes, peers
     takes.<artist>/<iter>/<off>.a5t   immutable takes (takefile.h); the folder carries the first 12 hex digits of the artist
     tmp/                 takes in progress and generated tracks waiting to be adopted
     cache/               disposable (loop tank, checkpoints)
   a single writer (aspace) is assumed; readers may open read-only. see docs/DATA.md */
#ifndef ASPSTORE_H
#define ASPSTORE_H
#include <stdint.h>
#include <stddef.h>
#include "takefile.h"

typedef struct AspStore AspStore;

typedef struct
  {
  int64_t id;
  int64_t iter, off;
  char    artist[33];           /* who recorded it: 32 hex digits */
  int64_t frames;
  int     rate, channels, bits;
  char    status[8];            /* trk | mix | off | legacy */
  } AspTake;

#define ASP_ERR 256
/* new session; descriptor_json is hashed (as given) into the session id. NULL on failure, err set */
AspStore *asp_create( const char *dir, const char *descriptor_json, char err[ASP_ERR] );
AspStore *asp_open( const char *dir, int readonly, char err[ASP_ERR] );
void      asp_close( AspStore *s );
const char *asp_session_id( AspStore *s );
const char *asp_dir( AspStore *s );
const char *asp_artist( AspStore *s );                     /* this store's artist, 32 hex digits ("000..." if it has none yet) */
const unsigned char *asp_artist_id( AspStore *s );          /* the same, 16 bytes */
int  asp_artist_local( unsigned char id[ASP_ARTIST], int create );   /* this user's artist, from the config; made if create */

/* the log: every action is an event. returns seq, or -1. */
int64_t asp_log( AspStore *s, const char *actor, const char *type, const char *args_json, int64_t tick );
int64_t asp_log_last( AspStore *s );
typedef void (*AspLogCb)( int64_t seq, int64_t tick, const char *actor, const char *type, const char *args_json, void *user );
int64_t asp_log_since( AspStore *s, int64_t after, AspLogCb cb, void *user );   /* calls cb for each newer event, returns the last seq */
typedef void (*AspLogCbW)( int64_t seq, int64_t tick, double wall, const char *actor, const char *type, const char *args_json, void *user );
int64_t asp_log_since_w( AspStore *s, int64_t after, AspLogCbW cb, void *user );   /* the same, with the wall clock time of each event */
const char *asp_descriptor( AspStore *s );                                       /* the session's descriptor json (static buffer) */

/* takes. add writes pcm as a take (takefile.h), then adopts it: rename into place and commit the row and its
   take_added / take_state events in one transaction. a half-written take never has a row. */
int  asp_take_add_d32( AspStore *s, int64_t iter, int64_t off, const char *d32_path,
                       int channels, int rate, const char *status, int64_t tick, AspTake *out );
/* a store of a running aspace starts a keeper (keeper.h): putting each adopted take in its place and making it safe on the disk, and
   the checkpoints of the log, happen in its thread, and the loop never waits for the disk. an adopted take is known at once, by the id
   it will have, and read from its part; its row (and take_added event) is made when the keeper has it in place and synced, so no
   other program ever finds a row without its file, and a power cut loses only takes that were not yet safe. asp_store_poll, called
   now and then from the loop, makes those rows. without a keeper (the offline tools) adopt does it all itself, before it returns. */
int  asp_store_keeper( AspStore *s );
/* a writer marks the log when it opens and closes the store. 1 if the last writer opened and never closed (it died; its pid in *pid),
   0 otherwise (also if that writer is still alive). for a store opened read write only */
int  asp_store_crashed( AspStore *s, int64_t *pid );
/* put back a take a dead run left (see a5recover): the file at file (its place or its part), under its old id and slot; 0 if done */
int  asp_take_restore( AspStore *s, int64_t id, int64_t iter, int64_t off, const char *file, const char *status );
int  asp_store_poll( AspStore *s );
/* a finished take already written at part (see asp_tmp_path, asp_tw_open): rename it to its place, row and events in one
   transaction. state_json NULL gives {"status":status}. on failure the part file is left alone. */
int  asp_take_adopt( AspStore *s, int64_t iter, int64_t off, const char *part, const char *status,
                     const char *state_json, int64_t tick, AspTake *out );
int  asp_tmp_path( AspStore *s, char *buf, size_t n );              /* a fresh name for a take in progress */
int  asp_take_get( AspStore *s, int64_t id, AspTake *out );
int  asp_take_find( AspStore *s, int64_t iter, int64_t off, AspTake *out );
int  asp_take_path( AspStore *s, const AspTake *t, char *buf, size_t n );
int  asp_take_export_d32( AspStore *s, int64_t id, const char *out_path );
/* record a new state (any json object) for a take; appends a take_state event */
int64_t asp_take_set_state( AspStore *s, int64_t id, const char *state_json, const char *actor, int64_t tick );

/* named mixes label a log point */
int  asp_mix_name( AspStore *s, const char *name, int64_t seq );
int64_t asp_mix_seq( AspStore *s, const char *name );          /* -1 if none */

/* state as of log point `upto` (< 0 means now): take_state events replayed in order,
   a revert{to:N} event restoring the state as of N. cb is called once per take. */
typedef void (*AspStateCb)( int64_t take_id, const char *state_json, void *user );
int  asp_fold( AspStore *s, int64_t upto, AspStateCb cb, void *user );
/* return to an earlier state: appends revert{to}; nothing is truncated */
int64_t asp_revert( AspStore *s, int64_t to_seq, const char *actor );

/* sha-256 (lowercase hex into out[65]); the session id is one */
void asp_sha256_buf( const void *p, size_t len, char out[65] );
#endif
