#!/bin/sh
# create (seconds, music, an audio file) and the metronome inserter, offline and then run headless.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); export A5PACE_CONFIG=$t/cfg; fail=0
B=$here/bin
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
# --- music: 4 bars of 4/4 at 120 bpm is 8 s, with the default click
$B/a5create $t/m1 m1 --sig 4/4 --bpm 120 --bars 4 > $t/m1.out
chk "music gives the exact loop length" "grep -q '384000 frames' $t/m1.out"
chk "and a click track" "grep -q 'click beats .* 16 clicks' $t/m1.out"
python3 -I - $t/m1 $B <<'PY' || fail=1
import sqlite3, subprocess, sys, numpy as np
d,B=sys.argv[1],sys.argv[2]
db=sqlite3.connect(d+"/session.db")
sha,iter_=db.execute("select blob_sha,iter from takes").fetchone()
a=np.frombuffer(subprocess.run([B+"/aspflac","dec",f"{d}/blobs/{sha[:2]}/{sha}.flac","-"],capture_output=True).stdout,dtype='<i4').astype(float)
ok=True
if len(a)!=384000: print("FAIL click length",len(a)); ok=False
on=[i for i in range(1,len(a)) if abs(a[i])>1e7 and np.max(abs(a[max(0,i-300):i]))<=1e7]
starts=[o for k,o in enumerate(on) if k==0 or o-on[k-1]>2000]
if len(starts)!=16 or any(abs(s-24000*k)>40 for k,s in enumerate(starts)): print("FAIL click places",starts[:5],len(starts)); ok=False
pk=[abs(a[s:s+400]).max() for s in starts]
if not (pk[0]>pk[1]*1.3 and pk[4]>pk[5]*1.3 and abs(pk[1]-pk[2])<pk[1]*0.05): print("FAIL accent",pk[:6]); ok=False
if iter_<90001: print("FAIL generated iteration should be in the 90000 series",iter_); ok=False
sys.exit(0 if ok else 1)
PY
# --- 3/4 and 6/8, other lengths
$B/a5create $t/m2 m2 --sig 3/4 --bpm 90 --bars 8 > $t/m2.out; chk "3/4 at 90, 8 bars is 16 s with 24 clicks" "grep -q '768000 frames' $t/m2.out && grep -q '24 clicks' $t/m2.out"
$B/a5create $t/m3 m3 --sig 6/8 --bpm 60 --bars 2 --click bars --click beats > $t/m3.out; chk "6/8 beats on dotted quarters: 2 bars is 6 s, 4 beats, 2 bar clicks, two takes" "grep -q '288000 frames' $t/m3.out && grep -q 'click bars .* 2 clicks' $t/m3.out && grep -q 'click beats .* 4 clicks' $t/m3.out"
# --- seconds with decimals; no click
$B/a5create $t/s1 s1 --seconds 7.25 > $t/s1.out; chk "seconds with decimals" "grep -q '348000 frames' $t/s1.out && ! grep -q click $t/s1.out"
$B/a5create $t/s1b s1b 7.25 > /dev/null; chk "and bare, as a5create DIR NAME 7.25" "[ -d $t/s1b ]"
# --- the metronome inserter on another space: fits, warns, adds styles, clears
$B/a5click $t/s1 --sig 4/4 --bpm 100 --bars 4 --insert > $t/c1.out
chk "it says when the loop does not fit and what tempo does" "grep -q 'do not fit' $t/c1.out && grep -q '4 bars of 4/4' $t/c1.out"
chk "a strict fit refuses" "! $B/a5click $t/s1 --sig 4/4 --bpm 100 --bars 4 --strict > /dev/null 2>&1"
$B/a5click $t/s1 --sig 4/4 --bpm 33.103 --bars 2 --style eighths --style offbeat --pan l --level 0 --insert > $t/c2.out
chk "two more tracks in their own takes" "grep -q 'eighths .* 16 clicks' $t/c2.out && grep -q 'offbeat .* 8 clicks' $t/c2.out"
$B/a5create $t/s3 s3 --seconds 6.25 > /dev/null
$B/a5click $t/s3 --sig 4/4 --bars 2 --style offbeat > $t/c4.out
chk "without --insert a click is only kept" "grep -q 'kept, not in the loop' $t/c4.out"
$B/a5click $t/s1 --clear > $t/c3.out; chk "clear takes them out" "grep -q '3 click track' $t/c3.out"
# --- an audio file becomes the loop
python3 -I - $t <<'PY'
import wave, struct, math, sys
t=sys.argv[1]
w=wave.open(t+"/tone.wav","wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<h",int(12000*math.sin(2*math.pi*440*i/48000))) for i in range(120000))); w.close()
w=wave.open(t+"/slow.wav","wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100); w.writeframes(b"\0\0"*50000); w.close()
PY
$B/a5create $t/f1 f1 --from $t/tone.wav > $t/f1.out
chk "a wav becomes the loop, its own length" "grep -q '120000 frames' $t/f1.out && grep -q 'mono' $t/f1.out"
python3 -I - $t/f1 $B <<'PY' || fail=1
import sqlite3, subprocess, sys, numpy as np, math
d,B=sys.argv[1],sys.argv[2]
db=sqlite3.connect(d+"/session.db")
sha,ch,fr=db.execute("select blob_sha,channels,frames from takes").fetchone()
a=np.frombuffer(subprocess.run([B+"/aspflac","dec",f"{d}/blobs/{sha[:2]}/{sha}.flac","-"],capture_output=True).stdout,dtype='<i4')
exp=np.array([int(12000*math.sin(2*math.pi*440*i/48000))<<16 for i in range(120000)],dtype='<i4')
sys.exit(0 if ch==1 and fr==120000 and (a==exp).all() else 1)
PY
chk "the take is the file, sample for sample" "true"
chk "the wrong rate is refused with a hint" "$B/a5create $t/f2 f2 --from $t/slow.wav 2>&1 | grep -q 'works at 48000'"
$B/aspflac enc /dev/null /dev/null 2>/dev/null || true
python3 -I -c "
import struct,sys
open('$t/st.d32','wb').write(b''.join(struct.pack('<ii',((i*37)%100000)*20000,-((i*11)%100000)*20000) for i in range(60000)))"
$B/aspflac enc $t/st.d32 $t/st.flac 2 48000
$B/a5create $t/f3 f3 --from $t/st.flac > $t/f3.out; chk "a flac too, stereo" "grep -q '60000 frames' $t/f3.out && grep -q 'stereo' $t/f3.out"
# --- run one headless: the click is in the loop, and the loop matches a rebuild from the store
$B/a5pace run --live -q -S 7001 --fast $t/m1 2> /dev/null &
pid=$!
n=0; while [ ! -f $t/m1/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
chk "it starts and has the click in the loop" "$B/a5control $t/m1 STATE | grep -q '\"loop_takes\":1' && $B/a5control $t/m1 TAKES | grep -q generated"
$B/a5control $t/m1 QUIT > /dev/null; wait $pid || true
$B/asptank rebuild $t/m1 $t/reb.d64 384000
chk "the loop file is exactly what the store says" "$B/asptank diff $t/m1/cache/loop.d64 $t/reb.d64 | grep -q '^0 of'"
# --- a5click on a space that is running: told, not written behind its back; kept unless --insert; clear works
$B/a5create $t/r1 r1 --seconds 6.25 > /dev/null
$B/a5pace run --live -q -S 7002 --step $t/r1 2> /dev/null &
pid=$!
n=0; while [ ! -f $t/r1/ctl ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
$B/a5click $t/r1 --sig 4/4 --bars 2 --style beats > $t/c5.out; $B/a5click $t/r1 --sig 4/4 --bars 2 --style offbeat --insert > $t/c6.out
$B/a5control $t/r1 STEP 60 > /dev/null
chk "running: the kept click is not in the loop, the inserted one is" "$B/a5control $t/r1 GRID | awk '\$1==-1{a=\$4} \$1==-2{b=\$4} END{exit !(a==0 && b==1)}'"
$B/a5click $t/r1 --clear > /dev/null; $B/a5control $t/r1 STEP 120 > /dev/null
chk "running: clear takes the inserted click out" "$B/a5control $t/r1 GRID | awk '\$4==1{n++} END{exit n>0}'"
$B/a5control $t/r1 QUIT > /dev/null; wait $pid || true
$B/asptank rebuild $t/r1 $t/reb1.d64 300000
chk "running: the loop file is what the store says" "$B/asptank diff $t/r1/cache/loop.d64 $t/reb1.d64 | grep -q '^0 of'"
rm -rf $t
[ $fail = 0 ] && echo "all create and click tests passed"
exit $fail
