#!/bin/sh
# a5sdl end to end with no card: the soft loop (aspace hears itself through the edge), the sync, a clean quit.
# skipped when a5sdl was not built (it needs SDL2: make bin/a5sdl).
here=$(cd "$(dirname "$0")/.." && pwd)
[ -x $here/bin/a5sdl ] || { echo "test_sdl: a5sdl not built, skipped"; exit 0; }
t=$(mktemp -d); d=$t/s; fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$here/bin/a5pace new $d demo 20 > /dev/null
SDL_AUDIODRIVER=dummy $here/bin/a5sdl $d --soft-loop --live -q 2> $t/err &
pid=$!
n=0; while [ ! -s $d/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
c="$here/bin/a5control $d"
n=0; while ! $c STATE 2>/dev/null | grep -q '"phase":"play"' && [ $n -lt 150 ]; do sleep 0.2; n=$((n+1)); done
chk "synced through the soft loop" "$c STATE | grep -q '\"phase\":\"play\"'"
chk "the round trip is the primed tick" "$c STATE | grep -q '\"latency\":9600'"
kill -INT $pid
n=0; while kill -0 $pid 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
chk "a signal closes it" "! kill -0 $pid 2>/dev/null"
chk "and aspace closed cleanly" "[ ! -s $d/ctl ] || ! $c LINE >/dev/null 2>&1"
[ $fail = 0 ] && echo "a5sdl test passed" || { cat $t/err; exit 1; }
rm -rf "$t"
