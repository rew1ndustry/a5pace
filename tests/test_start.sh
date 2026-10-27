#!/bin/sh
# a space that is not running is started by a5shell, a5control --start and a5start; how is decided in one place.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); export A5PACE_CONFIG=$t/cfg; fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
PATH=$here/tools:$here/bin:$PATH; export PATH
$here/bin/a5pace new $t/s st 6.25 > /dev/null
echo "edge sim" > $t/s/start
chk "not running to begin with" "! $here/bin/a5control $t/s LINE > /dev/null 2>&1"
chk "a5control --start starts it" "$here/bin/a5control --start $t/s STATE | grep -q '\"phase\"'"
chk "a5start says it is already running" "$here/tools/a5start $t/s | grep -q 'already running'"
$here/bin/a5control $t/s QUIT > /dev/null
n=0; while [ -f $t/s/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
$here/bin/a5shell $t/s --no-start > $t/ns.out 2>&1 < /dev/null || true
chk "a5shell --no-start does not start it" "grep -q 'not running' $t/ns.out && ! $here/bin/a5control $t/s LINE > /dev/null 2>&1"
printf q | $here/bin/a5shell $t/s > $t/sh.out 2>&1 || true
chk "a5shell starts it, shows it, and leaves it running" "grep -q 'starting it' $t/sh.out && grep -q 'a5shell: st' $t/sh.out && $here/bin/a5control $t/s LINE | grep -q ."
$here/bin/a5control $t/s QUIT > /dev/null
n=0; while [ -f $t/s/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
# the edge comes from the session's start file, then the defaults, then the environment wins
rm -f $t/s/start; mkdir -p $t/cfg; echo "edge sim" > $t/cfg/defaults
chk "the defaults can say how" "A5START_EDGE= $here/tools/a5start $t/s | grep -q 'simulated'"
$here/bin/a5control $t/s QUIT > /dev/null
n=0; while [ -f $t/s/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "a bad edge is refused" "! A5START_EDGE=nonsense $here/tools/a5start $t/s > /dev/null 2>&1"
rm -rf $t
[ $fail = 0 ] && echo "all start tests passed"
exit $fail
