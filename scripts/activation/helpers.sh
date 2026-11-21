# SPDX-License-Identifier: Apache-2.0
# activation/helpers.sh — shared helpers: verdicts, launches, log counts, sentinel state, crash reports, plain-orient runs.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
ok() { VERDICT=PASS; EVID="$*"; }
bad() { VERDICT=FAIL; EVID="$*"; }
unsure() { VERDICT=UNSURE; EVID="$*"; }
res() { print -r -- "$SIMCAM_RESULTS/$1-$RT-boot-$TAG-$2"; }   # result dir of run.sh <app> … <tag>-<n>
run() { local app=$1 n=$2; shift 2; XENV="${(F)@}"$'\n' SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh $app $RT boot $TAG-$n >/dev/null 2>&1; }
ev() { print -r -- "$STATE/$1/events.log"; }
tmpdir() { local c=$(xcrun simctl get_app_container $U $1 data 2>/dev/null); [[ $c == $HOME/* ]] && print -r -- "$c/tmp/simcam-state/$1" || print -r -- "/nonexistent-container/$1"; }
nl() { [[ -f $1 ]] && wc -l < $1 | tr -d ' ' || echo 0; }
since() { [[ -f $1 ]] && tail -n +$(( $2 + 1 )) $1 || true; }   # lines after the first $2
maxact() { [[ -f $1 ]] && awk 'NF>=5 && $2>m {m=$2} END {print m+0}' $1 || echo 0; }
fc() { grep -oE 'firstcall\.ms=[0-9]+' $1/app.log 2>/dev/null | head -1 | cut -d= -f2; }
cnt() { local n; n=$(grep -cE "$1" $2 2>/dev/null); print -r -- ${n:-0}; }   # matching lines in a file (0 if missing)
pass_n() { cnt '\| PASS' $1/app.log; }
in_range() { [[ -n $1 ]] && (( $1 >= $2 && $1 <= $3 )); }
nf() { print $#; }   # nf <paths with (N) globs>: how many exist
mtime() { stat -f %m $1 2>/dev/null || echo 0; }
setmtime() { python3 -I -c 'import os,sys; t=int(sys.argv[2]); os.utime(sys.argv[1], (t, t))' $1 $2; }
clear_sentinel() {   # host-side clear of one bundle's sentinel state (see the header)
  local b=$1 d t
  for d in $STATE/$b "$(tmpdir $b)"; do
    [[ -d $d ]] || continue
    print -r -- "$(date +%s) 0 clear - $BUILD host-clear" >> $d/events.log
    for t in $d/latched-*(N); do rm -f $t; done
    rm -f ${d:h}/preflight-fail-$BUILD
  done
  rm -f $STATE/preflight-fail-$BUILD
}
# plant_marker <path> <content> <until epoch> (a marker the engine would have written: content, mtime = until)
plant_marker() { mkdir -p ${1:h}; print -r -- "$2" > $1; setmtime $1 $3; }
mstock() { cnt 'simcam: CoreMotion is stock this launch' $1/dylib.log; }   # the one-shot stock line (record queue -> per-pid log)
mstand() { [[ $(cnt "PASS order.$2.(available|rate|timestamps)" $1/app.log) == 3 &&
  $(cnt 'motion: hooked 16' $1/app.log) == 1 && $(mstock $1) == 0 ]]; }   # the stand-in answered
mstockok() { [[ $(cnt "PASS order.$2.stock.(available|updates)" $1/app.log) == 2 &&
  $(cnt 'motion: hooked' $1/app.log) == 0 && $(mstock $1) == 1 ]]; }   # stock, one line
plant() { print -r -- "$2" >> "$(ev $1)"; }   # plant <bundle> <line>
# app_lines <regex> <run dir> <width>: the app's log lines matching <regex> (grep -E) without their "M5APP| " prefix,
# joined with spaces and cut to <width> characters (a FAIL line's evidence)
app_lines() { grep -E "$1" $2/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-$3; }
# crash reports by pid (see the header). fault_pid <app.log> [regex]: the pid of the launch's fault line (regex after
# "[simcam <pid>] "), or of its first [simcam <pid>] line with no regex. crash_reports <proc> <wait s> <pid> ...: reports
# captured since CASE_START (set by the driver before each case run; ipspid.py prints pid<TAB>path); sets CR (how
# many of the pids have a report), CR_NO (the pids without one), CR_F (pid -> report file, as "pid=file ..."); waits up to
# <wait s> for missing ones. report_of <pid>: that pid's report file (after crash_reports).
fault_pid() { grep -m1 -oE "\[simcam [0-9]+\] ${2:-}" $1 2>/dev/null | grep -m1 -oE '^\[simcam [0-9]+' | grep -oE '[0-9]+'; }
crash_reports() {
  local p=$1 w=$2 i out k; shift 2; CR=0; CR_NO=; CR_F=
  local -a pids; pids=(${@:#}); (( ${#pids} )) || return 0
  for i in {0..$w}; do
    out=$(python3 -I $ROOT/tools/ipspid.py $p ${CASE_START:-0} $pids)
    (( $(print -r -- "$out" | awk -F'\t' 'NF {print $1}' | sort -u | grep -c .) >= ${#pids} || i == w )) && break
    sleep 1
  done
  for k in $pids; do
    local f=$(print -r -- "$out" | awk -F'\t' -v k=$k '$1 == k {print $2; exit}')
    if [[ -n $f ]]; then CR=$((CR + 1)); CR_F="$CR_F $k=$f"; else CR_NO="$CR_NO $k"; fi
  done
}
report_of() { local x; for x in ${=CR_F}; do [[ ${x%%=*} == $1 ]] && { print -r -- ${x#*=}; return; }; done; }
# verdict_reports <engine ok 0|1> <want> <ok text> <bad text>: PASS if the engine side passed and all <want> pids have a
# report; UNSURE if it passed and some have none; FAIL otherwise
verdict_reports() {
  if (( $1 )) && (( CR == $2 )); then ok "$3"
  elif (( $1 )) && (( CR < $2 )) && [[ -n $CR_NO ]]
  then unsure "no crash report written for pid(s)$CR_NO (DiagnosticReports + Retired, waited 30 s; $CR of $2 found: host report cap?); engine side passed: $3"
  else bad "$4"; fi
}
ips_fault() {   # ips_fault <file>: "<exception type> queue=<faulting thread's queue> top=<image of the top frame>"
  python3 -I - $1 <<'PY'
import json, sys
raw = open(sys.argv[1], errors='replace').read().split('\n', 1); b = json.loads(raw[1])
th = b.get('threads') or []; fi = b.get('faultingThread', 0); t = th[fi] if 0 <= fi < len(th) else {}
imgs = b.get('usedImages') or []; fr = (t.get('frames') or [{}])[0]; ii = fr.get('imageIndex')
print((b.get('exception') or {}).get('type'), 'queue=%s' % t.get('queue'), 'top=%s' % (imgs[ii].get('name') if isinstance(ii, int) and ii < len(imgs) else '?'))
PY
}
bg_run() {   # bg_run <app> <n> K=V ...: run.sh in the background; sets BGPID, BGDIR
  local app=$1 n=$2; shift 2
  ( XENV="${(F)@}"$'\n' SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh $app $RT boot $TAG-$n >/dev/null 2>&1 ) &
  BGPID=$!; BGDIR=$(res $app $n)
}
# (common.sh's wait_for_line takes a basic regex, a count of 0.1 s polls and the launch's pid instead)
wait_for() {   # wait_for <file> <extended regex> <seconds>
  local i; for i in {1..$(( $3 * 20 ))}; do grep -qE "$2" $1 2>/dev/null && return 0; sleep 0.05; done; return 1
}
app_pid() { grep -oE '\[simcam [0-9]+\]' $1 2>/dev/null | head -1 | grep -oE '[0-9]+'; }
# 17.5 cases: the container-tmp fallback is forced by a regular file where the app's state directory would be (mkdir -p
# "succeeds" on it, events.log can't open under it). A per-launch SIMCAM_STATE_DIR doesn't work in boot mode: the boot
# environment's value wins.
block_state() { local d=$STATE/$BP; [[ -d $d ]] && mv $d $d.w4-blocked; [[ -e $d ]] || print 'W4 activation.sh: blocks this state dir' > $d; }
unblock_state() { local d=$STATE/$BP; [[ -f $d ]] && rm -f $d; [[ -d $d.w4-blocked && ! -e $d ]] && mv $d.w4-blocked $d; return 0; }
cfa() { grep -m1 -E "$1" $2 2>/dev/null | awk '{print $1}'; }   # the CFAbsoluteTime stamp of the first matching dylib.log line
p12() {   # p12 <mode> <orient> <tag> [K=V ...]: plain-orient in boot mode; sets PD
  local m=$1 o=$2 t=$3; shift 3
  # MAXW: p12.sh's 0.2 s polls for the app's DONE, 150 of them (30 s)
  NOINJ=1 MAXW=150 $ROOT/scripts/p12.sh $RT $m back $o $TAG-$t "$@" >/dev/null 2>&1
  PD=$SIMCAM_RESULTS/p12-$m-$RT-back-$o-$TAG-$t
  # p12.sh copies the newest engine log; take this launch's own one (its pid) instead
  local pid=$(app_pid $PD/app.log) bl=$(cat $ROOT/build/bootlogdir-$RT.txt)
  [[ -n $pid && -f $bl/dylib-$pid.log ]] && cp $bl/dylib-$pid.log $PD/dylib.log
}
angles() { { grep -oE 'previewAngle=[0-9.]+ captureAngle=[0-9.]+' $1/app.log | head -1; grep -oE '(photo|movie) connection angle=[0-9.]+' $1/app.log; } | tr '\n' ' '; }   # one line
