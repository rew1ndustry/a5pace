#!/bin/sh
# a5import: a file becomes a take in the next free 90000 slot, closed or running, in the loop or kept
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); export A5PACE_CONFIG=$t/cfg; fail=0
B=$here/bin
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$B/a5create $t/i1 i1 --seconds 6.25 --no-click > /dev/null
sox -n -r 48000 -c 1 -b 16 $t/g1.wav synth 6.25 sine 440 vol 0.3
sox -n -r 48000 -c 2 -b 16 $t/g2.flac synth 12.5 sine 330 vol 0.3
$B/a5import $t/i1 $t/g1.wav --play > $t/o1; $B/a5import $t/i1 $t/g2.flac > $t/o2
chk "closed: the first is 90001 and in the loop" "grep -q 'iteration 90001, in the loop' $t/o1"
chk "closed: the second is 90002 and kept" "grep -q 'iteration 90002, kept' $t/o2 && grep -q stereo $t/o2"
$B/a5click $t/i1 --sig 4/4 --bars 2 --style beats > $t/o3
chk "a click takes the next slot, 90003" "grep -q 'iteration 90003' $t/o3"
$B/a5pace run --live -q -S 7004 --step $t/i1 2> /dev/null &
pid=$!
n=0; while [ ! -f $t/i1/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
$B/a5import $t/i1 $t/g1.wav --level 1 --play > $t/o4
chk "running: the next slot, 90004" "grep -q 'iteration 90004, in the loop' $t/o4"
$B/a5control $t/i1 STEP 120 > /dev/null
chk "running: it is in the loop" "$B/a5control $t/i1 GRID | awk '\$1==90004{exit !(\$4==1)} END{}'"
chk "running: and the kept one is not" "$B/a5control $t/i1 GRID | awk '\$1==90002{exit !(\$4==0)} END{}'"
$B/a5control $t/i1 QUIT > /dev/null; wait $pid || true
$B/asptank rebuild $t/i1 $t/reb.d64 300000
chk "the loop file is what the store says" "$B/asptank diff $t/i1/cache/loop.d64 $t/reb.d64 | grep -q '^0 of'"
chk "not 48 kHz is refused" "sox -n -r 44100 -c 1 $t/bad.wav synth 1 sine 440 && ! $B/a5import $t/i1 $t/bad.wav > /dev/null 2>&1"
rm -rf $t
[ $fail = 0 ] && echo "all import tests passed"
exit $fail
