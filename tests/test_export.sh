#!/bin/sh
# a5export: files fit the loop exactly, the loop file is what the loop holds, a second call changes nothing,
# a closed session and a running one both work, nothing is deleted.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); export A5PACE_CONFIG=$t/cfg; fail=0
B=$here/bin
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$B/a5create $t/e1 e1 --seconds 6.25 --no-click > /dev/null
$B/a5pace run --live -q -S 7003 --step $t/e1 2> /dev/null &
pid=$!
n=0; while [ ! -f $t/e1/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
$B/a5control $t/e1 SIM BURST 2 0.1 5 > /dev/null
$B/a5control $t/e1 STEP 40 > /dev/null; $B/a5control $t/e1 STEP 40 > /dev/null
$B/a5control $t/e1 SIM BURST 2 0.1 5 > /dev/null; $B/a5control $t/e1 STEP 250 > /dev/null
$B/a5click $t/e1 --sig 4/4 --bars 2 --insert > /dev/null; $B/a5control $t/e1 STEP 80 > /dev/null
# --- cleanup: a take that leaves the loop is trashed (to the space's trash here: no gio in the test)
$B/a5export $t/e1 mix > /dev/null
$B/a5control $t/e1 MIX 1 5 MUTE NOW > /dev/null; $B/a5control $t/e1 STEP 120 > /dev/null
$B/a5export $t/e1 mix > /dev/null
chk "without --cleanup nothing is moved" "[ -f $t/e1/export/e1_00001_00005.flac ]"
env PATH=/nonexistent $B/a5export $t/e1 mix --cleanup > $t/o0
chk "--cleanup moves it out of the export, to the space's trash" "[ ! -f $t/e1/export/e1_00001_00005.flac ] && [ -f $t/e1/trash/e1_00001_00005.flac ]"
chk "--cleanup is for mix only" "! $B/a5export $t/e1 all --cleanup > /dev/null 2>&1"
$B/a5export $t/e1 all > /dev/null
chk "a muted take keeps its file name (the state is not in it)" "[ -f $t/e1/export/e1_00001_00005.flac ] && [ -z \"\$(ls $t/e1/export | grep -E '^(mute|loop)_')\" ]"
$B/a5control $t/e1 MIX 1 5 PLAY NOW > /dev/null; $B/a5control $t/e1 STEP 120 > /dev/null
$B/a5export $t/e1 all > /dev/null
chk "and back again" "[ -f $t/e1/export/e1_00001_00005.flac ]"
$B/a5export $t/e1 all --prefix=zz --to $t/pz > /dev/null
chk "--prefix replaces the folder name" "[ -f $t/pz/zz_00001_00005.flac ]"
$B/a5export $t/e1 all --fold --to $t/pf > /dev/null
chk "fold: a take over the loop edge is clips of the loop length, one that fits has no _nth" "[ -f $t/pf/e1_00001_00005_000.flac ] && [ -f $t/pf/e1_00001_00005_001.flac ] && [ -f $t/pf/e1_90001_00000.flac ] && [ \$(soxi -D $t/pf/e1_00001_00005_000.flac) = \$(soxi -D $t/pf/e1_00001_00005_001.flac) ] && [ \$(soxi -D $t/pf/e1_00001_00005_000.flac) = \$(soxi -D $t/pf/e1_90001_00000.flac) ]"
chk "a mono take is exported mono, a stereo one stereo" "[ \$(soxi -c $t/e1/export/e1_90001_00000.flac) = 1 ] && [ \$(soxi -c $t/e1/export/e1_00001_00005.flac) = 2 ]"
chk "no mode, no export: usage" "! $B/a5export $t/e1 > /dev/null 2>&1 && [ \$($B/a5export $t/e1 2>&1 | grep -c usage) = 1 ]"
# --- while it runs
$B/a5export $t/e1 all > $t/o1; $B/a5export $t/e1 loop > $t/o2
chk "running: a file for each iteration and the click" "[ -f $t/e1/export/e1_00001_00005.flac ] && [ -f $t/e1/export/e1_90001_00000.flac ]"
chk "running: every file is a whole number of loops" "for f in $t/e1/export/*_*_*.flac; do python3 -I -c \"import subprocess,sys;d=float(subprocess.check_output(['soxi','-D',sys.argv[1]]));sys.exit(0 if abs(d/6.25-round(d/6.25))<1e-9 and d>0 else 1)\" \$f || exit 1; done"
$B/a5export $t/e1 all > $t/o3
chk "a second call changes nothing" "grep -q ' 0 written' $t/o3"
$B/a5control $t/e1 QUIT > /dev/null; wait $pid || true
# --- closed: the loop file is exactly the loop file of the session
rm -f $t/e1/export/e1_mix_?????.flac; $B/a5export $t/e1 loop > /dev/null
chk "the loop export is the loop" "python3 -I - $t <<'PY'
import subprocess, struct, sys, glob
t=sys.argv[1]
raw=subprocess.check_output(['sox',glob.glob(t+'/e1/export/e1_mix_?????.flac')[0],'-t','raw','-e','signed','-b','32','-L','-c','2','-'])
d=open(t+'/e1/cache/loop.d64','rb').read()
S=128+9600*16
assert d[:8]==b'a5chip\xa5\xa1'
vals=()
for k in range((len(d)+S-1)//S):
    c=d[k*S+128:(k+1)*S]
    vals+=struct.unpack('<%dq'%(len(c)//8), c)
n=len(vals)//2
mine=struct.unpack('<%di'%(2*n), raw[:8*n])
clip=lambda v: max(-2147483647,min(2147483647,v))
bad=sum(1 for a,b in zip(vals,mine) if clip(a)!=b)
sys.exit(0 if len(raw)==8*n and bad==0 else 1)
PY"
rm -f $t/e1/export/e1_00001_00005.flac; $B/a5export $t/e1 all > /dev/null
chk "a missing file is made again" "[ -f $t/e1/export/e1_00001_00005.flac ]"
rm -rf $t
[ $fail = 0 ] && echo "all export tests passed"
exit $fail
