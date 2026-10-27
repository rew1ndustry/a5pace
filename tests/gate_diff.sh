#!/bin/sh
# differential test of the ported gate against asp4's own code.
#   tests/gate_diff.sh [ASP4_ASPACE_C] FILES.d32...
# builds the gate section of asp4's aspace.c as a reference and compares every scan result and
# every sample the switch touches, tick by tick, over the given audio. exits non zero on any difference.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
src=${ASP4_C:-$HOME/arb/asp4/aspace.c}
t=$(mktemp -d)
{
cat <<'P'
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef int32_t s4; typedef int64_t s8; typedef uint64_t u8;
#define HZ 48000
#define PADSAM 9600
static void dbug( const char *f, ... ) { (void)f; }
static void ctlTell( char *f, ... ) { (void)f; }
#define DB_ dbug(
#define _UG )
P
sed -n '929,1353p' "$src"
cat <<'P'
static Gate G;
void ref_reset( double gate, double ring ) { gateReset( &G ); my_gate = gate; my_ring = ring; }
int64_t ref_scan( int32_t *p, int n ) { return gateScan( &G, p, n ); }
void ref_switch( int32_t *p, int32_t *prev, int n, int armed ) { gateSwitch( &G, p, prev, n, armed ); }
P
} > "$t/ref.c"
gcc -O2 -w -c -o "$t/ref.o" "$t/ref.c"
gcc -O2 -Wall -I"$here/include" -o "$t/diff" "$here/tests/gate_diff.c" "$here/src/gate.c" "$t/ref.o" -lm
"$t/diff" "$@"
rc=$?
rm -rf "$t"
exit $rc
