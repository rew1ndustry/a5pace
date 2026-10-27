#!/bin/sh
# the runner end to end, headless: a session, a simulated card, the control port.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); d=$t/s; fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$here/bin/aspace new $d demo 6.25 > /dev/null
$here/bin/aspace run -q -S 7001 --fast $d 2> $t/err &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c="$here/bin/aspctl $d"
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
$here/bin/aspace run -S 7001 --fast $d 2> $t/err2 &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "no rebuild after a clean close" "! grep -q rebuilding $t/err2"
$c QUIT > /dev/null; wait $pid || true
# a crash leaves the loop dirty, and the next start rebuilds it from the store
$here/bin/aspace run -q -S 7001 --fast $d 2> /dev/null &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
sleep 0.3; kill -9 $pid; wait $pid 2>/dev/null || true; rm -f $d/ctl
$here/bin/aspace run -S 7001 --fast $d 2> $t/err3 &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "a crash is noticed and the loop rebuilt" "grep -q rebuilding $t/err3"
$c QUIT > /dev/null; wait $pid || true
# a declared latency starts playing at once, without the hunt
$here/bin/aspace run -q -S 7001 --fast --latency 4321 $d 2> /dev/null &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "declared latency plays at once" "$c STATE | grep -q '\"phase\":\"play\".*\"latency\":4321'"
$c QUIT > /dev/null; wait $pid || true
# profiles: made outside a session, listed, chosen
export ASPACE_CONFIG=$t/cfg
$here/bin/aspace profiles install > /dev/null
chk "the shipped profiles are listed" "$here/bin/aspace profiles | grep -q '^violin-line .*G3'"
printf 'sync_max -40\n' > $t/cfg/defaults
d2=$t/s2; $here/bin/aspace new $d2 prof 6.25 --profile violin-line > /dev/null
$here/bin/aspace run -q -S 7001 --fast $d2 2> /dev/null &
pid=$!
n=0; while [ ! -f $d2/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c2="$here/bin/aspctl $d2"
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
$here/bin/aspace run -q -S 7001 --fast --profile knocks-and-clicks $d2 2> /dev/null &
pid=$!
n=0; while [ ! -f $d2/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "a profile given at run is used and remembered" "$c2 STATE | grep -q '\"profile\":\"knocks-and-clicks\"' && $c2 STATE | grep -q '\"lowcut\":30'"
$c2 QUIT > /dev/null; wait $pid || true
rm -rf $t
[ $fail = 0 ] && echo "all runner tests passed"
exit $fail
