/* keeper: a servant thread for what waits on the disk, so the loop never does. it decides nothing and shares nothing with
   the core but a short queue: files to be made durable (fdatasync, and fsync of the directories they sit in), tagged so the
   core can be told, without waiting, when each is safe; and the database's write ahead log folded back into the database now
   and then, on a connection of its own. the core never waits for it, except stopping at the end, which lets it finish. */
#ifndef KEEPER_H
#define KEEPER_H
#include <stdint.h>

typedef struct AspKeeper AspKeeper;
/* db: the session database to checkpoint every period_ms (NULL: none). NULL if the thread cannot start */
AspKeeper *asp_keeper_start( const char *db, int period_ms );
void asp_keeper_settle( AspKeeper *k, int64_t tag, const char *path );        /* queued; returns at once */
int  asp_keeper_done( AspKeeper *k, int64_t *tags, int max );                 /* tags made durable since last asked; never blocks */
int  asp_keeper_checkpoints( AspKeeper *k );
void asp_keeper_stop( AspKeeper *k );                                         /* finish the queue, join */
void asp_settle_now( const char *path );                                      /* the same work, here, waiting */
#endif
