-- asp5 session.db, schema v1 (see docs/DATA.md)
PRAGMA user_version=1;
PRAGMA journal_mode=WAL;
CREATE TABLE session(id TEXT PRIMARY KEY, descriptor_json TEXT NOT NULL,
  schema_version INTEGER NOT NULL, created REAL NOT NULL);
CREATE TABLE takes(
  id INTEGER PRIMARY KEY, iter INTEGER NOT NULL, off INTEGER NOT NULL,
  blob_sha TEXT NOT NULL, md5 TEXT NOT NULL, frames INTEGER NOT NULL,
  rate INTEGER NOT NULL, channels INTEGER NOT NULL, bits INTEGER NOT NULL,
  status TEXT NOT NULL,            -- trk | mix | off | legacy
  lev_l TEXT, lev_r TEXT,          -- level shift per channel, '-' = out
  wrapped INTEGER, len INTEGER, gone INTEGER,
  t_start REAL, src_name TEXT, UNIQUE(iter, off));
CREATE TABLE log(seq INTEGER PRIMARY KEY AUTOINCREMENT, tick INTEGER, wall REAL,
  actor TEXT NOT NULL, type TEXT NOT NULL, args_json TEXT NOT NULL);
CREATE TABLE mixes(name TEXT PRIMARY KEY, seq INTEGER NOT NULL);
CREATE TABLE notes(id INTEGER PRIMARY KEY, take_id INTEGER, seq INTEGER,
  kind TEXT, text TEXT);
CREATE TABLE peers(id TEXT PRIMARY KEY, addr TEXT, last_seen REAL);
