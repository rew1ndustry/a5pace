#!/bin/sh
# the library alone: examples/embed.c (linked against libaspace.a only) records a tone, closes, and on a second run plays it back
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$here/bin/embed $t/s 8 > $t/a.out
chk "the tone was heard" "grep -q 'in=-1[0-9] dB' $t/a.out"
$here/bin/embed $t/s 2 > $t/b.out
chk "the take is in the loop on the next open" "grep -q 'takes in loop=1' $t/b.out"
chk "and it plays" "grep -q 'out=-2[0-9] dB' $t/b.out"
chk "the library has no main" "! nm $here/build/libaspace.a 2>/dev/null | grep -q ' T main'"
[ $fail = 0 ] && echo "embed test passed" || exit 1
rm -rf "$t"
