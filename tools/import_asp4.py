#!/usr/bin/env python3
"""Import an asp4 space (flat files) into a new asp5 session directory.
The source is only ever read. usage: import_asp4.py SRC_DIR NAME DEST_DIR"""
import os, sys, re, json, hashlib, sqlite3, subprocess, shutil, time, tempfile
from concurrent.futures import ThreadPoolExecutor

RATE, CH = 48000, 2
here = os.path.dirname(os.path.abspath(__file__))

def md5_of_pcm_file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""): h.update(b)
    return h.hexdigest()

ASPFLAC = os.path.join(here, "..", "bin", "aspflac")

def encode(src, tmpdir):
    out = os.path.join(tmpdir, os.path.basename(src) + ".flac")
    subprocess.run([ASPFLAC, "enc", src, out, str(CH), str(RATE)], check=True)
    h = hashlib.sha256()
    with open(out, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""): h.update(b)
    return out, h.hexdigest()

def read_state(base):
    for ext in ("trk", "mix", "off", "out"):
        p = f"{base}.{ext}"
        if os.path.exists(p):
            w = open(p).read().split()
            st = "off" if ext == "out" else ext
            if len(w) >= 6:
                return st, w[1], w[2], int(w[3]), int(w[4]), int(w[5])
            return st, None, None, None, None, None
    return "legacy", "0", "0", None, None, None

def main(src, name, dest):
    src, dest = os.path.realpath(src), os.path.realpath(dest)
    if dest == src or dest.startswith(src + os.sep) or src.startswith(dest + os.sep):
        sys.exit("refusing: dest and source overlap")
    if os.path.exists(dest) and os.listdir(dest):
        sys.exit("refusing: dest exists and is not empty")
    for d in ("blobs", "cache", "import"): os.makedirs(os.path.join(dest, d))
    db = sqlite3.connect(os.path.join(dest, "session.db"))
    db.executescript(open(os.path.join(here, "schema.sql")).read())
    desc = {"v": 1, "name": name, "origin": "asp4-import"}
    sid = hashlib.sha256(json.dumps(desc, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    db.execute("insert into session values(?,?,?,?)", (sid, json.dumps(desc), 1, time.time()))

    pat = re.compile(re.escape(name) + r"_(\d+)_(\d+)\.d32$")
    takes = sorted((m for m in (pat.match(f) for f in os.listdir(src)) if m),
                   key=lambda m: os.stat(os.path.join(src, m.group(0))).st_mtime)
    tmp = tempfile.mkdtemp(dir=os.path.join(dest, "import"))
    paths = [os.path.join(src, m.group(0)) for m in takes]
    with ThreadPoolExecutor(8) as ex:
        results = list(ex.map(lambda p: encode(p, tmp), paths))

    db.execute("insert into log(tick,wall,actor,type,args_json) values(0,?,?,?,?)",
        (time.time(), "importer", "import_begin", json.dumps({"src": src, "takes": len(takes)})))
    for m, p, (flac, sha) in zip(takes, paths, results):
        it, off = int(m.group(1)), int(m.group(2))
        base = p[:-4]
        size = os.path.getsize(p); mt = os.stat(p).st_mtime
        status, l, r, wr, ln, gone = read_state(base)
        dst = os.path.join(dest, "blobs", sha[:2], sha + ".flac")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst): os.remove(flac)
        else: shutil.move(flac, dst)
        cur = db.execute("insert into takes(iter,off,blob_sha,md5,frames,rate,channels,bits,status,"
            "lev_l,lev_r,wrapped,len,gone,t_start,src_name) values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (it, off, sha, md5_of_pcm_file(p), size // (4 * CH), RATE, CH, 32, status,
             l, r, wr, ln, gone, mt, os.path.basename(p)))
        tid = cur.lastrowid
        args = lambda d: json.dumps(d, sort_keys=True)
        db.execute("insert into log(tick,wall,actor,type,args_json) values(?,?,?,?,?)",
            (None, mt, "importer", "take_added", args({"take": tid, "iter": it, "off": off, "imported": True})))
        db.execute("insert into log(tick,wall,actor,type,args_json) values(?,?,?,?,?)",
            (None, mt, "importer", "take_state", args({"take": tid, "imported": True,
              "state": {"status": status, "lev": [l, r], "wrapped": wr, "len": ln, "gone": gone}})))
    shutil.rmtree(tmp)

    # sidecar notes: ledger, watch log, session log, per-comp .md
    for f, kind in (("tl.ledger.json", "ledger"), ("tl.watch.log", "watch"),
                    ("SESSION-LOG.md", "journal"), ("tl.prod.json", "prod"), ("tl.score.json", "score")):
        p = os.path.join(src, f.replace("tl", name, 1) if f.startswith("tl.") else f)
        if os.path.exists(p):
            db.execute("insert into notes(kind,text) values(?,?)", (kind, open(p, errors="replace").read()))
    for f in sorted(os.listdir(src)):
        if f.endswith(".md") and f.startswith(name + "-"):
            db.execute("insert into notes(kind,text) values(?,?)",
                       ("export:" + f, open(os.path.join(src, f), errors="replace").read()))
    db.execute("insert into log(tick,wall,actor,type,args_json) values(0,?,?,?,?)",
        (time.time(), "importer", "import_end", "{}"))
    db.commit()
    n = db.execute("select count(*) from takes").fetchone()[0]
    print(f"imported {n} takes into {dest}")

if __name__ == "__main__":
    if len(sys.argv) != 4: sys.exit(__doc__)
    main(*sys.argv[1:])
