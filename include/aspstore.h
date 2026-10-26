/* aspstore: one session = one directory.
     session.db           sqlite (WAL): session, takes, log, mixes, notes, peers
     blobs/ab/<sha>.flac  immutable takes, named by the sha-256 of the file
     cache/               disposable (loop tank, checkpoints)
   a single writer (aspace) is assumed; readers may open read-only. see docs/DATA.md */
#ifndef ASPSTORE_H
#define ASPSTORE_H
#include <stdint.h>
#include <stddef.h>

typedef struct AspStore AspStore;

typedef struct
  {
  int64_t id;
  int64_t iter, off;
  char    sha[65];
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

/* the log: every action is an event. returns seq, or -1. */
int64_t asp_log( AspStore *s, const char *actor, const char *type, const char *args_json, int64_t tick );
int64_t asp_log_last( AspStore *s );

/* takes. add encodes pcm to flac (.part, fsync, rename) then commits the row and its
   take_added / take_state events in one transaction. a half-written take never has a row. */
int  asp_take_add_d32( AspStore *s, int64_t iter, int64_t off, const char *d32_path,
                       int channels, int rate, const char *status, int64_t tick, AspTake *out );
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

/* sha-256 (lowercase hex into out[65]) of a file; used for blob names */
int  asp_sha256_file( const char *path, char out[65] );
void asp_sha256_buf( const void *p, size_t len, char out[65] );
#endif
