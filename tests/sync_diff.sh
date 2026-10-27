#!/bin/sh
# differential test of the ported sync decoder against asp4's own code.
#   tests/sync_diff.sh [CAPTURED.d32 ...]    (a captured return second, e.g. asp4's aspace_sync_got.d32)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
src=${ASP4_C:-$HOME/arb/asp4/aspace.c}
t=$(mktemp -d)
python3 "$here/tests/extract_sync_ref.py" "$src" > "$t/ref.c"

gcc -O2 -w -c -o "$t/ref.o" "$t/ref.c"
gcc -O2 -Wall -I"$here/include" -o "$t/diff" "$here/tests/sync_diff.c" "$here/src/sync.c" "$t/ref.o" -lm
"$t/diff" "$@"
rc=$?
rm -rf "$t"
exit $rc
