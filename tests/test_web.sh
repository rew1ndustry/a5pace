#!/bin/sh
# a5web end to end, headless: a simulated space, the http relay, what it refuses, and the writes it allows.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); d=$t/s; fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$here/bin/a5pace new $d demo 6.25 > /dev/null
$here/bin/a5pace run --live -q -S 7001 --fast $d 2> $t/err &
pid=$!
n=0; while [ ! -f $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c="$here/bin/a5control $d"
wait_for() { n=0; while ! eval "$1" && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done; }
wait_for "$c STATE | grep -q '\"phase\":\"play\"'"
wait_for "$c STATE | grep -q '\"gate_learnt\":1'"
$c SIM BURST 1 0.1 5 > /dev/null
wait_for "$c STATE | grep -q '\"loop_takes\":1'"
wait_for "$c TAKES | grep -q 'ch 1 .*\"mono\":1'"
port=$((20000 + $$ % 20000)); U=http://127.0.0.1:$port; O="Origin: $U"
$here/bin/a5web $d --port $port --www $here/web 2> $t/web.err &
wpid=$!
wait_for "curl -s -m 2 $U/ | grep -q canvas"
chk "the page is served" "curl -s -m 2 $U/ | grep -q canvas"
chk "state is relayed" "curl -s -m 2 $U/api/state | grep -q '\"phase\":\"play\"'"
chk "the map is relayed" "curl -s -m 2 '$U/api/map?from=1&to=3' | grep -q ' in 0 '"
chk "a wrong host is refused" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} -H 'Host: evil.com' $U/api/state)\" = 403 ]"
chk "a file outside the whitelist is not served" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} $U/../Makefile)\" = 404 ]"
chk "a write needs our own origin" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} -X POST $U/api/do?c=safe)\" = 403 ]"
chk "a foreign origin is refused" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} -X POST -H 'Origin: http://evil.com' $U/api/do?c=safe)\" = 403 ]"
chk "a get cannot write" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} $U/api/do?c=safe)\" = 404 ]"
chk "quit is not an action" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} -X POST -H '$O' $U/api/do?c=quit)\" = 400 ]"
chk "safe, by the page's route" "curl -s -m 2 -X POST -H '$O' '$U/api/do?c=safe' >/dev/null; $c LINE | grep -q SAFE"
chk "a half typed command waits" "[ \"\$(curl -s -m 2 -X POST -H '$O' '$U/api/mix?s=%2B2&t=1.0')\" = WAIT ]"
chk "a bad one is said to be bad" "[ \"\$(curl -s -m 2 -X POST -H '$O' '$U/api/mix?s=%2B2m1&t=1.0')\" = BAD ]"
chk "words are not a command" "[ \"\$(curl -s -m 2 -o /dev/null -w %{http_code} -X POST -H '$O' '$U/api/mix?s=QUIT&t=')\" = 400 ]"
chk "no take, no mix" "[ \"\$(curl -s -m 2 -X POST -H '$O' '$U/api/mix?s=%2B2.&t=')\" = NONE ]"
it=$($c GRID | head -1 | cut -d' ' -f1); off=$($c GRID | head -1 | cut -d' ' -f2)
chk "a mix reaches the take" "[ \"\$(curl -s -m 2 -X POST -H '$O' '$U/api/mix?s=%2B2!&t=$it.$off')\" = 'OK 1' ]"
wait_for "$c GRID | grep -q '^$it $off [0-9]* [01] 1 2 '"
chk "and shows as its gain" "$c GRID | grep -q '^$it $off [0-9]* [01] 1 2 '"
kill $wpid 2>/dev/null || true
# a5web starts a space that is not running (through a5start, here on a simulated card), and --quiet says nothing
d2=$t/auto
$here/bin/a5pace new $d2 auto 6.25 > /dev/null
PATH=$here/tools:$here/bin:$PATH A5START_EDGE=sim $here/bin/a5web $d2 --port $((port+1)) --www $here/web --quiet > $t/auto.out 2>&1 &
apid=$!
wait_for "$here/bin/a5control $d2 LINE 2>/dev/null | grep -q ."
chk "a5web started the space" "$here/bin/a5control $d2 LINE | grep -q 'iter'"
chk "and --quiet kept quiet" "[ ! -s $t/auto.out ]"
chk "a path that is not a space stops a5web with a message" "PATH=$here/tools:$here/bin:$PATH $here/bin/a5web $t/nosuch --port $((port+2)) --www $here/web 2>&1 | grep -q 'nothing to show'"
$here/bin/a5control $d2 QUIT > /dev/null 2>&1 || true
kill $apid 2>/dev/null || true
$c QUIT > /dev/null
wait $pid || true
rm -rf $t
[ $fail = 0 ] && echo "all web tests passed"
exit $fail
