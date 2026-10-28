#!/bin/bash
# a run that dies: the next start says so, and a5recover puts back what it can. the simulated card is stepped, so the moment of
# the kill is exact: once while a take is being recorded, once just after it ended and was handed on.
here=$(cd "$(dirname "$0")/.." && pwd); B=$here/bin
t=$(mktemp -d); fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
q() { python3 -I - "$1" "$2" <<'PY'
import sqlite3, sys
db = sqlite3.connect("file:%s/session.db?mode=ro" % sys.argv[1], uri=True)
print(db.execute(sys.argv[2]).fetchone()[0])
PY
}
wait_ctl() { n=0; while [ ! -s $1/ctl ] && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done; }
step_until() {   # DIR "sql giving a count": one tick at a time until it is not 0
  n=0; while [ "$(q $1 "$2")" = 0 ] && [ $n -lt 400 ]; do $B/a5control $1 STEP 1 > /dev/null; n=$((n+1)); done; }

for scene in recording handed_on_unplaced handed_on_placed; do
  d=$t/$scene; $B/a5pace new $d demo 6.25 > /dev/null
  case $scene in handed_on_unplaced) export ASP_KEEPER_PAUSE_BEFORE_MS=20000 ;; handed_on_placed) export ASP_KEEPER_PAUSE_AFTER_MS=20000 ;; esac
  $B/a5pace run --live -q -S 7001 --step $d 2> $t/err1 &
  pid=$!; wait_ctl $d
  $B/a5control $d STEP 80 > /dev/null                                  # synced, and the gate has learnt its floor
  $B/a5control $d SIM BURST 1 0.1 2 > /dev/null
  if [ $scene = recording ]; then
    step_until $d "select count(*) from log where type='new'"; $B/a5control $d STEP 3 > /dev/null
    chk "$scene: no take ended yet" "[ \$(q $d \"select count(*) from log where type='end'\") = 0 ]"
  else
    step_until $d "select count(*) from log where type='end'"
  fi
  kill -9 $pid; wait $pid 2> /dev/null; rm -f $d/ctl; unset ASP_KEEPER_PAUSE_BEFORE_MS ASP_KEEPER_PAUSE_AFTER_MS
  chk "$scene: the log has no row for the take" "[ \$(q $d \"select count(*) from takes\") = 0 ]"
  [ $scene = handed_on_unplaced ] && chk "$scene: its file is still at its part" "find $d/tmp -name '*.part' -size +0 | grep -q ."
  [ $scene = handed_on_placed ] && chk "$scene: its file is already at its place" "ls $d/takes.*/*/*.a5t > /dev/null"

  # the next start says so
  $B/a5pace run --live -S 7001 --fast $d 2> $t/err2 & pid=$!; wait_ctl $d
  chk "$scene: the start reports the crash" "grep -q 'did not close properly' $t/err2"
  chk "$scene: and the log has it" "[ \$(q $d \"select count(*) from log where type='crash_detected'\") = 1 ]"
  $B/a5control $d QUIT > /dev/null; wait $pid
  # closed cleanly this time: no report again
  $B/a5pace run --live -S 7001 --fast $d 2> $t/err3 & pid=$!; wait_ctl $d; $B/a5control $d QUIT > /dev/null; wait $pid
  chk "$scene: a clean run is not reported" "! grep -q 'did not close' $t/err3"
  [ $scene = recording ] && : # nothing more to prepare

  # recover
  $B/a5recover $d --dry > $t/dry.out; chk "$scene: dry run finds it" "grep -qE 'can be (saved|put back)' $t/dry.out"
  chk "$scene: and changes nothing" "[ \$(q $d \"select count(*) from takes\") = 0 ]"
  $B/a5recover $d > $t/rec.out; rc=$?
  chk "$scene: recover finishes without loss" "[ $rc = 0 ]"
  chk "$scene: the take is back" "[ \$(q $d \"select count(*) from takes\") = 1 ]"
  f=$(ls $d/takes.*/*/*.a5t 2>/dev/null | head -1)
  chk "$scene: its file decodes whole" "$B/aspstore $d export 1 $t/$scene.d32 && [ \$(stat -c %s $t/$scene.d32) -gt 100000 ]"
  if [ $scene = recording ]; then
    chk "recording: saved as a take that is out of the loop" "grep -q 'kept out of the loop' $t/rec.out && ls $d/tmp/recovered/*.part > /dev/null"
  else
    chk "$scene: put back as it was" "grep -q 'put back' $t/rec.out"
  fi
  chk "$scene: a second recover has nothing to do" "$B/a5recover $d | grep -q '0 put back, 0 saved'"
  # and it starts: the loop is rebuilt from the store with the take in it
  $B/a5pace run --live -S 7001 --fast $d 2> $t/err4 & pid=$!; wait_ctl $d; sleep 1
  chk "$scene: starts, rebuilding" "grep -q 'rebuilding' $t/err4 && ! grep -q 'did not close' $t/err4"
  $B/a5control $d QUIT > /dev/null; wait $pid
done

# a running space is not touched
d=$t/running; $B/a5pace new $d demo 6.25 > /dev/null
$B/a5pace run -q -S 7001 --fast $d 2> /dev/null & pid=$!; wait_ctl $d
chk "a5recover will not work on a running space" "! $B/a5recover $d 2> $t/refuse && grep -q 'running' $t/refuse"
$B/a5control $d QUIT > /dev/null; wait $pid
rm -rf $t
[ $fail = 0 ] && echo "all recover tests passed"
exit $fail
