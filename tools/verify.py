#!/usr/bin/env python3
"""usage: verify.py SRC_DIR DEST_DIR  -- bit-exact blobs, counts, rebuild and revert."""
import os, sys, re, json, sqlite3, hashlib, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from store import fold
src, dest = sys.argv[1:3]
db = sqlite3.connect(f"file:{dest}/session.db?mode=ro", uri=True)
bad = 0
rows = db.execute("select id,blob_sha,md5,frames,src_name from takes").fetchall()
for tid, sha, md5, frames, sn in rows:
    p = f"{dest}/blobs/{sha[:2]}/{sha}.flac"
    if hashlib.sha256(open(p, "rb").read()).hexdigest() != sha: print("sha mismatch", sn); bad += 1; continue
    h = hashlib.md5(); n = 0
    pr = subprocess.Popen([os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bin", "aspflac"), "dec", p, "-"], stdout=subprocess.PIPE)
    for b in iter(lambda: pr.stdout.read(1 << 22), b""): h.update(b); n += len(b)
    if pr.wait(): print("decode/md5 failed", sn); bad += 1
    if h.hexdigest() != md5 or n // 8 != frames: print("pcm mismatch", sn); bad += 1
srcn = len([f for f in os.listdir(src) if re.match(r"tl_\d+_\d+\.d32$", f)])
print(f"takes: source {srcn}, db {len(rows)}, bit-exact failures {bad}")
# rebuild and revert
last = db.execute("select max(seq) from log").fetchone()[0]
full = fold(db)
mid = db.execute("select seq from log where type='take_state' order by seq limit 1 offset 100").fetchone()[0]
at_mid = fold(db, mid)
print("states at end:", len(full), " at log point", mid, ":", len(at_mid))
# simulate a revert: fold with an appended revert event in a memory copy
mem = sqlite3.connect(":memory:"); db.backup(mem)
mem.execute("insert into log(tick,wall,actor,type,args_json) values(0,0,'t','revert',?)", (json.dumps({"to": mid}),))
print("revert to", mid, "reproduces earlier state:", fold(mem) == at_mid)
print("status counts:", dict(db.execute("select status,count(*) from takes group by 1")))
sys.exit(1 if bad else 0)
