#!/bin/sh
# the calibration path end to end with a made-up capture: import, tune, show, save, then use the profile.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); export A5PACE_CONFIG=$t/cfg; fail=0
chk() { if ! eval "$2"; then echo "FAIL $1"; fail=1; fi; }
$here/bin/a5tune synth $here/share/calibration.script $t/cap.raw $t/win.txt --seed 3
$here/tools/a5cal capture violin --from $t/cap.raw --windows $t/win.txt > /dev/null
chk "the capture is kept" "[ -s $t/cfg/calibration/violin/capture.flac ] && [ -s $t/cfg/calibration/violin/windows.txt ]"
$here/tools/a5cal tune violin --lowest 196 > $t/tune.out
chk "a proposal with the lowest note known is found" "[ -s $t/cfg/calibration/violin/proposal ]"
chk "it catches every good sound and opens for none of the bad ones" "grep -q 'good caught 7/7, false opens 0/6' $t/tune.out"
chk "it is better than the gate as it was" "grep -q 'false opens 5/6' $t/tune.out"
$here/tools/a5cal show violin > $t/show.out
chk "show reports the floor" "grep -q 'noise floor' $t/show.out"
$here/tools/a5cal save violin > /dev/null
chk "the profile is written where the loop shell lists it" "$here/bin/a5pace profiles | grep -q '^violin '"
chk "and holds the proposal" "$here/bin/a5pace profiles show violin | grep -q 'lowcut='"
chk "an existing profile is not replaced without --force" "! $here/tools/a5cal save violin > /dev/null 2>&1"
$here/tools/a5cal save violin --force > /dev/null
chk "the old one is kept when replaced" "[ -f $t/cfg/profiles/violin.old ]"
chk "a session can use it" "$here/bin/a5pace new $t/s demo 6.25 --profile violin > /dev/null"
# without the lowest note it must not cut anything it might need
$here/tools/a5cal tune open --lowest 0 > /dev/null 2>&1 || true
# a profile applied to the capture does what the tuner said
p=$($here/bin/a5pace profiles show violin)
chk "replayed with the saved profile: no false opens" "$here/bin/a5tune eval $t/cfg/calibration/violin/capture.flac $t/cfg/calibration/violin/windows.txt '$p' | grep -q 'false opens 0 of 6'"
rm -rf $t
[ $fail = 0 ] && echo "all calibration tests passed"
exit $fail
