#!/bin/sh
# the runner end to end, headless: a session, a simulated card, the control port.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); d=$t/s; fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$here/bin/a5pace new $d demo 6.25 > /dev/null
$here/bin/a5pace run -q -S 7001 --fast $d 2> $t/err &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c="$here/bin/a5control $d"
wait_for() { n=0; while ! eval "$1" && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done; }
wait_for "$c STATE | grep -q '\"phase\":\"play\"'"
chk "synced to the simulated latency" "$c STATE | grep -q '\"latency\":7001'"
wait_for "$c STATE | grep -q '\"gate_learnt\":1'"
$c SIM BURST 1 0.1 5 > /dev/null
wait_for "$c STATE | grep -q '\"loop_takes\":1'"
chk "a take was recorded, from one channel, as mono" "$c TAKES | grep -q 'ch 1 .*\"mono\":1'"
$c SAFE > /dev/null
chk "safe shows" "$c LINE | grep -q SAFE"
$c AUTOMIX overlap 3 > /dev/null
chk "automix set" "$c STATE | grep -q '\"automix\":\"overlap\",\"automix_n\":3'"
chk "an unknown command is refused" "! $c NONSENSE > /dev/null"
chk "revert needs safe, and works in it" "$c REVERT 5 > /dev/null"
chk "and the take is gone" "[ -z \"$($c TAKES)\" ]"
$c QUIT > /dev/null
wait $pid || true
chk "closed cleanly, control file removed" "[ ! -f $d/ctl ]"
# open it again: it must not need a rebuild
$here/bin/a5pace run -S 7001 --fast $d 2> $t/err2 &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "no rebuild after a clean close" "! grep -q rebuilding $t/err2"
$c QUIT > /dev/null; wait $pid || true
# a crash leaves the loop dirty, and the next start rebuilds it from the store
$here/bin/a5pace run -q -S 7001 --fast $d 2> /dev/null &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
sleep 0.3; kill -9 $pid; wait $pid 2>/dev/null || true; rm -f $d/ctl
$here/bin/a5pace run -S 7001 --fast $d 2> $t/err3 &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "a crash is noticed and the loop rebuilt" "grep -q rebuilding $t/err3"
$c QUIT > /dev/null; wait $pid || true
# a declared latency starts playing at once, without the hunt
$here/bin/a5pace run -q -S 7001 --fast --latency 4321 $d 2> /dev/null &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "declared latency plays at once" "$c STATE | grep -q '\"phase\":\"play\".*\"latency\":4321'"
$c QUIT > /dev/null; wait $pid || true
# profiles: made outside a session, listed, chosen
export A5PACE_CONFIG=$t/cfg
$here/bin/a5pace profiles install > /dev/null
chk "the shipped profiles are listed" "$here/bin/a5pace profiles | grep -q '^violin-line .*G3'"
printf 'sync_max -40\n' > $t/cfg/defaults
d2=$t/s2; $here/bin/a5pace new $d2 prof 6.25 --profile violin-line > /dev/null
$here/bin/a5pace run -q -S 7001 --fast $d2 2> /dev/null &
pid=$!
n=0; while [ ! -f $d2/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c2="$here/bin/a5control $d2"
chk "a new session carries the defaults and the chosen profile" "$c2 SETTINGS | grep -q 'sync_max=-40' && $c2 SETTINGS | grep -q 'lowcut=190' && $c2 STATE | grep -q '\"profile\":\"violin-line\"'"
chk "the profile is in force" "$c2 STATE | grep -q '\"lowcut\":190,\"strict\":1'"
chk "the loop shell can list them" "$c2 PROFILES | grep -q 'bass-line'"
$c2 PROFILE bass-line > /dev/null
chk "and choose another" "$c2 STATE | grep -q '\"lowcut\":35' && $c2 STATE | grep -q '\"profile\":\"bass-line\"'"
chk "an unknown profile is refused" "! $c2 PROFILE nonesuch > /dev/null"
chk "tick times are measured" "$c2 STATE | grep -q 'tick_ms_max'"
$c2 SAFE > /dev/null; $c2 REVERT 3 > /dev/null
chk "a revert brings back the settings of that time" "$c2 STATE | grep -q '\"profile\":\"violin-line\"'"
$c2 QUIT > /dev/null; wait $pid || true
$here/bin/a5pace run -q -S 7001 --fast --profile knocks-and-clicks $d2 2> /dev/null &
pid=$!
n=0; while [ ! -f $d2/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "a profile given at run is used and remembered" "$c2 STATE | grep -q '\"profile\":\"knocks-and-clicks\"' && $c2 STATE | grep -q '\"lowcut\":30'"
$c2 QUIT > /dev/null; wait $pid || true
# wipe, history, and going back to a take by iteration and offset
d3=$t/s3; $here/bin/a5pace new $d3 hist 6.25 > /dev/null
$here/bin/a5pace run -q -S 7001 --fast $d3 2> /dev/null &
pid=$!
n=0; while [ ! -f $d3/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c3="$here/bin/a5control $d3"
wait_for "$c3 STATE | grep -q '\"gate_learnt\":1'"
$c3 SIM BURST 2 0.1 5 > /dev/null
wait_for "$c3 STATE | grep -q '\"loop_takes\":1'"
sleep 2
$c3 SIM BURST 2 0.1 5 > /dev/null
wait_for "$c3 STATE | grep -q '\"loop_takes\":2'"
wait_for "$c3 STATE | grep -q '\"take_open\":0'"
chk "history lists the takes, newest first, with how many were in the loop" "$c3 HISTORY | head -1 | grep -q 'take, iteration' && $c3 HISTORY | head -1 | grep -q '2 in the loop' && $c3 HISTORY | sed -n 2p | grep -q '1 in the loop'"
first=$($c3 TAKES | head -1 | sed 's/.*iter \([0-9]*\) off \([0-9]*\) .*/\1 \2/')
chk "wipe takes the recorded takes out" "$c3 WIPE | grep -q '2 take' && $c3 STATE | grep -q '\"loop_takes\":0'"
chk "history records it" "$c3 HISTORY | grep -q 'wipe: 2 take'"
$c3 SAFE > /dev/null
chk "going back to a take by iteration and offset" "$c3 REVERT TAKE $first > /dev/null && $c3 STATE | grep -q '\"loop_takes\":1'"
chk "an unknown iteration is refused" "! $c3 REVERT TAKE 99 99 > /dev/null"
chk "the line shows the automix amount" "$c3 AUTOMIX OVERLAP 3 > /dev/null && $c3 LINE | grep -q 'automix overlap 3'"
# keep: recorded and stored, not put into the loop; help; the space's name
chk "the state names the space" "$c3 STATE | grep -q '\"name\":\"hist\"'"
chk "help lists the commands" "$c3 HELP | grep -q 'WIPE' && $c3 HELP | grep -q 'AUTOMIX LOOP | KEEP'"
$c3 SAFE > /dev/null; $c3 REVERT 3 > /dev/null; $c3 LIVE > /dev/null
$c3 AUTOMIX KEEP > /dev/null
chk "the line says keep" "$c3 LINE | grep -q 'automix keep'"
before=$($c3 STATE | sed 's/.*"loop_takes":\([0-9]*\).*/\1/')
$c3 SIM BURST 2 0.1 5 > /dev/null
wait_for "$c3 TAKES | grep -q '\"status\":\"off\"'"
wait_for "$c3 STATE | grep -q '\"take_open\":0'"
chk "a kept take is stored but not in the loop" "$c3 TAKES | grep -q '\"status\":\"off\"' && [ $($c3 STATE | sed 's/.*\"loop_takes\":\([0-9]*\).*/\1/') = $before ]"
chk "and the history says so" "$c3 HISTORY | head -1 | grep -q 'kept (not in the loop)'"
$c3 AUTOMIX LOOP > /dev/null
chk "back to the plain loop" "$c3 LINE | grep -q 'automix loop'"
$c3 QUIT > /dev/null; wait $pid || true
# the tape: the input as it came, in flac; the loop as it went out only if asked
d4=$t/s4; $here/bin/a5pace new $d4 tape 6.25 > /dev/null
$here/bin/a5pace run -q -S 7001 --fast --tape $d4 2> /dev/null &
pid=$!
n=0; while [ ! -f $d4/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c4="$here/bin/a5control $d4"
chk "only the input is taped by default" "$c4 TAPE | grep -q 'input tape: .*/tape/input-' && $c4 TAPE | grep -q 'loop tape: off'"
wait_for "$c4 STATE | grep -q '\"gate_learnt\":1'"
$c4 SIM BURST 1 0.1 3 > /dev/null
sleep 1
$c4 TAPE LOOP ON $t/loop.flac > /dev/null
sleep 1
chk "the loop can be taped for a while" "$c4 TAPE | grep -q 'loop tape: $t/loop.flac'"
$c4 TAPE LOOP OFF > /dev/null
chk "and stopped" "$c4 TAPE | grep -q 'loop tape: off'"
tk=$($c4 STATE | sed 's/.*"tick":\([0-9]*\).*/\1/')
$c4 QUIT > /dev/null; wait $pid || true
in=$(ls $d4/tape/input-*.flac)
chk "the input tape decodes with its checksum" "$here/bin/aspflac dec $in $t/in.d32"
chk "it is every tick of the run" "[ \$(( \$(stat -c %s $t/in.d32) / 8 )) -ge \$(( tk * 9600 )) ] && [ \$(( \$(stat -c %s $t/in.d32) / 8 % 9600 )) = 0 ]"
chk "the burst is in it" "python3 -I -c \"import numpy as n;a=n.fromfile('$t/in.d32','<i4');import sys;sys.exit(0 if abs(a).max()>1e8 else 1)\""
chk "the stopped loop tape is a valid flac, shorter than the run" "$here/bin/aspflac dec $t/loop.flac $t/loop.d32 && [ \$(stat -c %s $t/loop.d32) -lt \$(stat -c %s $t/in.d32) ]"
rm -rf $t
[ $fail = 0 ] && echo "all runner tests passed"
exit $fail
