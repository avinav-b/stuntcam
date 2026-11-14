#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# wd-traced.sh <rt> [reps] — the watchdog's debugger rule (safety.inc SCWatchdog: "while P_TRACED, an expired op is recorded as
#   unknown-hang and never counted"), on a sim armed by scripts/boot.sh <rt> stub, with fixtures/plain (w4 e27, TODO U5 item 2).
#   traced   x reps: plain with SIMCAM_WD_TEST=hang, launched with simctl --wait-for-debugger; scripts/lldb-attach.py attaches
#            by pid, continues, stays attached and running (no breakpoints, the watchdog keeps ticking) until the engine log has
#            both 'over its deadline while a debugger is attached' lines, then detaches and quits; the app runs on to DONE
#            untraced. Expect: 2 'unknown-hang photo', 0 'hang', 'debugger detached', no latch, no latched-* marker. Then a
#            launch with no debugger and no test hook: armed, 14/14, no latch.
#   control  x reps: the same hang launch with no debugger: 2 'hang photo' (n=1, n=2) and an in-process latch; the next
#            launch is latched with a latched-<build> marker. A SIMCAM_SENTINEL=clear launch after each rep.
#   Prints "PASS|FAIL <step> — evidence"; exits 1 if any step failed. Results: $SIMCAM_RESULTS/plain-<rt>-boot-<tag>-<step>/
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh
RT=${1:?usage: $0 <rt> [reps]}; N=${2:-3}
V=SIMCAM_UDID_$RT; U=${(P)V:-}; [[ -n $U ]] || { echo "$0: set $V" >&2; exit 2; }
BP=dev.simcam.spike.m10plain; ST=$SIMCAM_HOME/sims/$U/state/$BP; EV=$ST/events.log
BUILD=$(<$ROOT/build/current_dylib.txt); BUILD=${BUILD:h:t}
PY=/Applications/Xcode.app/Contents/Developer/usr/bin/python3; LP=$(xcrun lldb -P)
TAG=wdt-$(date +%H%M%S); fails=0
res() { print -r -- "$SIMCAM_RESULTS/plain-$RT-boot-$TAG-$1"; }
run() { local n=$1; shift; XENV="${(F)@}"$'\n' SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh plain $RT boot $TAG-$n >/dev/null 2>&1; }
nl() { [[ -f $1 ]] && wc -l < $1 | tr -d ' ' || echo 0; }
since() { [[ -f $EV ]] && tail -n +$(( $1 + 1 )) $EV || true; }
cnt() { local n; n=$(grep -cE "$1" $2 2>/dev/null); print -r -- ${n:-0}; }
tmpd() { local c=$(xcrun simctl get_app_container $U $BP data 2>/dev/null); [[ $c == $HOME/* ]] && print -r -- "$c/tmp/simcam-state/$BP" || print -r -- /nonexistent; }
markers() { print -l $ST/latched-*(N) $(tmpd)/latched-*(N); }
ok() { echo "PASS $1 — $2"; }
bad() { echo "FAIL $1 — $2"; fails=$((fails + 1)); }
clear_engine() { run $1 SIMCAM_SENTINEL=clear; }
dbgleft() { [[ -n $1 ]] || { echo yes; return; }; pgrep -f "debugserver.*[^0-9]$1([^0-9]|\$)" >/dev/null && echo yes || echo no; }   # a debugserver still naming the whole pid; no pid counts as left

echo "wd-traced $RT: udid $U build $BUILD events $EV tag $TAG"
[[ -e $HOME/.lldbinit-Xcode ]] && echo "note: ~/.lldbinit-Xcode exists before the run"
clear_engine clear0; echo "markers after the first clear: $(markers | grep -c .)"
for i in {1..$N}; do
  # --- traced: lldb attached and running through the whole activation ---
  n0=$(nl $EV); d=$(res tr$i); att=$SIMCAM_RESULTS/wd-traced-$RT-$TAG-attach$i.txt
  ( XENV="SIMCAM_WD_TEST=hang"$'\n' SIMCAM_NO_RETRY=1 WAIT_DEBUGGER=1 $ROOT/scripts/run.sh plain $RT boot $TAG-tr$i >/dev/null 2>&1 ) & rp=$!
  pid=; for _ in {1..200}; do pid=$(pgrep -f "Devices/$U/data/Containers/Bundle/Application/.*/m5app.app/m5app" | head -1); [[ -n $pid ]] && break; sleep 0.05; done
  el="$(cat $ROOT/build/bootlogdir-$RT.txt)/dylib-$pid.log"   # this pid's engine log (boot mode)
  if [[ -n $pid ]]; then PYTHONPATH=$LP $PY $ROOT/scripts/lldb-attach.py $pid $el '(?s)over its deadline while a debugger.*over its deadline while a debugger' 40 > $att 2>&1
  else echo "ATTACH| no pid" > $att; fi
  wait $rp; left=$(dbgleft $pid)
  since $n0 > $d/events.txt
  uh=$(cnt ' unknown-hang photo ' $d/events.txt); hg=$(cnt ' hang ' $d/events.txt); tr1=$(cnt ' activate .*traced=1' $d/events.txt)
  wl=$(cnt 'photo op over its deadline while a debugger is attached \(not counted\)' $d/dylib.log); lt=$(cnt 'latched' $d/dylib.log)
  mk=$(markers | grep -c .); ps=$(cnt '\| PASS' $d/app.log); at=$(grep -m1 -oE 'attached pid [0-9]+ in [0-9.]+ s|attach FAILED.*|no pid' $att)
  wd=$(cnt 'watchdog: debugger detached' $d/dylib.log); dt=$(cnt 'ATTACH\| .* detached' $att)
  if [[ $at == attached* && $tr1 == 1 && $uh == 2 && $hg == 0 && $wl == 2 && $lt == 0 && $mk == 0 && $ps == 14 && $left == no && $wd == 1 && $dt == 1 ]]
  then ok traced-$i "$at, activate traced=1, 2 unknown-hang photo, 0 hang, 0 latched lines, 0 markers, lldb detached (watchdog saw it), 14/14, no debugserver left"
  else bad traced-$i "attach='$at' traced=1:$tr1 unknown-hang=$uh hang=$hg watchdog-lines=$wl latched=$lt markers=$mk pass=$ps lldb-detached=$dt watchdog-detached=$wd debugserver-left=$left (see $d)"; fi
  # --- the next launch, no debugger, no test hook: still armed ---
  n0=$(nl $EV); run nx$i; d=$(res nx$i); since $n0 > $d/events.txt
  ar=$(cnt 'LAZY armed by' $d/dylib.log); lt=$(cnt 'latched' $d/dylib.log); mk=$(markers | grep -c .); ps=$(cnt '\| PASS' $d/app.log)
  tr0=$(cnt ' activate .*traced=0' $d/events.txt); ld=$(cnt 'last debug run ended inside photo op' $d/dylib.log)
  if [[ $ar == 1 && $lt == 0 && $mk == 0 && $ps == 14 && $tr0 == 1 ]]; then ok next-$i "armed (activate traced=0), 14/14, no latch, 0 markers; 'last debug run ended inside photo op': $ld"
  else bad next-$i "armed=$ar latched=$lt markers=$mk pass=$ps traced=0:$tr0 (see $d)"; fi
done
for i in {1..$N}; do
  # --- control: the same hang launch with no debugger counts ---
  n0=$(nl $EV); run c$i SIMCAM_WD_TEST=hang; d=$(res c$i); since $n0 > $d/events.txt
  h1=$(cnt ' hang photo .* n=1' $d/events.txt); h2=$(cnt ' hang photo .* n=2' $d/events.txt); uh=$(cnt 'unknown-hang' $d/events.txt)
  la=$(cnt 'photo latched after 2 hangs' $d/dylib.log)
  if [[ $h1 == 1 && $h2 == 1 && $uh == 0 && $la == 1 ]]; then ok control-$i "hang photo n=1 and n=2 counted, 0 unknown-hang, 'photo latched after 2 hangs' in-process"
  else bad control-$i "hang n=1:$h1 n=2:$h2 unknown-hang=$uh in-process-latch=$la (see $d)"; fi
  n0=$(nl $EV); run cn$i; d=$(res cn$i); since $n0 > $d/events.txt
  ml=$(markers); sl=$(cnt 'sentinel: photo latched' $d/dylib.log); al=$(cnt ' activate .*latched=1' $d/events.txt)
  if [[ $sl == 1 && $al == 1 && $(print -r -- $ml | grep -c "latched-$BUILD") == 1 ]]; then ok control-next-$i "next launch latched: 'sentinel: photo latched', activate latched=1, marker ${ml##*/}"
  else bad control-next-$i "sentinel-latched=$sl activate-latched=$al markers='$ml' (see $d)"; fi
  print -r -- "$ml" > $d/markers.txt
  clear_engine cc$i; mk=$(markers | grep -c .)
  (( mk == 0 )) && ok clear-$i "SIMCAM_SENTINEL=clear launch removed the marker" || bad clear-$i "$mk markers left after the clear launch"
done
[[ -e $HOME/.lldbinit-Xcode ]] && bad lldbinit "~/.lldbinit-Xcode exists" || echo "no ~/.lldbinit-Xcode"
(( fails )) && { echo "$fails step(s) failed"; exit 1; } || echo "all steps passed"
