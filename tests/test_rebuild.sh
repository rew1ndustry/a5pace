#!/bin/bash
# a5rebuild: the derived tables of session.db and the loop file are made again; the history is never touched.
here=$(cd "$(dirname "$0")/.." && pwd); B=$here/bin
t=$(mktemp -d); fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
sql() { python3 -I - "$1" "$2" <<'PY'
import sqlite3, sys
db = sqlite3.connect("%s/session.db" % sys.argv[1])
db.execute(sys.argv[2]); db.commit()
PY
}
d=$t/s; $B/a5pace new $d demo 6.25 > /dev/null 2>&1
chk "a good session is left as it is" "$B/a5rebuild $d --sql | grep -q 'schema is as it should be'"
sql $d "drop table peaks"; sql $d "drop table moves"; sql $d "create table moves(x)"
chk "derived tables made again" "$B/a5rebuild $d --sql | grep -q 'peaks was missing' && $B/a5rebuild $d --sql | grep -q 'as it should be'"
rm $d/cache/loop.d64
chk "no loop file: aspace says to run a5rebuild" "$B/a5pace run --live -S 7001 --fast $d 2>&1 | grep -q 'run a5rebuild'"
chk "the loop file is made" "$B/a5rebuild $d --tank > /dev/null && [ -s $d/cache/loop.d64 ]"
sql $d "drop table notes"
chk "a missing history table stops it" "! $B/a5rebuild $d --sql > /dev/null 2>&1"
rm -rf $t; [ $fail = 0 ] && echo "all rebuild tests passed"; exit $fail
