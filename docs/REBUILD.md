# a5rebuild

The last resort, for a space whose files are damaged. It lives outside the core on purpose: aspace never rebuilds anything and never
marks the loop file dirty. A kill is handled by the moves list in the session (it resumes), not by a rebuild. Use `a5rebuild` only when
aspace refuses to start with "run a5rebuild", or when you know a file is bad.

```
a5rebuild DIR                    check session.db, then make the loop file new
a5rebuild DIR --sql              session.db only
a5rebuild DIR --tank [FILE]      the loop file only (default DIR/cache/loop.d64)
a5rebuild --help
```

The space must not be running (it refuses if it is). Put `DIR` before `--tank`. Exit 0 when done, 1 when something could not be mended,
2 for a bad call.

## --sql

1. `PRAGMA integrity_check`. If it fails the run stops, nothing is changed, and you are told to restore session.db from a copy.
2. Every table is compared with the schema (`tools/schema.sql`) by column names and types.
   - **Derived tables** `peaks`, `moves`, `peers`: missing or the wrong shape means dropped and made again. A `moves` table with the right
     shape is kept as it is: it is the list used to resume moves after a kill.
   - **History** `session`, `takes`, `log`, `mixes`, `notes`: never written. Missing or the wrong shape stops the run; nothing else holds that
     information, so restore a copy.
3. A schema version 2 session is brought to 3; any other version is refused ("a session of an older aspace").
4. Takes with no levels get them from their files (what `a5peaks` does, which shares the same code).

## --tank

Makes the loop file new from the store: every take that is in the loop is put in as the log says it stands. It is what you need for a loop file
that is lost, the wrong size, an old format (before the chip model, which has no file header and a header per chip), or corrupt (chip
iterations that neither all match nor fall by exactly one at one edge). `a5recover` does the same after it puts a take back.

## Not done

A check for a corrupt loop (it always rebuilds when asked); re-adopting take files into a new database when the history tables are damaged.

Tests: `tests/test_rebuild.sh`.
