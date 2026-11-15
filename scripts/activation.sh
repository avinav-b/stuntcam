#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# activation.sh <rt> [case ...] — bounded lazy activation and CoreMotion's load-time decision
#   (design §4.1, "Activation"), on a sim armed by
#   scripts/boot.sh <rt> stub. Prints "PASS <case> — evidence" / "FAIL <case> — evidence" (one line per case, also appended
#   to results/activation-<rt>-<tag>.txt) and exits 1 if any case failed. On 17.5: act-slow-ok act-slow-timeout act-fail + the -175 cases.
#   A timing case that fails is run once more before it counts (another agent's load can starve the machine); both
#   verdicts and the load average are printed.
#   THREADS_N (default 100): launches of the `threads` case. MOTION_N (default 10): launches of `motion-only`.
# Every counted case starts and ends with a cleared sentinel. The clear is done host-side (a clear line appended to the app's
# events.log and its latched-* markers removed, in the state dir and the container-tmp fallback): the same state a
# SIMCAM_SENTINEL=clear launch leaves, without its ~3-15 s launch.
#   cases (default: all that apply to <rt>):
#     act-slow-ok act-slow-timeout act-latch act-latch-phase act-decay act-marker-multi act-swapback act-sentinel-off
#     act-cli-clear act-markers-175 act-trim-race act-swap-slow act-start-slow act-control-slow act-override act-pause act-threads-pause
#     act-fail act-timeout-throw act-swapthrow act-crash-prepare act-crash-fig act-handler-restore act-fake-sdk act-armed-crash act-never
#     act-late act-175-record act-got act-main act-reenter threads threads-slow orient-nomotion orient-sameturn orient-bg orient-bg-turnback
#     motion-only motion-threads none motion-gate camera-then-motion mutation-L56 mutation-L57 act-traced
#   CoreMotion at load: orient-nomotion-marker camera-then-motion-real motion-after-gate motion-latched motion-pf-expiry motion-crash
#     mutation-L58 mutation-L59 mutation-L60   (PRB=1: only these and the motion-related activation cases)
#   tracker faults, marker pruning: orient-crash mutation-L61 pf-prune motion-stock-threads (in PRB_CASES)
#   attitude stand-in, motion-key scope: motion-attitude motion-attitude-stock orient-crash-main mutation-L62
#     orient-crash-ensure mutation-L63 pf-prune-race mutation-L64 (in PRB_CASES)
#   app-answerable sends: app-send-crash mutation-L65 (in PRB_CASES)
#   prune vs a fresh marker: pf-prune-kept (in PRB_CASES)
#   op trace file, deferred-stop flush: trace-crash-start mutation-L69 trace-crash2 trace-repick trace-unwritable
#     mutation-L71 flush-race mutation-L73 (the E54 list; they read traces with cli/bin/stuntcam, build it first)
#   crash-handler race: mutation-L70 (M54_OLD_CRASH: trace-crash2's negative control), in the E54 list
#   late stop waiter: late-waiter mutation-L74 (a stop waiter starved past the flush bound; L74), in the E54 list
# Verdicts: PASS, FAIL, and UNSURE (not counted as a failure; the run's last line counts them). Crash
# reports: a case that wants the host's .ips for a crash it caused matches reports by pid, never by count or mtime. The pids come
# from the launch's own lines: the fault line "[simcam <pid>] M16_*: fault ..." where the case has one, else the launch's
# first "[simcam <pid>]" line. A report counts if its process name is the app's and its "pid" is one of those, in
# DiagnosticReports or Retired/ (tools/ipspid.py), captured at or after the case's start (CASE_START: a reused pid never
# matches an older report); a missing one is waited for up to 30 s (reports were seen 12 s late). If
# every engine-side check passes and a pid still has no report, the case is UNSURE ("no crash report written for pid ..."):
# macOS stops writing reports for a process after many crashes, so a missing report is host state, not an engine verdict
# (seen: whenever m5app crashes got no report, DiagnosticReports held exactly 25 m5app reports, the rest in Retired/;
# once orient-crash-ensure's second report was missing with only 5 there, cause unknown). A case that wants NO report
# (act-slow-timeout) counts this launch's pid only.
#   act-traced attaches lldb from the command line (SB API, scripts/lldb-attach.py; no GUI). Not here: act-tsan (a TSan app +
#   engine boot)
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh
RT=${1:?usage: $0 <rt> [case ...]}; shift
V=SIMCAM_UDID_$RT; U=${(P)V:-}; [[ -n $U ]] || { echo "$0: set $V" >&2; exit 2; }
typeset -a CASES PRB_CASES; CASES=("$@")
PRB_CASES=(orient-nomotion-marker camera-then-motion-real motion-after-gate motion-latched motion-pf-expiry motion-crash mutation-L58 mutation-L59 mutation-L60
  orient-crash mutation-L61 pf-prune motion-stock-threads
  motion-attitude motion-attitude-stock orient-crash-main mutation-L62 orient-crash-ensure mutation-L63 pf-prune-race mutation-L64
  app-send-crash mutation-L65 pf-prune-kept)
typeset -a E54_CASES; E54_CASES=(trace-crash-start mutation-L69 trace-crash2 mutation-L70 trace-repick trace-unwritable mutation-L71 flush-race mutation-L73 late-waiter mutation-L74)
if (( ! ${#CASES} )); then
  CASES=(act-slow-ok act-slow-timeout act-fail)
  [[ $RT != 175 ]] && CASES+=(act-latch act-cli-clear act-latch-phase act-decay act-marker-multi act-swapback act-sentinel-off act-trim-race act-swap-slow
    act-start-slow act-control-slow act-override act-pause act-threads-pause act-timeout-throw act-swapthrow act-crash-prepare act-crash-fig act-handler-restore
    act-fake-sdk act-armed-crash act-never act-late act-got act-main act-reenter threads threads-slow orient-nomotion orient-sameturn orient-bg orient-bg-turnback
    motion-only motion-threads none motion-gate camera-then-motion mutation-L56 mutation-L57 act-traced)
  [[ $RT == 175 ]] && CASES+=(act-markers-175 act-175-record)
  [[ $RT != 175 ]] && CASES+=($PRB_CASES $E54_CASES)
  [[ ${PRB:-0} == 1 ]] && CASES=(motion-only motion-threads none motion-gate camera-then-motion orient-nomotion $PRB_CASES)
fi
TAG=act-$(date +%H%M%S); OUTF=$SIMCAM_RESULTS/activation-$RT-$TAG.txt; : > $OUTF
STATE=$SIMCAM_HOME/sims/$U/state; BUILD=$(<$ROOT/build/current_dylib.txt); BUILD=${BUILD:h:t}
BP=dev.simcam.spike.m10plain BC=dev.simcam.spike.m10canary BO=dev.simcam.spike.m12plain BM=dev.simcam.e24.motionprobe
TIMING=(act-slow-ok act-slow-timeout act-swap-slow act-start-slow act-override act-pause act-threads-pause act-main act-reenter
  threads-slow motion-threads act-never act-late)
fails=0; unsures=0; VERDICT=; EVID=
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
mstand() { [[ $(cnt "PASS order.$2.(available|rate|timestamps)" $1/app.log) == 3 && $(cnt 'motion: hooked 16' $1/app.log) == 1 && $(mstock $1) == 0 ]]; }   # the stand-in answered
mstockok() { [[ $(cnt "PASS order.$2.stock.(available|updates)" $1/app.log) == 2 && $(cnt 'motion: hooked' $1/app.log) == 0 && $(mstock $1) == 1 ]]; }   # stock, one line
plant() { print -r -- "$2" >> "$(ev $1)"; }   # plant <bundle> <line>
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
  elif (( $1 )) && (( CR < $2 )) && [[ -n $CR_NO ]]; then unsure "no crash report written for pid(s)$CR_NO (DiagnosticReports + Retired, waited 30 s; $CR of $2 found: host report cap?); engine side passed: $3"
  else bad "$4"; fi
}
trap unblock_state EXIT
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
wait_for() {   # wait_for <file> <regex> <seconds>
  local i; for i in {1..$(( $3 * 20 ))}; do grep -qE "$2" $1 2>/dev/null && return 0; sleep 0.05; done; return 1
}
app_pid() { grep -oE '\[simcam [0-9]+\]' $1 2>/dev/null | head -1 | grep -oE '[0-9]+'; }

# ---------------------------------------------------------------- cases
c_act-slow-ok() {
  clear_sentinel $BP; run plain so M16_SLOW_ACTIVATE_MS=300; local d=$(res plain so) f
  f=$(fc $d)
  if in_range "$f" 300 450 && [[ $(pass_n $d) == 14 && $(cnt 'LAZY armed by' $d/dylib.log) == 1 ]]; then ok "firstcall ${f} ms, 14/14, one armed-by line"
  else bad "firstcall ${f:-?} ms, pass $(pass_n $d), armed-by lines $(cnt 'LAZY armed by' $d/dylib.log)"; fi
  clear_sentinel $BP
}
c_act-slow-timeout() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d f lc h cr pid; n0=$(nl $e)
  run plain st M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d=$(res plain st)
  f=$(fc $d); lc=$(grep -oE 'latercall\.ms=[0-9.]+' $d/app.log | cut -d= -f2)
  pid=$(fault_pid $d/app.log); crash_reports m5app 0 $pid; cr=$CR; [[ -n $pid ]] || cr=no-pid
  h=$(since $e $n0 | grep -c 'hang activate .* phase=preflight')
  if in_range "$f" 900 1100 && (( ${lc%.*} < 5 )) && [[ $(cnt DONE $d/app.log) == 1 && $(cnt 'finished after .* staying stock' $d/dylib.log) == 1 && $(cnt 'swap:' $d/dylib.log) == 0 && $h == 1 && $cr == 0 && $(pass_n $d) == 2 ]]
  then ok "first call ${f} ms -> stock, later call ${lc} ms, 1 hang activate phase=preflight, late prepare stayed stock, no swap, no crash report (pid $pid)"
  else bad "firstcall ${f:-?} later ${lc:-?} hang=$h finished=$(cnt 'finished after' $d/dylib.log) swap=$(cnt 'swap:' $d/dylib.log) crashes=$cr pass=$(pass_n $d)"; fi
  clear_sentinel $BP
}
latch_seq() {   # latch_seq <prefix> <slow spec>: L1, L2 slow; L3, L4 normal; L5 clear. Sets L3/L4/L5 dirs, H (hang lines), PH (phase= values)
  local p=$1 s=$2 e=$(ev $BP) n0; clear_sentinel $BP; n0=$(nl $e)
  run plain $p-1 M16_SLOW_ACTIVATE_MS=$s M5APP_EXPECT=stock
  run plain $p-2 M16_SLOW_ACTIVATE_MS=$s M5APP_EXPECT=stock
  run plain $p-3 ${L3EXTRA:-} M5APP_EXPECT=stock
  M3=$(mtime $STATE/$BP/latched-$BUILD); MK3=$(cat $STATE/$BP/latched-$BUILD 2>/dev/null)
  run plain $p-4 M5APP_EXPECT=stock
  H=$(since $e $n0 | grep -c 'hang activate'); PH=$(since $e $n0 | grep -oE 'phase=[a-z]+' | sort -u | tr '\n' ' ')
  run plain $p-5 SIMCAM_SENTINEL=clear
  L3=$(res plain $p-3); L4=$(res plain $p-4); L5=$(res plain $p-5)
}
c_act-latch() {
  run plain la-0 SIMCAM=0 M5APP_EXPECT=stock; local f0=$(fc "$(res plain la-0)")   # stock AVF's own first-call time (no engine)
  latch_seq la 1500
  local f3=$(fc $L3) f4=$(fc $L4) mk5=$(nf $STATE/$BP/latched-*(N))
  if [[ $H == 2 && $(cnt 'PREFLIGHT' $L3/app.log) == 0 && $(cnt 'latched after' $L3/dylib.log) == 1 && $M3 -gt 0 ]] && (( f3 < 50 )) &&
     [[ $(cnt 'TIMING gate entered' $L4/app.log) == 0 && $(cnt 'latched until' $L4/dylib.log) == 1 ]] && (( f4 < f0 + 5 )) && [[ $(pass_n $L5) == 14 && $mk5 == 0 ]]
  then ok "L1+L2 2 hangs ($PH); L3 latched in PREPARE 1 (no preflight, ${f3} ms, marker '$MK3'); L4 marker at load, no prepare, ${f4} ms (stock AVF alone: ${f0} ms), one 'latched until'; L5 clear 14/14, markers gone"
  else bad "hangs=$H L3: preflight=$(cnt PREFLIGHT $L3/app.log) latched=$(cnt 'latched after' $L3/dylib.log) marker=$M3 fc=$f3; L4: gate=$(cnt 'TIMING gate entered' $L4/app.log) until=$(cnt 'latched until' $L4/dylib.log) fc=$f4 (stock $f0); L5 pass=$(pass_n $L5) markers=$mk5"; fi
  clear_sentinel $BP
}
c_act-cli-clear() {   # the real CLI's clear unlatches (this build's marker lives only next to the events.log in use)
  local e=$(ev $BP) T=$(tmpdir $BP) cli=$ROOT/cli/bin/stuntcam n0 d3 d4 ms mt ls1 ls2
  clear_sentinel $BP; n0=$(nl $e)
  run plain cc-1 M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain cc-2 M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain cc-3 M5APP_EXPECT=stock; d3=$(res plain cc-3)   # latched in PREPARE 1: writes this build's marker
  ms=$(nf $STATE/$BP/latched-$BUILD(N)); mt=$(nf $T/latched-$BUILD(N))
  ls1=$($cli sentinel ls --udid $U --bundle $BP 2>&1 | grep -iE 'latch' | head -1)
  $cli sentinel clear --udid $U --bundle $BP > $SIMCAM_RESULTS/activation-$RT-$TAG-cliclear.txt 2>&1; local rc=$?
  ls2=$($cli sentinel ls --udid $U --bundle $BP 2>&1 | grep -ciE 'latched')
  run plain cc-4; d4=$(res plain cc-4)
  if [[ $(since $e $n0 | grep -c ' hang activate') == 2 && $(cnt 'latched after' $d3/dylib.log) == 1 && $ms == 1 && $mt == 0 && -n $ls1 && $rc == 0 && $ls2 == 0 &&
        $(cnt 'latched until' $d4/dylib.log) == 0 && $(cnt 'LAZY armed by' $d4/dylib.log) == 1 && $(pass_n $d4) == 14 ]]
  then ok "2 hangs; L3 latched, marker in the state dir only (container tmp: none); ls before: '${ls1:0:90}'; cli/bin/stuntcam sentinel clear (rc 0); ls after: not latched; next launch armed 14/14"
  else bad "hangs=$(since $e $n0 | grep -c ' hang activate') latched=$(cnt 'latched after' $d3/dylib.log) marker state=$ms tmp=$mt ls1='${ls1:0:80}' rc=$rc ls2=$ls2; next: until=$(cnt 'latched until' $d4/dylib.log) armed=$(cnt 'LAZY armed by' $d4/dylib.log) pass=$(pass_n $d4)"; fi
  clear_sentinel $BP
}
c_act-latch-phase() {
  # @sentinel: L3 also sleeps after the parse, so it times out too, but its latch decision still writes the marker
  L3EXTRA="M16_SLOW_ACTIVATE_MS=1500@sentinel" latch_seq lps 1500@sentinel
  local hs=$H phs=$PH m3=$M3 g4=$(cnt 'TIMING gate entered' $L4/app.log) u4=$(cnt 'latched until' $L4/dylib.log)
  latch_seq lpr 1500@resolve
  local hr=$H phr=$PH pf3=$(cnt PREFLIGHT $L3/app.log) l3=$(cnt 'latched after' $L3/dylib.log) u4r=$(cnt 'latched until' $L4/dylib.log)
  if [[ $hs == 3 && $phs == "phase=sentinel " && $m3 -gt 0 && $g4 == 0 && $u4 == 1 && $hr == 2 && $phr == "phase=resolve " && $pf3 == 0 && $l3 == 1 && $u4r == 1 ]]
  then ok "@sentinel: 3 hangs (L3 too) phase=sentinel, L3's marker written, L4 no prepare; @resolve: 2 hangs phase=resolve, L3 latched without preflight, L4 marker-stock"
  else bad "@sentinel hangs=$hs '$phs' marker=$m3 L4gate=$g4 until=$u4; @resolve hangs=$hr '$phr' L3pf=$pf3 latched=$l3 until=$u4r"; fi
  clear_sentinel $BP
}
c_act-decay() {
  local e=$(ev $BP) mx now=$(date +%s) mk=$STATE/$BP/latched-$BUILD d1 d2 m2
  clear_sentinel $BP; mx=$(maxact $e)
  plant $BP "$(( now - 90000 )) $(( mx + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant $BP "$(( now - 90000 )) $(( mx + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  print activate > $mk; setmtime $mk $(( now - 3600 ))
  run plain dc1; d1=$(res plain dc1)
  local aged="pass=$(pass_n $d1) latched=$(cnt 'latched after' $d1/dylib.log) marker=$([[ -f $mk ]] && echo kept || echo deleted)"
  clear_sentinel $BP; now=$(date +%s); mx=$(maxact $e)
  plant $BP "$(( now - 3600 )) $(( mx + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant $BP "$(( now - 3600 )) $(( mx + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  print activate > $mk; setmtime $mk $(( now - 3600 ))
  run plain dc2 M5APP_EXPECT=stock; d2=$(res plain dc2); m2=$(mtime $mk)
  local hrs=$(( (m2 - now) / 360 ))   # tenths of an hour
  if [[ $aged == "pass=14 latched=0 marker=deleted" && $(cnt 'latched after' $d2/dylib.log) == 1 ]] && (( hrs >= 225 && hrs <= 235 ))
  then ok "aged (25 h): $aged, armed; control (1 h): re-latched, marker until +$(( hrs / 10 )).$(( hrs % 10 )) h"
  else bad "aged: $aged; control: latched=$(cnt 'latched after' $d2/dylib.log) marker until +${hrs} tenths h"; fi
  clear_sentinel $BP
}
c_act-marker-multi() {
  local e=$(ev $BP) mx now=$(date +%s) mk=$STATE/$BP/latched-$BUILD d1 d2 m c
  clear_sentinel $BP; mx=$(maxact $e)
  plant $BP "$(( now - 7200 )) $(( mx + 1 )) crash photo $BUILD signal=11"; plant $BP "$(( now - 7200 )) $(( mx + 2 )) crash photo $BUILD signal=11"
  plant $BP "$now $(( mx + 3 )) crash session $BUILD signal=11"; plant $BP "$now $(( mx + 4 )) crash session $BUILD signal=11"
  run plain mm1 M5APP_EXPECT=stock; c=$(cat $mk 2>/dev/null); m=$(mtime $mk)
  run plain mm2 M5APP_EXPECT=stock; d2=$(res plain mm2)
  local line=$(grep -m1 'latched until' $d2/dylib.log | cut -c1-200)
  if [[ $c == *session* && $c == *photo* && $line == *session* && $line == *photo* ]] && (( m - now >= 86400 - 120 && m - now <= 86400 + 120 ))
  then ok "marker '$c', until = session expiry (+$(( (m - now) / 3600 )) h, not photo's +22 h); next launch: '${line#*simcam: }'"
  else bad "marker '$c' until +$(( m - now )) s; line '$line'"; fi
  clear_sentinel $BP
}
c_act-swapback() {
  local e=$(ev $BP) mx now A_LDR=$(cat $ROOT/build/current_loader.txt) B_LDR BB d2 d3 gone
  ( cd $ROOT && EXTRA=-O2 ./build.sh >/dev/null 2>&1 ); B_LDR=$(<$ROOT/build/current_loader.txt); BB=${B_LDR:h:t}
  ( cd $ROOT && ./build.sh >/dev/null 2>&1 )
  [[ "$(<$ROOT/build/current_loader.txt)" == $A_LDR && $BB != $BUILD ]] || { bad "could not make a second build (A=$BUILD B=$BB)"; return; }
  clear_sentinel $BP; now=$(date +%s); mx=$(maxact $e)
  plant $BP "$now $(( mx + 1 )) hang activate $BUILD ms=1000 phase=preflight"; plant $BP "$now $(( mx + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  run plain sb1 M5APP_EXPECT=stock
  local mA=$([[ -f $STATE/$BP/latched-$BUILD ]] && echo yes || echo no)
  mx=$(maxact $e); for i in {1..20}; do plant $BP "$now $(( mx + i )) activate - $BB traced=0"; done
  $ROOT/scripts/swap.sh $U $B_LDR >/dev/null; run plain sb2; d2=$(res plain sb2)
  gone=$([[ -f $STATE/$BP/latched-$BUILD ]] && echo no || echo yes)
  $ROOT/scripts/swap.sh $U $A_LDR >/dev/null; run plain sb3; d3=$(res plain sb3)
  if [[ $mA == yes && $gone == yes && $(cnt 'removed stale marker' $d2/dylib.log) -ge 1 && $(pass_n $d3) == 14 ]]
  then ok "A ($BUILD) latched with marker; 20 activations of B ($BB); B's prepare removed latched-A; A then armed 14/14"
  else bad "markerA=$mA removedByB=$gone ('$(grep -m1 'stale marker' $d2/dylib.log | cut -c1-80)') A-after pass=$(pass_n $d3)"; fi
  clear_sentinel $BP
}
c_act-sentinel-off() {
  local mk=$STATE/$BP/latched-$BUILD now=$(date +%s) d m0 m1
  clear_sentinel $BP; mkdir -p ${mk:h}; print activate > $mk; setmtime $mk $(( now + 3600 )); m0=$(mtime $mk)
  run plain sof SIMCAM_SENTINEL=0; d=$(res plain sof); m1=$(mtime $mk)
  if [[ $(pass_n $d) == 14 && $(cnt 'LAZY armed by' $d/dylib.log) == 1 && $m0 == $m1 ]]; then ok "valid marker ignored: armed 14/14; marker untouched (no marker written)"
  else bad "pass=$(pass_n $d) armed=$(cnt 'LAZY armed by' $d/dylib.log) marker mtime $m0 -> $m1"; fi
  rm -f $mk; clear_sentinel $BP
}
# 17.5 cases: the container-tmp fallback is forced by a regular file where the app's state directory would be (mkdir -p
# "succeeds" on it, events.log can't open under it). A per-launch SIMCAM_STATE_DIR doesn't work in boot mode: the boot
# environment's value wins.
block_state() { local d=$STATE/$BP; [[ -d $d ]] && mv $d $d.w4-blocked; [[ -e $d ]] || print 'W4 activation.sh: blocks this state dir' > $d; }
unblock_state() { local d=$STATE/$BP; [[ -f $d ]] && rm -f $d; [[ -d $d.w4-blocked && ! -e $d ]] && mv $d.w4-blocked $d; return 0; }
c_act-markers-175() {
  local td=$(tmpdir $BP) now=$(date +%s) d1 d2
  block_state; mkdir -p $td; print activate > $td/latched-$BUILD; setmtime $td/latched-$BUILD $(( now + 3600 ))
  run plain mk0 M5APP_EXPECT=stock; local d0=$(res plain mk0)
  run plain mk1 SIMCAM_SENTINEL=clear; d1=$(res plain mk1)
  local gone=$([[ -f $td/latched-$BUILD ]] && echo no || echo yes)
  run plain mk2; d2=$(res plain mk2)
  unblock_state
  if [[ $(cnt 'latched until' $d0/dylib.log) == 1 && $gone == yes && $(pass_n $d2) == 14 && $(cnt 'events=.*/tmp/simcam-state/' $d2/dylib.log) == 1 ]]
  then ok "a container-tmp marker is read at load (stock, 'latched until'); clear removed it; the next launch armed 14/14 with events in the tmp fallback"
  else bad "load stock=$(cnt 'latched until' $d0/dylib.log) marker gone=$gone; normal pass=$(pass_n $d2) tmpEvents=$(cnt 'events=.*/tmp/simcam-state/' $d2/dylib.log)"; fi
  rm -rf $td   # our own app's container-tmp sentinel state
}
c_act-trim-race() {
  local e=$(ev $BP) mx old=$(( $(date +%s) - 3 * 86400 )) d1 d2 n1 n2 h1 h2
  clear_sentinel $BP; mx=$(maxact $e)
  awk -v s=$(( mx + 1 )) -v t=$old -v b=$BUILD 'BEGIN { for (i = 0; i < 4100; i++) printf "%d %d exit - %s atexit\n", t, s + i, b }' >> $e
  local n0=$(nl $e) hl
  run plain tr1 M16_SLOW_ACTIVATE_MS=1500@trim M5APP_EXPECT=stock; d1=$(res plain tr1); n1=$(nl $e); hl=$(since $e $n0 | grep 'hang activate .*phase=trim'); h1=$(print -r -- "$hl" | grep -c .)
  run plain tr2; d2=$(res plain tr2); n2=$(nl $e); h2=$(grep -cF -- "$hl" $e)
  if [[ $(cnt 'trim skipped' $d1/dylib.log) == 1 && $h1 == 1 && $(cnt 'trimmed .* to the newest 2000' $d2/dylib.log) == 1 && $h2 == 1 ]] && (( n1 > 4000 && n2 < 2200 ))
  then ok "L1: rename skipped (file changed), hang activate phase=trim kept ($n1 lines); L2 trimmed to $n2 lines, the hang line still there"
  else bad "L1 skipped=$(cnt 'trim skipped' $d1/dylib.log) hang=$h1 lines=$n1; L2 trimmed=$(cnt 'trimmed' $d2/dylib.log) lines=$n2 hang=$h2"; fi
  clear_sentinel $BP
}
c_act-swap-slow() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d f; n0=$(nl $e)
  run plain sws M16_SLOW_SWAP_MS=1500; d=$(res plain sws); f=$(fc $d)
  if in_range "$f" 1450 2000 && [[ $(pass_n $d) == 14 && $(since $e $n0 | grep -c 'hang activate') == 0 && $(cnt 'COMMITTING overran' $d/dylib.log) == 1 ]]
  then ok "waiters returned after SWAP (${f} ms, not at 1000), armed 14/14, no hang, one COMMITTING-overran line"
  else bad "firstcall ${f:-?} pass=$(pass_n $d) hang=$(since $e $n0 | grep -c 'hang activate') overran=$(cnt 'COMMITTING overran' $d/dylib.log)"; fi
  clear_sentinel $BP
}
cfa() { grep -m1 -E "$1" $2 2>/dev/null | awk '{print $1}'; }   # the CFAbsoluteTime stamp of the first matching dylib.log line
c_act-start-slow() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d f tsrc tor tarm tff tact
  n0=$(nl $e)
  run plain sts M16_SLOW_START_MS=5000 SIMCAM_SOURCE=image:$SIMCAM_DEV/media/qr.png SIMCAM_ORIENT=landscapeLeft; d=$(res plain sts); f=$(fc $d)
  tsrc=$(cfa 'm12 source applied: image' $d/dylib.log); tor=$(cfa 'orientation -> 3 \(SIMCAM_ORIENT\)' $d/dylib.log); tarm=$(cfa 'LAZY armed by' $d/dylib.log)
  tff=$(cfa 'TIMING first preview frame' $d/dylib.log); tact=$(since $e $n0 | awk '$3=="activate"{print $1; exit}')
  local ob=$(since $e $n0 | awk -v a="$tact" '$3=="op-begin" && $4=="session" && $1-a<=1' | wc -l | tr -d ' ')
  if (( f < 100 )) && [[ -n $tsrc && -n $tor && -n $tarm && -n $tff && -n $tact ]] && python3 -I -c "import sys; s,o,a,ff,act=map(float,sys.argv[1:]); sys.exit(0 if s<=ff+0.05 and o<a-3 and a-ff>3 and act-978307200<a-3 else 1)" $tsrc $tor $tarm $tff $tact && (( ob >= 1 ))
  then ok "firstcall ${f} ms; still applied before the first frame, landscape forced at SWAP; activate + op-begin session at once; armed/flush line $(python3 -I -c "import sys;print('%.1f'%(float(sys.argv[1])-float(sys.argv[2])))" $tarm $tff) s after the first frame"
  else bad "fc=$f src=$tsrc orient=$tor armed=$tarm firstframe=$tff activate=$tact opbegin=$ob"; fi
  clear_sentinel $BP
}
c_act-control-slow() {
  clear_sentinel $BP; local C=$SIMCAM_RESULTS/activation-ctl-$TAG.txt img=$SIMCAM_DEV/media/qr.png
  printf 'source=image:%s\norientation=landscapeLeft\nevent=probeA@1\n' $img > $C
  bg_run plain cs SIMCAM_CONTROL=$C M16_SLOW_START_MS=5000
  wait_for $BGDIR/app.log 'START sleeps' 20 && { sleep 1; printf 'source=image:%s\norientation=landscapeLeft\nevent=probeB@2\n' $img > $C.tmp; mv $C.tmp $C; }
  wait $BGPID; local d=$BGDIR
  if [[ $(cnt 'event probeA@1 present at start, not replayed' $d/dylib.log) == 1 && $(cnt 'unknown event probeA' $d/dylib.log) == 0 && $(cnt 'unknown event probeB' $d/dylib.log) == 1 &&
        $(cnt 'source applied: image' $d/dylib.log) -ge 1 && $(cnt 'orientation 3 forced at activation \(control file\)' $d/dylib.log) == 1 ]]
  then ok "control file's source/orientation applied at SWAP; event at the read not replayed; event written during START's sleep replayed once"
  else bad "atStart=$(cnt 'probeA@1 present' $d/dylib.log) replayA=$(cnt 'unknown event probeA' $d/dylib.log) replayB=$(cnt 'unknown event probeB' $d/dylib.log) src=$(cnt 'source applied: image' $d/dylib.log) orient=$(cnt 'forced at activation' $d/dylib.log)"; fi
  rm -f $C; clear_sentinel $BP
}
c_act-override() {
  clear_sentinel $BP; local d f bad1= v i=0
  run plain ov0 SIMCAM_ACTIVATE_TIMEOUT_MS=200 M16_SLOW_ACTIVATE_MS=300 M5APP_EXPECT=stock; d=$(res plain ov0); f=$(fc $d)
  local r200="firstcall ${f:-?} ms, hang ms=$(grep -oE 'ms=[0-9]+ phase' "$(ev $BP)" | tail -1)"
  in_range "$f" 150 260 || bad1="200: $r200"
  clear_sentinel $BP
  for v in 0 -5 abc 999999; do
    i=$((i + 1)); run plain ov$i SIMCAM_ACTIVATE_TIMEOUT_MS=$v; d=$(res plain ov$i)
    [[ $(cnt "SIMCAM_ACTIVATE_TIMEOUT_MS=$v is not an integer" $d/app.log) == 1 && $(cnt 'budget 1000 ms' $d/dylib.log) == 1 && $(pass_n $d) == 14 ]] || bad1="$bad1 [$v: warn=$(cnt 'is not an integer' $d/app.log) budget1000=$(cnt 'budget 1000 ms' $d/dylib.log) pass=$(pass_n $d)]"
  done
  [[ -z $bad1 ]] && ok "200: $r200; 0, -5, abc, 999999: one stderr line at load each, budget 1000 ms, armed" || bad "$bad1"
  clear_sentinel $BP
}
pause_case() {   # pause_case <tag> [K=V ...]: =800, kill -STOP 5 s during prepare
  local n=$1; shift; clear_sentinel $BP
  bg_run plain $n M16_SLOW_ACTIVATE_MS=800 "$@"
  local pid=
  if wait_for $BGDIR/app.log 'TIMING gate entered' 20; then pid=$(app_pid $BGDIR/app.log); sleep 0.2; kill -STOP $pid; sleep 5; kill -CONT $pid; fi
  wait $BGPID; PD=$BGDIR; PPID_=$pid
}
c_act-pause() {
  pause_case pz; local d=$PD f=$(fc $PD)
  local ext=$(grep -oE 'deadline \+[0-9.]+ s' $d/dylib.log | tr '\n' ' ')
  if [[ -n $PPID_ && $(pass_n $d) == 14 && $(cnt 'deadline \+' $d/dylib.log) == 1 ]] && in_range "$f" 5500 6500; then ok "armed 14/14; one '$ext'; first call ${f} ms"
  else bad "pid=$PPID_ pass=$(pass_n $d) ext='$ext' firstcall=${f:-?}"; fi
  clear_sentinel $BP
}
c_act-threads-pause() {
  pause_case tpz M5APP_ORDER=threads; local d=$PD
  local ext=$(grep -oE 'deadline \+[0-9.]+ s' $d/dylib.log | tr '\n' ' ')
  if [[ -n $PPID_ && $(cnt 'PASS order.threads ' $d/app.log) == 1 && $(cnt 'deadline \+' $d/dylib.log) == 1 && $(pass_n $d) == 15 ]]; then ok "armed, all 3 threads saw cameras ($(grep -oE 'results=\[[^]]*\]' $d/app.log)), exactly one '$ext'"
  else bad "pid=$PPID_ threads=$(cnt 'PASS order.threads ' $d/app.log) ext='$ext' pass=$(pass_n $d)"; fi
  clear_sentinel $BP
}
c_act-fail() {
  local e=$(ev $BP) n0 d f out= sum= want
  for f in preflight throw missing pfbreak; do
    clear_sentinel $BP; n0=$(nl $e)
    if [[ $f == pfbreak ]]; then run plain fl-$f SIMCAM_PF_BREAK=1 M5APP_EXPECT=stock; want='DISABLED - preflight failed'
    else run plain fl-$f M16_ACTIVATE_FAIL=$f M5APP_EXPECT=stock; case $f in preflight) want='DISABLED - preflight failed';; throw) want='activation threw M16ActivateThrow';; missing) want='activation failed: .*not found';; esac; fi
    d=$(res plain fl-$f)
    local raw=$(grep -cE '^simcam: ' $d/app.log) w=$(grep -cE "^simcam: .*$want" $d/app.log)
    local mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N) "$(tmpdir $BP)"/latched-*(N)) ca=$(since $e $n0 | grep -c 'crash activate .*exception=M16ActivateThrow')
    sum="$sum $f:pass=$(pass_n $d),stderr=$raw/$w,markers=$mk,crash=$ca"
    [[ $(pass_n $d) == 2 && $(cnt DONE $d/app.log) == 1 && $raw == 1 && $w == 1 && $mk == 0 ]] || out="$out $f"
    [[ $f == throw && $ca != 1 ]] && out="$out throw-record"
    [[ $f != throw && $ca != 0 ]] && out="$out $f-record"
  done
  [[ -z $out ]] && ok "stock + one stderr line + DONE each, no marker; throw -> crash activate counted ($sum)" || bad "failed:$out ($sum)"
  clear_sentinel $BP
}
c_act-timeout-throw() {   # a timeout, then prepare throws: exactly one counted strike, so the next launch arms
  clear_sentinel $BP; local e=$(ev $BP) n0 d1 d2 c; n0=$(nl $e)
  run plain tt1 M16_SLOW_ACTIVATE_MS=1500 M16_ACTIVATE_FAIL=throw M5APP_EXPECT=stock; d1=$(res plain tt1); sleep 1
  c=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ (hang|crash) ')
  local lines=$(since $e $n0 | awk '$3=="hang"||$3=="crash"{print $3, $4}' | tr '\n' ',')
  run plain tt2; d2=$(res plain tt2)
  if [[ $c == 1 && $lines == "hang activate," && $(cnt 'threw M16ActivateThrow after the timeout; not recorded' $d1/dylib.log) == 1 && $(grep -cE '^simcam: ' $d1/app.log) == 1 &&
        $(cnt 'LAZY armed by' $d2/dylib.log) == 1 && $(pass_n $d2) == 14 ]]
  then ok "timeout at $(fc $d1) ms, then prepare threw: 1 counted line ($lines one stderr line), 'not recorded' logged; next launch armed 14/14"
  else bad "counted=$c ($lines) notrec=$(cnt 'after the timeout; not recorded' $d1/dylib.log) stderr=$(grep -cE '^simcam: ' $d1/app.log); next: armed=$(cnt 'LAZY armed by' $d2/dylib.log) pass=$(pass_n $d2)"; fi
  clear_sentinel $BP
}
c_act-swapthrow() {   # swapthrow throws in SWAP and again at START's end: one counted strike per launch all the same
  clear_sentinel $BP; local e=$(ev $BP) n0 c1 c2 l1 l2 d1 d2 d3; n0=$(nl $e)
  run plain swt1 M16_ACTIVATE_FAIL=swapthrow M5APP_EXPECT=stock; c1=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ (hang|crash) '); l1=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ crash activate [^ ]+ exception=M16SwapThrow'); n0=$(nl $e); d1=$(res plain swt1)
  run plain swt2 M16_ACTIVATE_FAIL=swapthrow M5APP_EXPECT=stock; c2=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ (hang|crash) '); l2=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ crash activate [^ ]+ exception=M16SwapThrow'); d2=$(res plain swt2)
  run plain swt3 M5APP_EXPECT=stock; d3=$(res plain swt3)
  local nr1=$(cnt "START threw M16StartThrow; not recorded" $d1/dylib.log) nr2=$(cnt "START threw M16StartThrow; not recorded" $d2/dylib.log)
  if [[ $c1 == 1 && $l1 == 1 && $c2 == 1 && $l2 == 1 && $nr1 == 1 && $nr2 == 1 && $(cnt 'SWAP threw M16SwapThrow: armed but degraded' $d1/dylib.log) == 1 &&
        $(cnt 'latched after' $d3/dylib.log) == 1 && $(cnt PREFLIGHT $d3/app.log) == 0 ]]
  then ok "1st/2nd: ARMED-degraded, SWAP and START both threw, exactly one counted line each (crash activate exception=M16SwapThrow; START's 'not recorded'); 3rd latched in PREPARE 1"
  else bad "counted $c1/$c2 (swap lines $l1/$l2) start-not-recorded $nr1/$nr2 degraded=$(cnt 'armed but degraded' $d1/dylib.log) 3rd latched=$(cnt 'latched after' $d3/dylib.log)"; fi
  clear_sentinel $BP
}
crash_seq() {   # crash_seq <tag> <fail>: two crashing launches, then a normal one. Sets C1 C2 F1 F2 (report summaries, "no-report"
  # if the pid has none) D3, and CR/CR_NO for both crashing pids (the launch's first [simcam <pid>] line: no fault line here)
  local t=$1 f=$2 e=$(ev $BP) n0 rp; local -a pids
  clear_sentinel $BP
  for i in 1 2; do
    n0=$(nl $e); run plain $t$i M16_ACTIVATE_FAIL=$f M5APP_EXPECT=stock
    eval "C$i=\$(since \$e \$n0 | grep -c 'crash activate')"; pids+=($(fault_pid "$(res plain $t$i)/app.log"))
  done
  crash_reports m5app 30 $pids
  for i in 1 2; do rp=$(report_of ${pids[$i]:-x}); eval "F$i=\"\$( [[ -n \$rp ]] && ips_fault \$rp || echo no-report )\""; done
  CS_PIDS="$pids"
  run plain ${t}3 M5APP_EXPECT=stock; D3=$(res plain ${t}3)
}
# the report-side checks of act-crash-prepare / act-crash-fig run only on the reports that exist (a missing one is UNSURE)
rep_ok() { [[ $1 == no-report ]] || [[ $1 == $~2 ]]; }
c_act-crash-prepare() {
  crash_seq cp crash
  local eng=0; [[ $C1 == 1 && $C2 == 1 && $(cnt 'latched after' $D3/dylib.log) == 1 ]] && rep_ok "$F1" '*EXC_BAD_ACCESS*queue=simcam.activate*' && rep_ok "$F2" '*queue=simcam.activate*' && eng=1
  verdict_reports $eng 2 "one crash activate per crash (pids $CS_PIDS); reports: $F1 / $F2; 3rd latched" "records $C1/$C2; reports '$F1' / '$F2' (pids $CS_PIDS); 3rd latched=$(cnt 'latched after' $D3/dylib.log)"
  clear_sentinel $BP
}
c_act-crash-fig() {
  crash_seq cf crashfig
  local eng=0; [[ $C1 == 1 && $C2 == 1 && $(cnt 'latched after' $D3/dylib.log) == 1 && $F1 != *simcam.activate* ]] && rep_ok "$F1" '*top=CoreMedia*' && eng=1
  verdict_reports $eng 2 "one crash activate per crash (pids $CS_PIDS; PC in CoreMedia on another thread: $F1); 3rd latched" "records $C1/$C2; reports '$F1' / '$F2' (pids $CS_PIDS); 3rd latched=$(cnt 'latched after' $D3/dylib.log)"
  clear_sentinel $BP
}
c_act-handler-restore() {
  local d1 d2 b1 a1 b2 a2 m2
  clear_sentinel $BP; run plain hr1 M16_ACTIVATE_FAIL=preflight M5APP_SIGCHECK=1 M5APP_EXPECT=stock; d1=$(res plain hr1)
  b1=$(grep -oE 'sigcheck before SIGSEGV handler=[^ ]+' $d1/app.log | cut -d= -f2); a1=$(grep -oE 'sigcheck SIGSEGV handler=[^ ]+' $d1/app.log | cut -d= -f2)
  # also read the handler while the never-finishing prepare runs (500 ms in): it must be ours (the minimal one),
  # or the restore checks would pass without it ever going in
  clear_sentinel $BP; run plain hr2 M16_SLOW_ACTIVATE_MS=600000 M5APP_SIGCHECK=1 M5APP_SIGCHECK_AT_MS=500 M5APP_EXPECT=stock; d2=$(res plain hr2)
  b2=$(grep -oE 'sigcheck before SIGSEGV handler=[^ ]+' $d2/app.log | cut -d= -f2); a2=$(grep -oE 'sigcheck SIGSEGV handler=[^ ]+' $d2/app.log | cut -d= -f2)
  m2=$(grep -oE 'sigcheck during SIGSEGV handler=[^ ]+' $d2/app.log | cut -d= -f2)
  if [[ -n $b1 && $b1 == $a1 && -n $b2 && $b2 == $a2 && $m2 == libsimcam.dylib* ]]; then ok "prepare failure: $b1 -> $a1; never-finishing prepare: $b2 -> during prepare $m2 -> after the waiter's restore $a2"
  else bad "preflight: '$b1' -> '$a1'; timeout: '$b2' -> during '$m2' -> '$a2'"; fi
  clear_sentinel $BP
}
c_act-fake-sdk() {
  # the fault comes at 1600 ms (not 3000): M5APP_EXPECT=stock exits ~1 s after the first call returns. It is a
  # related-image fault (CoreMedia, on a global queue: M16_FIG_FAULT_AT_MS), which the minimal handler would record
  # as `crash activate` if gActRunning were still set: the waiter clears it before anything else, so the chain through the
  # fake SDK to our minimal handler must record nothing. (An app-code fault, as before, is never recorded either way.)
  clear_sentinel $BP; local e=$(ev $BP) n0 d; n0=$(nl $e)
  run plain fk M16_SLOW_ACTIVATE_MS=1500 M5APP_FAKESDK_AT_MS=500 M5APP_SIGCHECK=1 M16_FIG_FAULT_AT_MS=1600 M5APP_EXPECT=stock; d=$(res plain fk); sleep 2
  local after=$(grep -oE 'sigcheck SIGSEGV handler=[^ ]+' $d/app.log | cut -d= -f2) rec=$(cnt 'FAKESDK record' $d/app.log) ours=$(since $e $n0 | grep -cE 'crash activate|crash-other')
  local pid=$(fault_pid $d/app.log 'M16_FIG_FAULT_AT_MS: CoreMedia fault now') rp fl= eng=0; crash_reports m5app 30 $pid; rp=$(report_of ${pid:-x}); [[ -n $rp ]] && fl=$(ips_fault $rp)
  [[ $after == m5app* && $rec == 1 && $ours == 0 && -n $pid && ( -z $rp || $fl == *top=CoreMedia* ) ]] && eng=1
  verdict_reports $eng 1 "after the timeout the SDK's handler is current ($after); the CoreMedia fault at 1600 ms (pid $pid, report: ${fl:-none}) gave one FAKESDK record, no crash activate/crash-other line" \
    "after=$after sdkRecords=$rec ourLines=$ours fault pid=${pid:-none} report='$fl'"
  clear_sentinel $BP
}
c_act-armed-crash() {
  clear_sentinel $BP; local e=$(ev $BP) n0; n0=$(nl $e)
  run plain ac SIMCAM_WD_TEST=crash; sleep 1
  local c=$(since $e $n0 | grep -c 'crash photo') a=$(since $e $n0 | grep -c 'crash activate')
  [[ $c == 1 && $a == 0 ]] && ok "exactly one crash photo line (full handler only), no crash activate" || bad "crash photo=$c crash activate=$a"
  clear_sentinel $BP
}
c_act-never() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d; n0=$(nl $e)
  run plain nv M16_SLOW_ACTIVATE_MS=600000 M5APP_EXPECT=stock; d=$(res plain nv); sleep 1
  local h=$(since $e $n0 | grep -c 'hang activate') tw=$(cfa 'TIMING first camera call' $d/dylib.log) tr=$(cfa 'hang activate recorded' $d/dylib.log)
  if [[ $h == 1 && $(cnt DONE $d/app.log) == 1 && -n $tw && -n $tr ]] && python3 -I -c "import sys; sys.exit(0 if abs(float(sys.argv[1])-float(sys.argv[2]))<1 else 1)" $tw $tr
  then ok "one hang activate line, recorded within 1 s of the timeout; app exited normally, no second line"
  else bad "hang=$h done=$(cnt DONE $d/app.log) timeout@$tw record@$tr"; fi
  clear_sentinel $BP
}
c_act-late() {
  clear_sentinel $BP; local e=$(ev $BP) n0 mx d; n0=$(nl $e); mx=$(maxact $e)
  # prepare ends 300 ms after the timeout (1050 left 50 ms, less than one 100 ms wait slice plus wake-up latency, for
  # the waiter's CAS to beat prepare's); still well inside the launch, so "finished after … staying stock" is logged
  run plain lt M16_SLOW_ACTIVATE_MS=1300 M5APP_EXPECT=stock; d=$(res plain lt)
  local h=$(since $e $n0 | grep -c 'hang activate') nums=$(since $e $n0 | awk '{print $2}' | sort -u | tr '\n' ' ')
  if [[ $h == 1 && $(cnt 'finished after .* staying stock' $d/dylib.log) == 1 && $(cnt 'swap:' $d/dylib.log) == 0 && $nums == "$(( mx + 1 )) " ]]
  then ok "one hang line, one 'finished after … staying stock', no swap; every line of this launch carries activation $(( mx + 1 ))"
  else bad "hang=$h finished=$(cnt 'finished after' $d/dylib.log) swap=$(cnt 'swap:' $d/dylib.log) activation numbers '$nums' (want $(( mx + 1 )))"; fi
  clear_sentinel $BP
}
c_act-175-record() {
  local td=$(tmpdir $BP) d3 d4; rm -rf $td; block_state
  run plain r1 M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain r2 M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain r3 M5APP_EXPECT=stock; d3=$(res plain r3)
  run plain r4 M5APP_EXPECT=stock; d4=$(res plain r4)
  unblock_state
  local h=$(grep -c 'hang activate' $td/events.log 2>/dev/null)
  if [[ $h == 2 && $(cnt 'latched after' $d3/dylib.log) == 1 && -f $td/latched-$BUILD && $(cnt 'latched until' $d4/dylib.log) == 1 && $(cnt 'TIMING gate entered' $d4/app.log) == 0 ]]
  then ok "both hang lines in the container-tmp events.log; 3rd latched (marker in tmp), 4th STOCK from the marker"
  else bad "tmp hangs=${h:-0} 3rd latched=$(cnt 'latched after' $d3/dylib.log) marker=$([[ -f $td/latched-$BUILD ]] && echo yes || echo no) 4th until=$(cnt 'latched until' $d4/dylib.log)"; fi
  rm -rf $td
}
c_act-got() {
  clear_sentinel $BC; local d1 d2
  run canary got1 M6_SESSHOOK=got; d1=$(res canary got1)
  local walk_after=$(awk '/LAZY armed by/{a=1; next} a && /got route:/' $d1/dylib.log | wc -l | tr -d ' ')
  # the timeout half runs in plain (M5APP_EXPECT=stock lives ~2 s): the canary quits at once with no camera, before the late
  # prepare's resolve step logs the slot
  clear_sentinel $BP; run plain got2 M6_SESSHOOK=got M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d2=$(res plain got2); sleep 1
  local slot=$(grep -m1 -oE 'got route: .*prepare: found, not yet patched' $d2/dylib.log | cut -c1-120)
  if [[ $(cat $d1/summary.txt) == *core=PASS* && $(cnt 'prepare: found' $d1/dylib.log) -ge 1 && $walk_after == 0 && $(cnt 'entries rebound' $d1/dylib.log) == 1 &&
        $(pass_n $d2) == 2 && -n $slot && $(cnt 'entries rebound' $d2/dylib.log) == 0 ]]
  then ok "normal: $(grep -oE 'got, [0-9]+ of [0-9]+ entries rebound' $d1/dylib.log), canary core PASS, no image walk after ARMED; timeout: stock, slot unchanged ($slot)"
  else bad "normal: $(cut -c1-60 $d1/summary.txt) found=$(cnt 'prepare: found' $d1/dylib.log) walkAfter=$walk_after; timeout: pass=$(pass_n $d2) slot='$slot' rebound=$(cnt 'entries rebound' $d2/dylib.log)"; fi
  clear_sentinel $BC; clear_sentinel $BP
}
c_act-main() {
  clear_sentinel $BP; local d1 d2 f
  run plain mn1 M16_ACTIVATE_NEEDS_MAIN=1 M5APP_EXPECT=stock; d1=$(res plain mn1); f=$(fc $d1)
  # bg caller: plain-orient coord-bg (first call on a camera queue; main is busy 300 ms, then free). plain's threads mode can't
  # be used: its main thread blocks in a group wait on the three threads, so prepare's dispatch_sync to main can never run.
  clear_sentinel $BO; p12 coord-bg landscapeLeft mn2 M16_ACTIVATE_NEEDS_MAIN=1; local d2=$PD
  if in_range "$f" 900 1150 && [[ $(cnt DONE $d1/app.log) == 1 && $(pass_n $d1) == 2 && $(cnt 'LAZY armed by .* on bg thread' $d2/dylib.log) == 1 && $(cnt 'P12\| DONE' $d2/app.log) == 1 ]]
  then ok "main caller: stock at ${f} ms, no deadlock, app finished; bg caller (camera queue): armed ($(grep -oE 'firstcall\.ms=[0-9]+' $d2/app.log))"
  else bad "main: fc=$f done=$(cnt DONE $d1/app.log) pass=$(pass_n $d1); bg: armed=$(cnt 'LAZY armed by .* on bg thread' $d2/dylib.log) $(grep -oE 'firstcall\.ms=[0-9]+' $d2/app.log)"; fi
  clear_sentinel $BP; clear_sentinel $BO
}
c_act-reenter() {
  clear_sentinel $BP; local d ms
  run plain re M16_REENTER=1; d=$(res plain re); ms=$(grep -oE 'LAZY armed by .* in [0-9.]+ ms' $d/dylib.log | grep -oE '[0-9.]+ ms$' | cut -d' ' -f1)
  if [[ $(pass_n $d) == 14 && $(cnt 're-entrant call init forwarded to stock' $d/dylib.log) == 1 ]] && (( ${ms%.*} < 20 )); then ok "armed in ${ms} ms, 14/14; 're-entrant call init forwarded to stock (no wait)'"
  else bad "pass=$(pass_n $d) reenter=$(cnt 're-entrant call' $d/dylib.log) armed in ${ms:-?} ms"; fi
  clear_sentinel $BP
}
# tsan_ours <app.log>: "<libsimcam frame> <unrestored stack> <no SUMMARY>" — ThreadSanitizer reports that may be the engine's,
# by the first reason that applies. A report runs from its WARNING line to its SUMMARY line; one that a new WARNING or the end
# of the log interrupts first was cut short (the app was killed mid-report) and counts whatever its frames say.
tsan_ours() {
  awk 'function done() { if (!b) return; if (s) l++; else if (f) u++; else if (!m) c++; b = 0 }
    /WARNING: ThreadSanitizer/ { done(); b = 1; s = 0; f = 0; m = 0; next }
    b && /libsimcam/ { s = 1 }
    b && /\[failed to restore the stack\]/ { f = 1 }
    b && /SUMMARY: ThreadSanitizer/ { m = 1; done() }
    END { done(); print l + 0, u + 0, c + 0 }' $1
}
c_threads() {
  clear_sentinel $BP; local n=${THREADS_N:-100} i d okn=0 dl=0 bad1= ts=0 te=0 t e k tk="0 0 0" kl=0 kf=0 kc=0
  for i in {1..$n}; do
    run plain th$i M5APP_ORDER=threads; d=$(res plain th$i)
    # ThreadSanitizer reports (a TSan app + engine: act-tsan's threads x100 half; none on a normal build). A report counts
    # against the case (as possibly ours) if it has a libsimcam frame, a stack TSan could not restore ("[failed to restore
    # the stack]": the other access may be ours), or no SUMMARY line (cut short: the app was killed mid-report). plain's own
    # races (its M9 frame counter: both stacks restored, no libsimcam frame) are counted apart.
    t=$(cnt 'WARNING: ThreadSanitizer' $d/app.log); ts=$((ts + t))
    e=0; (( t )) && { cp $d/app.log $SIMCAM_RESULTS/activation-$RT-$TAG-th$i-tsan.log
      k=$(tsan_ours $d/app.log); e=$(( ${${=k}[1]} + ${${=k}[2]} + ${${=k}[3]} ))
      kl=$((kl + ${${=k}[1]})); kf=$((kf + ${${=k}[2]})); kc=$((kc + ${${=k}[3]})); }
    te=$((te + e))
    if [[ $(cnt DONE $d/app.log) != 1 ]]; then dl=$((dl + 1)); bad1="$bad1 $i:nodone"
    elif (( e )); then bad1="$bad1 ${i}:tsan-engine=$e"
    elif [[ $(cnt 'PASS order.threads ' $d/app.log) == 1 && $(cnt 'LAZY armed by' $d/dylib.log) == 1 && $(pass_n $d) == 15 ]]; then okn=$((okn + 1))
    else bad1="$bad1 $i:$(grep -oE '(PASS|FAIL) order.threads[^|]*' $d/app.log | cut -c1-60)"; fi
    rm -rf $d   # keep results small; a failing run's verdict is in the line
  done
  (( okn == n )) && ok "$n/$n: no deadlock, one armed-by line, all 3 threads saw cameras, 15/15; ThreadSanitizer: $te possibly ours ($kl libsimcam frame, $kf unrestored stack, $kc cut short), $ts in all" || bad "$okn/$n ok, $dl no DONE, TSan $te possibly ours ($kl libsimcam, $kf unrestored, $kc cut short) / $ts all;${bad1:0:300}"
  clear_sentinel $BP
}
c_threads-slow() {
  clear_sentinel $BP; local d1 d2 m1 m2
  run plain ts1 M5APP_ORDER=threads M16_SLOW_ACTIVATE_MS=500; d1=$(res plain ts1)
  m1=$(grep -oE 'firstcall\.ms thread=[0-9] ms=-?[0-9]+' $d1/app.log | grep -oE 'ms=-?[0-9]+$' | cut -d= -f2 | tr '\n' ' ')
  clear_sentinel $BP; run plain ts2 M5APP_ORDER=threads M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d2=$(res plain ts2)
  m2=$(grep -oE 'firstcall\.ms thread=[0-9] ms=-?[0-9]+' $d2/app.log | grep -oE 'ms=-?[0-9]+$' | cut -d= -f2 | tr '\n' ' ')
  local okA=1 okB=1 x
  for x in ${=m1}; do in_range $x 450 550 || okA=0; done; for x in ${=m2}; do in_range $x 900 1100 || okB=0; done
  if [[ $okA == 1 && ${#${=m1}} == 3 && $(cnt 'PASS order.threads ' $d1/app.log) == 1 && $okB == 1 && ${#${=m2}} == 3 && $(cnt 'PASS order.threads.stock' $d2/app.log) == 1 ]]
  then ok "500: all armed, per-thread ms $m1; 1500: each returned by t0+1.1 s ($m2), all 3 saw stock"
  else bad "500: ms '$m1' threads=$(cnt 'PASS order.threads ' $d1/app.log); 1500: ms '$m2' stock=$(cnt 'PASS order.threads.stock' $d2/app.log)"; fi
  clear_sentinel $BP
}
p12() {   # p12 <mode> <orient> <tag> [K=V ...]: plain-orient in boot mode; sets PD
  local m=$1 o=$2 t=$3; shift 3
  NOINJ=1 MAXW=150 $ROOT/scripts/p12.sh $RT $m back $o $TAG-$t "$@" >/dev/null 2>&1
  PD=$SIMCAM_RESULTS/p12-$m-$RT-back-$o-$TAG-$t
  # p12.sh copies the newest engine log; take this launch's own one (its pid) instead
  local pid=$(app_pid $PD/app.log) bl=$(cat $ROOT/build/bootlogdir-$RT.txt)
  [[ -n $pid && -f $bl/dylib-$pid.log ]] && cp $bl/dylib-$pid.log $PD/dylib.log
}
angles() { { grep -oE 'previewAngle=[0-9.]+ captureAngle=[0-9.]+' $1/app.log | head -1; grep -oE '(photo|movie) connection angle=[0-9.]+' $1/app.log; } | tr '\n' ' '; }   # one line
c_orient-nomotion() {
  clear_sentinel $BO; p12 orient landscapeLeft nm1 M5_NO_MOTION=1; local a1=$(angles $PD) d1=$PD
  p12 orient landscapeLeft nm0; local a0=$(angles $PD)
  if [[ -n $a1 && $a1 == $a0 && $a1 != *captureAngle=90* && $(cnt 'motion: hooked' $d1/app.log) == 0 && $(cnt 'P12\| DONE' $d1/app.log) == 1 ]]
  then ok "M5_NO_MOTION=1 (CoreMotion stock): '$a1', same as without the switch"
  else bad "no-motion '$a1' vs with motion '$a0' (hooked=$(cnt 'motion: hooked' $d1/app.log))"; fi
  clear_sentinel $BO
}
c_orient-sameturn() {
  clear_sentinel $BO; p12 coord-first landscapeLeft sf P12_UI=landscape
  local l=$(grep -m1 -E '(PASS|FAIL) coord-first' $PD/app.log)
  [[ $l == *PASS* ]] && ok "${l#*\| } ($(grep -m1 'COORD same-turn' $PD/app.log | grep -oE 'ui=[0-9]+ dev=[0-9]+'))" || bad "${l:-no verdict}; $(grep 'COORD' $PD/app.log | tr '\n' ' ' | cut -c1-200)"
  clear_sentinel $BO
}
c_orient-bg() {
  clear_sentinel $BO; p12 coord-bg landscapeLeft cb P12_UI=landscape
  local l=$(grep -m1 -E '(PASS|FAIL) coord-bg' $PD/app.log)
  local detail="$(grep 'COORD' $PD/app.log | sed 's/^P12| //' | tr '\n' ';' | cut -c1-240) $(grep -m1 -oE 'ui orientation at load: .*' $PD/dylib.log)"
  [[ $l == *PASS* ]] && ok "${l#*\| } — $detail" || bad "${l:-no verdict} — $detail"
  clear_sentinel $BO
}
c_orient-bg-turnback() {   # the UI turns landscape, then back to portrait, before the first camera call: the snapshot follows both
  clear_sentinel $BO; p12 coord-bg landscapeLeft tb P12_UI=turnback
  local l=$(grep -m1 -E '(PASS|FAIL) coord-bg-turnback' $PD/app.log) snap=$(grep -m1 -oE 'ui orientation at load: .*' $PD/dylib.log)
  local detail="$(grep -E 'COORD|turnback ui' $PD/app.log | sed 's/^P12| //' | tr '\n' ';' | cut -c1-240) $snap"
  [[ $l == *PASS* && $snap == *'then 3 (geometry change), then 1 (geometry change)'* ]] && ok "${l#*\| } — $detail" || bad "${l:-no verdict} — $detail"
  clear_sentinel $BO
}
c_mutation-L57() {   # M16_NO_UIORIENT_AT_LOAD=1: no UI orientation snapshot at load (option (b) off): orient-bg must catch it
  clear_sentinel $BO; p12 coord-bg landscapeLeft l57 P12_UI=landscape M16_NO_UIORIENT_AT_LOAD=1
  local l=$(grep -m1 -E '(PASS|FAIL) coord-bg' $PD/app.log)
  if [[ $l == *FAIL*preview=90* && $(cnt 'ui orientation at load: off \(M16_NO_UIORIENT_AT_LOAD\)' $PD/dylib.log) == 1 ]]
  then ok "mutant caught: ${l#*\| } (the coordinator on the camera queue reads portrait until main frees)"
  else bad "mutant not caught: ${l:-no verdict}; $(grep -m1 -oE 'ui orientation at load: .*' $PD/dylib.log)"; fi
  clear_sentinel $BO
}
c_act-traced() {   # lldb attached (SB API, no GUI) from before the first instruction: budget x10; 11000 -> unknown-hang, not counted
  local py=/Applications/Xcode.app/Contents/Developer/usr/bin/python3 lp=$(xcrun lldb -P) e=$(ev $BP) n0 i d pid
  typeset -A F A
  clear_sentinel $BP; n0=$(nl $e)
  for i in 1500 11000; do
    local x=(M16_SLOW_ACTIVATE_MS=$i); [[ $i == 11000 ]] && x+=(M5APP_EXPECT=stock)
    ( XENV="${(F)x}"$'\n' SIMCAM_NO_RETRY=1 WAIT_DEBUGGER=1 $ROOT/scripts/run.sh plain $RT boot $TAG-tr$i >/dev/null 2>&1 ) & local rp=$!
    d=$(res plain tr$i); pid=
    # simctl prints the pid only once the app runs: take it from the host's process list (the path names this sim's UDID)
    for _ in {1..200}; do pid=$(pgrep -f "Devices/$U/data/Containers/Bundle/Application/.*/m5app.app/m5app" | head -1); [[ -n $pid ]] && break; sleep 0.05; done
    if [[ -n $pid ]]; then PYTHONPATH=$lp $py $ROOT/scripts/lldb-attach.py $pid $d/app.log 'DONE' 40 > $SIMCAM_RESULTS/activation-$RT-$TAG-attach$i.txt 2>&1
    else echo "ATTACH| no pid" > $SIMCAM_RESULTS/activation-$RT-$TAG-attach$i.txt; fi
    wait $rp; F[$i]=$(fc $d); A[$i]=$(grep -m1 -oE 'attached pid [0-9]+ in [0-9.]+ s|attach FAILED.*|no pid' $SIMCAM_RESULTS/activation-$RT-$TAG-attach$i.txt)
  done
  local d1=$(res plain tr1500) d2=$(res plain tr11000) uh=$(since $e $n0 | grep -c 'unknown-hang activate') hg=$(since $e $n0 | grep -cE ' hang activate')
  local mk=$(nf $STATE/$BP/latched-*(N))
  if [[ ${A[1500]} == attached* && ${A[11000]} == attached* && $(pass_n $d1) == 14 && $(cnt 'LAZY armed by' $d1/dylib.log) == 1 ]] && in_range "${F[1500]}" 1450 1700 &&
     in_range "${F[11000]}" 9800 10200 && [[ $(cnt DONE $d2/app.log) == 1 && $(pass_n $d2) == 2 && $uh == 1 && $hg == 0 && $mk == 0 ]]
  then ok "traced (${A[1500]}; ${A[11000]}): =1500 armed at ${F[1500]} ms (budget x10), 14/14; =11000 stock at ${F[11000]} ms, 1 'unknown-hang activate' (not counted: 0 hang lines), no marker"
  else bad "attach '${A[1500]}' / '${A[11000]}'; =1500 fc=${F[1500]:-?} pass=$(pass_n $d1) armed=$(cnt 'LAZY armed by' $d1/dylib.log); =11000 fc=${F[11000]:-?} done=$(cnt DONE $d2/app.log) unknown-hang=$uh hang=$hg markers=$mk"; fi
  clear_sentinel $BP
}
c_motion-only() {
  local n=${MOTION_N:-10} i d okn=0 bad1=
  for i in {1..$n}; do
    run plain mo$i M5APP_ORDER=motion-only; d=$(res plain mo$i)
    local order=$(grep -nE 'M5APP\| order=motion-only' $d/app.log | head -1 | cut -d: -f1) tr=$(grep -nE 'tracking started by' $d/app.log | head -1 | cut -d: -f1)
    if [[ $(cnt 'PASS order.motion-only' $d/app.log) == 3 && $(cnt 'LAZY armed' $d/app.log) == 0 && -n $tr && -n $order ]] && (( tr > order )) && [[ $(cnt DONE $d/app.log) == 1 ]]; then okn=$((okn + 1))
    else bad1="$bad1 [$i: pass=$(cnt 'PASS order.motion-only' $d/app.log) armed=$(cnt 'LAZY armed' $d/app.log) tracking@$tr order@$order]"; fi
  done
  (( okn == n )) && ok "$n/$n: stand-in answers (available, rate, timestamps), never armed, '$(grep -m1 -oE 'tracking started by [^)]*\)' $d/app.log)' after the app's first query (no tracker before it)" || bad "$okn/$n;${bad1:0:300}"
}
c_motion-threads() {
  local d; run plain mt M5APP_ORDER=motion-threads; d=$(res plain mt)
  local q=$(grep -oE 'firstquery ms=\[[^]]*\]' $d/app.log)
  if [[ $(cnt 'PASS order.motion-threads.updates' $d/app.log) == 1 && $(cnt 'PASS order.motion-threads.firstquery' $d/app.log) == 1 && $(cnt 'tracking started by' $d/app.log) == 1 ]]
  then ok "all 3 managers got updates ($(grep -oE 'counts=\[[^]]*\]' $d/app.log)), one 'tracking started', first queries $q"
  else bad "$(grep -E 'order.motion-threads' $d/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-200) tracking=$(cnt 'tracking started by' $d/app.log)"; fi
}
c_none() {
  local d; run plain no M5APP_ORDER=none; d=$(res plain no)
  # the tracker (0.5 s main-queue timer + device-orientation notifications) logs 'tracking started by' (motion start) or
  # 'tracker starts' (any start); the load-time UI orientation snapshot (L57) logs nothing in an app that never starts it
  [[ $(cnt 'LAZY armed' $d/app.log) == 0 && $(cnt 'tracking started|tracker starts' $d/app.log) == 0 && $(cnt DONE $d/app.log) == 1 ]] && ok "no armed-by, no tracker start (no main-queue timer, no orientation notifications), DONE" || bad "armed=$(cnt 'LAZY armed' $d/app.log) tracking=$(cnt 'tracking started|tracker starts' $d/app.log) done=$(cnt DONE $d/app.log)"
}
c_motion-gate() {
  clear_sentinel $BP; local d; run plain mg M5APP_ORDER=motion SIMCAM_PF_BREAK=1; d=$(res plain mg)
  local end=$(grep -oE 'motion.end counts=\[[0-9]+\]' $d/app.log | grep -oE '[0-9]+') mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N))
  if [[ $(cnt 'stand-in removed' $d/dylib.log) == 0 && $(cnt 'PASS order.motion.rate' $d/app.log) == 1 && $(cnt 'DISABLED - preflight failed' $d/app.log) -ge 1 && $mk == 0 && $(cnt DONE $d/app.log) == 1 ]] && (( ${end:-0} > 180 ))
  then ok "camera stock (forced preflight failure), stand-in kept, stream kept running ($end updates by the end), no 'stand-in removed', no marker"
  else bad "removed=$(cnt 'stand-in removed' $d/dylib.log) rate=$(cnt 'PASS order.motion.rate' $d/app.log) end=$end markers=$mk"; fi
  clear_sentinel $BP
}
c_camera-then-motion() {
  clear_sentinel $BP; local d; run plain ctm M5APP_ORDER=camera-then-motion SIMCAM_PF_BREAK=1; d=$(res plain ctm)
  local mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N))
  if [[ $(cnt 'camera-then-motion discovery=0' $d/app.log) == 1 && $(cnt 'PASS order.camera-then-motion' $d/app.log) == 3 && $mk == 0 ]]
  then ok "camera stock (0 devices), then the stand-in answers in the same launch (available, rate, timestamps), no marker"
  else bad "$(grep -E 'discovery=|order.camera-then-motion' $d/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-200) markers=$mk"; fi
  clear_sentinel $BP
}
c_mutation-L56() {   # M16_NO_ACTTIMEOUT=1 brings back today's unbounded inline activation: act-slow-timeout must catch it
  clear_sentinel $BP; local d f; run plain l53 M16_NO_ACTTIMEOUT=1 M16_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d=$(res plain l53); f=$(fc $d)
  if (( ${f:-0} >= 1500 )) && [[ $(cnt 'inline: M16_NO_ACTTIMEOUT' $d/dylib.log) == 1 ]]; then ok "mutant caught: the first call waited ${f} ms (act-slow-timeout wants 1000 ± 100), inline armed"
  else bad "mutant not caught: firstcall ${f:-?} ms"; fi
  clear_sentinel $BP
}

# ---------------------------------------------------------------- CoreMotion decided at load from markers
c_orient-nomotion-marker() {   # a planted latched-motion marker instead of M5_NO_MOTION: the same landscape angles, CoreMotion stock with its line
  clear_sentinel $BO; local now=$(date +%s) a0 a1 d1
  p12 orient landscapeLeft nmk0; a0=$(angles $PD)
  plant_marker $STATE/$BO/latched-motion-$BUILD motion $(( now + 7200 ))
  p12 orient landscapeLeft nmk1 P12_MOTION_QUERY=1; a1=$(angles $PD); d1=$PD
  if [[ -n $a1 && $a1 == $a0 && $a1 != *captureAngle=90* && $(cnt 'motion: hooked' $d1/app.log) == 0 && $(cnt 'one-shot forwarding traps' $d1/app.log) == 1 &&
        $(cnt 'INFO motion accel=false' $d1/app.log) == 1 && $(mstock $d1) == 1 && $(cnt "latched-motion-$BUILD: motion until" $d1/dylib.log) == 1 && $(cnt 'P12\| DONE' $d1/app.log) == 1 ]]
  then ok "marker: '$a1', same as without it; CoreMotion stock (accel=false), one stock line: '$(grep -m1 -oE 'CoreMotion is stock this launch \([^)]*\)' $d1/dylib.log)'"
  else bad "marker '$a1' vs none '$a0'; hooked=$(cnt 'motion: hooked' $d1/app.log) traps=$(cnt 'one-shot forwarding traps' $d1/app.log) accel=$(grep -oE 'motion accel=[a-z]+' $d1/app.log) stocklines=$(mstock $d1)"; fi
  clear_sentinel $BO
}
c_camera-then-motion-real() {   # a REAL preflight failure: stand-in this launch, preflight-fail written, the next motion-only is stock
  clear_sentinel $BP; local d1 d2 pf=$STATE/preflight-fail-$BUILD now=$(date +%s) m
  run plain ctr1 M5APP_ORDER=camera-then-motion SIMCAM_PF_BREAK=1 M16_PF_FAIL_REAL=1; d1=$(res plain ctr1); m=$(mtime $pf)
  run plain ctr2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d2=$(res plain ctr2)
  if [[ $(cnt 'camera-then-motion discovery=0' $d1/app.log) == 1 && $(cnt 'PASS order.camera-then-motion' $d1/app.log) == 3 && $(mstock $d1) == 0 ]] &&
     (( m - now >= 86400 - 120 && m - now <= 86400 + 120 )) && mstockok $d2 motion-only && [[ $(cnt "preflight-fail-$BUILD: preflight failed until" $d2/dylib.log) == 1 ]]
  then ok "same launch: camera stock, stand-in answered; preflight-fail written (until +$(( (m - now) / 3600 )) h); next motion-only: stock (accel=false, 0 updates), one line '$(grep -m1 -oE 'CoreMotion is stock this launch \([^)]*\)' $d2/dylib.log)'"
  else bad "L1: $(grep -E 'discovery=|order.camera-then-motion' $d1/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-160) pf until +$(( m - now )) s; L2: $(grep -E 'order.motion-only' $d2/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-160) hooked=$(cnt 'motion: hooked' $d2/app.log) lines=$(mstock $d2)"; fi
  clear_sentinel $BP
}
c_motion-after-gate() {   # a FORCED preflight failure leaves no marker: the next motion-only gets the stand-in
  clear_sentinel $BP; local d1 d2
  run plain mag1 M5APP_ORDER=motion SIMCAM_PF_BREAK=1; d1=$(res plain mag1)
  local mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N))
  run plain mag2 M5APP_ORDER=motion-only; d2=$(res plain mag2)
  if [[ $(cnt 'DISABLED - preflight failed' $d1/app.log) -ge 1 && $(cnt 'PASS order.motion.rate' $d1/app.log) == 1 && $mk == 0 ]] && mstand $d2 motion-only
  then ok "motion-gate launch: camera stock, stand-in kept, no marker; next motion-only: stand-in (available, rate, timestamps), no stock line"
  else bad "gate: disabled=$(cnt 'DISABLED - preflight failed' $d1/app.log) rate=$(cnt 'PASS order.motion.rate' $d1/app.log) markers=$mk; next: $(grep -E 'order.motion-only' $d2/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-160) lines=$(mstock $d2)"; fi
  clear_sentinel $BP
}
c_motion-latched() {   # preflight-fail (real) and a camera latch marker each stock CoreMotion from the first query; clear brings it back
  clear_sentinel $BP; local e=$(ev $BP) d0 d1 d2 d3 now=$(date +%s) n0 r1 r2 r3
  run plain mla0 M16_ACTIVATE_FAIL=preflight M16_PF_FAIL_REAL=1 M5APP_EXPECT=stock; d0=$(res plain mla0)
  run plain mla1 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d1=$(res plain mla1)
  mstockok $d1 motion-only && [[ $(cnt "preflight-fail-$BUILD" $d1/dylib.log) == 1 && -f $STATE/preflight-fail-$BUILD ]] && r1=ok || r1="pf: $(grep -E 'order.motion-only' $d1/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d1)"
  clear_sentinel $BP; plant_marker $STATE/$BP/latched-$BUILD activate $(( now + 7200 ))
  run plain mla2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d2=$(res plain mla2)
  mstockok $d2 motion-only && [[ $(cnt "latched-$BUILD: activate until" $d2/dylib.log) == 1 ]] && r2=ok || r2="camera marker: $(grep -E 'order.motion-only' $d2/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d2)"
  plant_marker $STATE/preflight-fail-$BUILD "preflight failed" $(( now + 7200 )); n0=$(nl $e)
  run plain mla3 M5APP_ORDER=motion-only SIMCAM_SENTINEL=clear; d3=$(res plain mla3)
  local left=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N)) cl=$(since $e $n0 | grep -c ' clear - .*SIMCAM_SENTINEL=clear')
  mstand $d3 motion-only && [[ $left == 0 && $cl == 1 ]] && r3=ok || r3="clear: $(grep -E 'order.motion-only' $d3/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) markers left=$left clear lines=$cl"
  if [[ $r1 == ok && $r2 == ok && $r3 == ok ]]
  then ok "real preflight failure -> next motion-only stock (accel=false, 0 updates), exactly one stock line; planted latched-$BUILD -> the same, line names it; SIMCAM_SENTINEL=clear -> stand-in, markers gone, clear line appended"
  else bad "$r1 | $r2 | $r3"; fi
  clear_sentinel $BP
}
c_motion-pf-expiry() {   # preflight-fail expires after 24 h; a passing preflight deletes it
  clear_sentinel $BP; local pf=$STATE/preflight-fail-$BUILD now=$(date +%s) d1 d2 d3 d4 d5 r1 r2 r3 r4 r5
  plant_marker $pf "preflight failed" $(( now - 60 ))   # written 24 h + 1 min ago
  run plain mpe1 M5APP_ORDER=motion-only; d1=$(res plain mpe1); mstand $d1 motion-only && r1=ok || r1="expired: $(grep -E 'order.motion-only' $d1/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d1)"
  plant_marker $pf "preflight failed" $(( now + 82800 ))   # fresh (written 1 h ago)
  run plain mpe2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d2=$(res plain mpe2); mstockok $d2 motion-only && r2=ok || r2="fresh (control): $(grep -E 'order.motion-only' $d2/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120)"
  run plain mpe3; d3=$(res plain mpe3); [[ $(pass_n $d3) == 14 && ! -f $pf ]] && r3=ok || r3="passing camera launch: pass=$(pass_n $d3) pf=$([[ -f $pf ]] && echo kept || echo gone)"
  run plain mpe4 M5APP_ORDER=motion-only; d4=$(res plain mpe4); mstand $d4 motion-only && r4=ok || r4="after: $(grep -E 'order.motion-only' $d4/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d4)"
  # a fresh marker that the same launch's passing preflight unlinks before the first motion call: the stock
  # line still names it with a real until (from the load-time stat), never "?"
  plant_marker $pf "preflight failed" $(( now + 82800 ))
  run plain mpe5 M5APP_ORDER=camera-then-motion M5APP_EXPECT_MOTION=stock; d5=$(res plain mpe5)
  local l5=$(grep -m1 -oE 'simcam: CoreMotion is stock this launch \([^)]*\)' $d5/dylib.log)
  mstockok $d5 camera-then-motion && [[ ! -f $pf && $l5 == *"preflight-fail-$BUILD: preflight failed until "[0-9][0-9][0-9][0-9]-* && $l5 != *'?'* ]] && r5=ok || r5="unlinked mid-launch: pf=$([[ -f $pf ]] && echo kept || echo gone) line '$l5' lines=$(mstock $d5)"
  if [[ $r1 == ok && $r2 == ok && $r3 == ok && $r4 == ok && $r5 == ok ]]
  then ok "expired (24 h + 1 min): stand-in; fresh: stock (control); a passing camera launch (14/14) unlinked it; next motion-only: stand-in; unlinked by the same launch's preflight before the first motion call: '${l5#simcam: CoreMotion is stock this launch }'"
  else bad "$r1 | $r2 | $r3 | $r4 | $r5"; fi
  clear_sentinel $BP
}
motion_crash_seq() {   # motion_crash_seq <tag> [K=V ...]: launches N, N+1 crash in a motion tick (camera armed); sets MC_D1 MC_D2 MC_EV MC_MK MC_MKT
  local t=$1; shift; local e=$(ev $BP) n0; clear_sentinel $BP; n0=$(nl $e)
  run plain $t-1 M5APP_ORDER=camera-then-motion M16_MOTION_FAULT=1 "$@"; MC_D1=$(res plain $t-1)
  run plain $t-2 M5APP_ORDER=camera-then-motion M16_MOTION_FAULT=1 "$@"; MC_D2=$(res plain $t-2)
  MC_EV=$(since $e $n0 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' ')
  MC_MK=$(nf $STATE/$BP/latched-motion-$BUILD(N)); MC_MKT=$(mtime $STATE/$BP/latched-motion-$BUILD)
  MC_C1=$(since $e $n0 | awk '$3 == "crash" && $4 == "motion" {print $1; exit}')
}
c_motion-crash() {   # crashes in the stand-in at N, N+1 (camera armed): `crash motion`, not provider; N+1's handler writes latched-motion
  motion_crash_seq mc; local d1=$MC_D1 d2=$MC_D2 d3 dc eng=0
  local p1=$(fault_pid $d1/app.log 'M16_MOTION_FAULT: fault') p2=$(fault_pid $d2/app.log 'M16_MOTION_FAULT: fault'); crash_reports m5app 30 $p1 $p2
  local ut=$(( MC_MKT - ${MC_C1:-0} ))
  run plain mc-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT_MOTION=stock; d3=$(res plain mc-3)
  local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $d3/app.log | grep -oE '[0-9]+$')
  run canary mc-c; dc=$(res canary mc-c)
  if [[ $MC_EV == "crash-motionx2 " && $MC_MK == 1 && -n $p1 && -n $p2 ]] && (( ut >= 86400 - 5 && ut <= 86400 + 5 )) && (( ${disc:-0} > 0 )) &&
     [[ $(cnt 'LAZY armed by' $d3/dylib.log) == 1 ]] && mstockok $d3 camera-then-motion && [[ $(cnt "latched-motion-$BUILD: motion until" $d3/dylib.log) == 1 && $(cat $dc/summary.txt 2>/dev/null) == *core=PASS* ]]; then eng=1; fi
  verdict_reports $eng 2 "N, N+1 (pids $p1 $p2): $MC_EV(no provider), $CR crash reports; N+1's handler wrote latched-motion (until = N's strike + 24 h); N+2: camera armed ($disc devices), CoreMotion stock from the first query, one line; canary $(cut -c1-40 $dc/summary.txt)" \
    "events '$MC_EV' marker=$MC_MK until-strike=${ut}s fault pids '$p1' '$p2' reports=$CR; N+2 discovery=${disc:-?} armed=$(cnt 'LAZY armed by' $d3/dylib.log) $(grep -E 'order.camera-then-motion' $d3/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d3); canary '$(cut -c1-60 $dc/summary.txt 2>/dev/null)'"
  clear_sentinel $BP; clear_sentinel $BC
}
c_motion-stock-threads() {   # a latched-motion launch whose 3 threads make their first motion call at once: exactly one stock line
  local i d r= okn=0 now=$(date +%s)
  for i in 1 2 3; do
    clear_sentinel $BP; plant_marker $STATE/$BP/latched-motion-$BUILD motion $(( now + 7200 ))
    run plain mst$i M5APP_ORDER=motion-threads M5APP_EXPECT_MOTION=stock; d=$(res plain mst$i)
    if [[ $(cnt 'PASS order.motion-threads.stock.updates' $d/app.log) == 1 && $(cnt 'motion: hooked' $d/app.log) == 0 && $(mstock $d) == 1 && $(cnt DONE $d/app.log) == 1 ]]; then okn=$((okn + 1))
    else r="$r $i: $(grep -E 'order.motion-threads' $d/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d)"; fi
  done
  (( okn == 3 )) && ok "3/3: three threads' first calls into the forwarding traps at once, 0 updates (stock), exactly one stock line each" || bad "$okn/3 ok;$r"
  clear_sentinel $BP
}
c_pf-prune() {   # PREPARE 1 prunes preflight-fail-* past its until (this build and others); another build's fresh one stays
  clear_sentinel $BP; local now=$(date +%s) d mine=$STATE/preflight-fail-$BUILD old=$STATE/preflight-fail-0000deadbeef fresh=$STATE/preflight-fail-0000cafef00d
  plant_marker $mine "preflight failed" $(( now - 60 )); plant_marker $old "preflight failed" $(( now - 3600 )); plant_marker $fresh "preflight failed" $(( now + 3600 ))
  run plain pfp; d=$(res plain pfp)
  local k="mine=$([[ -f $mine ]] && echo kept || echo gone) other-expired=$([[ -f $old ]] && echo kept || echo gone) other-fresh=$([[ -f $fresh ]] && echo kept || echo gone)"
  if [[ $(pass_n $d) == 14 && $k == "mine=gone other-expired=gone other-fresh=kept" && $(cnt 'removed expired marker .*preflight-fail-' $d/dylib.log) == 2 ]]
  then ok "one camera launch (14/14): $k, 2 'removed expired marker' lines"
  else bad "pass=$(pass_n $d) $k removed-lines=$(cnt 'removed expired marker .*preflight-fail-' $d/dylib.log)"; fi
  rm -f $old $fresh; clear_sentinel $BP
}
orient_crash_seq() {   # orient_crash_seq <tag> [K=V ...]: plain-orient coord-bg (armed, a coordinator) faulting in the tracker's upd
  # at N and N+1, then N+2 without the fault. Sets OC_EV (crash lines of N, N+1) OC_PIDS (the fault lines' pids) OC_D3
  local t=$1; shift; local e=$(ev $BO) n0; clear_sentinel $BO; n0=$(nl $e)
  p12 coord-bg landscapeLeft $t-1 P12_UI=landscape M16_ORIENT_FAULT=1 "$@"; OC_PIDS=$(fault_pid $PD/app.log 'M16_ORIENT_FAULT: fault')
  p12 coord-bg landscapeLeft $t-2 P12_UI=landscape M16_ORIENT_FAULT=1 "$@"; OC_PIDS="$OC_PIDS $(fault_pid $PD/app.log 'M16_ORIENT_FAULT: fault')"
  OC_EV=$(since $e $n0 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' ')
  p12 coord-bg landscapeLeft $t-3 P12_UI=landscape "$@"; OC_D3=$PD
}
c_orient-crash() {   # a fault in the orientation tracker (camera code) counts against the camera, never `motion`: N+2 is camera-latched
  orient_crash_seq oc; local l3=$(grep -m1 -E '(PASS|FAIL) coord-bg' $OC_D3/app.log) eng=0; local -a pp; pp=(${=OC_PIDS}); crash_reports p12 30 $pp
  [[ $OC_EV == (crash-providerx2|crash-activatex2|crash-activatex1 crash-providerx1)' ' && ${#pp} == 2 && $(cnt 'latched after' $OC_D3/dylib.log) == 1 && $l3 == *'FAIL coord-bg no device'* ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OC_PIDS) faulted in upd after the coordinators' KVO: $OC_EV(no motion), $CR crash reports; N+2 camera-latched in PREPARE 1 (${l3#*\| })" \
    "events '$OC_EV' fault pids '$OC_PIDS' reports=$CR; N+2 latched=$(cnt 'latched after' $OC_D3/dylib.log) '${l3:-no verdict}'"
  clear_sentinel $BO
}
c_mutation-L61() {   # M17_TRACKER_MOTION_SCOPE=1: upd under the motion key again: orient-crash must catch it
  orient_crash_seq l61 M17_TRACKER_MOTION_SCOPE=1; local l3=$(grep -m1 -E '(PASS|FAIL) coord-bg' $OC_D3/app.log)
  if [[ $OC_EV == 'crash-motionx2 ' && $(cnt 'latched after' $OC_D3/dylib.log) == 0 ]]; then ok "mutant caught: '$OC_EV', N+2 camera not latched (${l3#*\| }; orient-crash wants provider and a latched camera)"
  else bad "mutant not caught: events '$OC_EV'; N+2 latched=$(cnt 'latched after' $OC_D3/dylib.log) '${l3:-no verdict}'"; fi
  clear_sentinel $BO
}
c_mutation-L58() {   # M17_NO_MOTION_MARKERS=1: motion ignores markers at load: motion-latched must catch it (the stand-in answers)
  clear_sentinel $BP; local d now=$(date +%s); plant_marker $STATE/$BP/latched-motion-$BUILD motion $(( now + 7200 ))
  run plain l58 M5APP_ORDER=motion-only M17_NO_MOTION_MARKERS=1; d=$(res plain l58)
  if mstand $d motion-only; then ok "mutant caught: with a valid latched-motion marker the stand-in still answered (motion-latched wants stock + one line)"
  else bad "mutant not caught: $(grep -E 'order.motion-only' $d/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-160) lines=$(mstock $d)"; fi
  clear_sentinel $BP
}
c_mutation-L59() {   # M17_NO_MOTION_KEY=1: no feature key: motion crashes are `provider` and close the camera: motion-crash must catch it
  motion_crash_seq l59 M17_NO_MOTION_KEY=1; local d3
  run plain l59-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT=stock; d3=$(res plain l59-3)
  if [[ $MC_EV == "crash-providerx2 " && $(cnt 'camera-then-motion discovery=0' $d3/app.log) == 1 ]]; then ok "mutant caught: '$MC_EV', N+2 camera latched (0 devices; motion-crash wants motion x2 and the camera armed)"
  else bad "mutant not caught: events '$MC_EV'; N+2 $(grep -oE 'discovery=[0-9]+' $d3/app.log)"; fi
  clear_sentinel $BP
}
c_mutation-L60() {   # M17_NO_HANDLER_MARKER=1: the second strike writes no marker: N+2 still gets the stand-in (one launch late)
  motion_crash_seq l60 M17_NO_HANDLER_MARKER=1; local d3
  run plain l60-3 M5APP_ORDER=camera-then-motion; d3=$(res plain l60-3)
  if [[ $MC_EV == "crash-motionx2 " && $MC_MK == 0 ]] && mstand $d3 camera-then-motion; then ok "mutant caught: '$MC_EV', no latched-motion after N+1, N+2 got the stand-in (motion-crash wants stock at N+2)"
  else bad "mutant not caught: events '$MC_EV' marker=$MC_MK; N+2 $(grep -E 'order.camera-then-motion' $d3/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-120) lines=$(mstock $d3)"; fi
  clear_sentinel $BP
}

# ---------------------------------------------------------------- the attitude stand-in
mprobe() {   # mprobe <tag> <probe args...>: one fixtures/motion-probe launch (XENV from MPX, one K=V per line); sets MD
  local t=$1; shift
  XENV="${MPX:-}" $ROOT/fixtures/motion-probe/drive.sh $RT $TAG-$t boot "$@" >/dev/null 2>&1; MD=$SIMCAM_RESULTS/motion-$RT-$TAG-$t
}
att_sum() { grep -m1 -oE 'ATT SUMMARY pass=[0-9]+ fail=[0-9]+' $1/app.log; }
c_motion-attitude() {   # attitude, quaternion, rotationMatrix, roll/pitch/yaw in portrait, upside-down and both landscapes: 24 checks each
  # The orientation comes from the control file's orientation= line, the line `stuntcam orient` writes (the CLI refuses a sim
  # the scripts armed); the engine reads it when the camera arms, so the probe makes one camera call first (-armcam 1).
  clear_sentinel $BM; local o d c=$SIMCAM_RESULTS/activation-$RT-$TAG-att-control.txt okn=0 r= e= s
  for o in portrait portraitUpsideDown landscapeLeft landscapeRight; do
    print -r -- "orientation=$o" > $c
    MPX="SIMCAM_CONTROL=$c"$'\n' mprobe att-$o -attitude 1 -armcam 1 -expect $o; d=$MD; s=$(att_sum $d)
    if [[ $s == 'ATT SUMMARY pass=24 fail=0' && $(cnt 'motion: hooked 16' $d/app.log) == 1 ]]; then okn=$((okn + 1))
      e="$e $o: $(grep -m1 -oE 'gravity=\([^)]*\) q=\([^)]*\) roll=[-0-9.]+ pitch=[-0-9.]+ yaw=[-0-9.]+' $d/app.log), $(grep -m1 -oE 'samples=[0-9]+ in 2 s' $d/app.log);"
    else r="$r [$o: ${s:-no summary} hooked=$(cnt 'motion: hooked 16' $d/app.log) $(grep 'ATT FAIL' $d/app.log | sed 's/.*ATT FAIL //' | cut -c1-120 | tr '\n' ';')]"; fi
  done
  rm -f $c
  (( okn == 4 )) && ok "4/4 orientations, 24/24 checks each (frames, pull start, rate, R = matrix(q) = matrix(angles), R*down = gravity, stable 2 s, magneticField, Apple's CMAttitude oracle, multiply, copy, coding as Apple's, M1: decoded attitudes are the stand-in, multiply/isEqual with dm.attitude):$e" || bad "$okn/4;$r"
  clear_sentinel $BM
}
c_motion-attitude-stock() {   # a latched-motion launch: the new selectors go through the forwarding traps and get stock answers
  clear_sentinel $BM; local now=$(date +%s) d0 d1
  MPX="M5_NO_MOTION=1"$'\n' mprobe atts0 -attitude stock; d0=$MD   # stock CoreMotion, no traps: the reference answers
  plant_marker $STATE/$BM/latched-motion-$BUILD motion $(( now + 7200 ))
  MPX= mprobe atts1 -attitude stock; d1=$MD
  local f0=$(grep -m1 -oE ' frames=[0-9]+' $d0/app.log) f1=$(grep -m1 -oE ' frames=[0-9]+' $d1/app.log) r0=$(grep -m1 -oE 'samples=0 frame=[0-9]+' $d0/app.log) r1=$(grep -m1 -oE 'samples=0 frame=[0-9]+' $d1/app.log)
  if [[ $(att_sum $d1) == 'ATT SUMMARY pass=3 fail=0' && $(cnt 'motion: hooked' $d1/app.log) == 0 && $(cnt '16 one-shot forwarding traps' $d1/app.log) == 1 && $(mstock $d1) == 1 &&
        -n $f0 && $f1 == $f0 && $f1 != ' frames=1' && -n $r0 && $r1 == $r0 ]]
  then ok "16 traps, one stock line; availableAttitudeReferenceFrames forwarded:$f1 (stock:$f0, the stand-in answers 1); reference-frame starts (queue and pull) delivered nothing, attitudeReferenceFrame $r1 as stock"
  else bad "$(att_sum $d1) hooked=$(cnt 'motion: hooked' $d1/app.log) traps=$(cnt '16 one-shot forwarding traps' $d1/app.log) lines=$(mstock $d1) frames trap:$f1 stock:$f0 '$r1' vs '$r0'"; fi
  clear_sentinel $BM
}
crash_ev() { since $1 $2 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' '; }
camera_ev_ok() { [[ $1 == (crash-providerx2|crash-activatex2|crash-activatex1 crash-providerx1)' ' ]]; }
orient_early_seq() {   # orient_early_seq <tag> <fault env N> <fault env N+1> <order> [K=V ...]: N, N+1 fault in camera code outside
  # upd (M16_ORIENT_FAULT=<variant>, M16_TRAP_FAULT=1), N+2 plain camera-then-motion. Sets OE_EV OE_F (fault lines of N, N+1)
  # OE_PIDS (their pids) OE_D3. The crash reports (by those pids) are counted only by the cases that want them (CR, CR_NO).
  local t=$1 v1=$2 v2=$3 o=$4; shift 4; local e=$(ev $BP) n0 rx='M16_[A-Z_]+: fault in [A-Za-z0-9]+ now \([^)]*\)'; clear_sentinel $BP; n0=$(nl $e)
  run plain $t-1 M5APP_ORDER=$o $v1 "$@"; local d1=$(res plain $t-1)
  run plain $t-2 M5APP_ORDER=$o $v2 "$@"; local d2=$(res plain $t-2)
  OE_EV=$(crash_ev $e $n0)
  OE_F="$(grep -m1 -oE $rx $d1/app.log | sed 's/^M16_[A-Z_]*: //'); $(grep -m1 -oE $rx $d2/app.log | sed 's/^M16_[A-Z_]*: //')"
  OE_PIDS="$(fault_pid $d1/app.log 'M16_[A-Z_]+: fault in') $(fault_pid $d2/app.log 'M16_[A-Z_]+: fault in')"
  run plain $t-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT=stock "$@"; OE_D3=$(res plain $t-3)
}
oe_reports() { local -a pp; pp=(${=OE_PIDS}); crash_reports m5app 30 $pp; OE_NP=${#pp}; }   # CR, CR_NO for OE_PIDS
c_orient-crash-main() {   # the first motion call on main: faults in M5OrientTrackStart (N) and M5OrientTrackRunMain before upd (N+1) are camera
  orient_early_seq ocm M16_ORIENT_FAULT=start M16_ORIENT_FAULT=runmain motion-main-camera-bg; local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $OE_D3/app.log) eng=0; oe_reports
  camera_ev_ok "$OE_EV" && [[ $OE_NP == 2 && $OE_F == 'fault in M5OrientTrackStart now (main thread); fault in M5OrientTrackRunMain now (main thread)' && $(cnt 'latched after' $OE_D3/dylib.log) == 1 && $disc == *=0 ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OE_PIDS): $OE_F; events $OE_EV(no motion), $CR crash reports; N+2 camera-latched in PREPARE 1 ($disc)" \
    "faults '$OE_F' (pids '$OE_PIDS') events '$OE_EV' reports=$CR; N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log) $disc"
  clear_sentinel $BP
}
c_mutation-L62() {   # M17_TRACKSTART_KEEP_KEY=1: M5OrientTrackStart keeps the motion key (motion.inc's clear deleted): orient-crash-main must catch it
  orient_early_seq l62 M16_ORIENT_FAULT=start M16_ORIENT_FAULT=runmain motion-main-camera-bg M17_TRACKSTART_KEEP_KEY=1
  if [[ $OE_EV == 'crash-motionx2 ' && $(cnt 'latched after' $OE_D3/dylib.log) == 0 ]]; then ok "mutant caught: '$OE_EV' ($OE_F), N+2 camera not latched (orient-crash-main wants provider/activate and a latched camera)"
  else bad "mutant not caught: events '$OE_EV' ($OE_F); N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log)"; fi
  clear_sentinel $BP
}
c_orient-crash-ensure() {   # the first camera call on main arriving with the motion key set: ActEnsure's tracker run is camera
  orient_early_seq oce M16_ORIENT_FAULT=ensure M16_ORIENT_FAULT=ensure camera-then-motion; local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $OE_D3/app.log) eng=0; oe_reports
  camera_ev_ok "$OE_EV" && [[ $OE_NP == 2 && $OE_F == 'fault in M5OrientTrackRunMain now (main thread); fault in M5OrientTrackRunMain now (main thread)' && $(cnt 'latched after' $OE_D3/dylib.log) == 1 && $disc == *=0 ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OE_PIDS): $OE_F; events $OE_EV(no motion), $CR crash reports; N+2 camera-latched ($disc)" \
    "faults '$OE_F' (pids '$OE_PIDS') events '$OE_EV' reports=$CR; N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log) $disc"
  clear_sentinel $BP
}
c_mutation-L63() {   # M17_ENSURE_KEEP_KEY=1: ActEnsure's main-thread tracker run keeps the key (lazy.inc's clear deleted): orient-crash-ensure must catch it
  orient_early_seq l63 M16_ORIENT_FAULT=ensure M16_ORIENT_FAULT=ensure camera-then-motion M17_ENSURE_KEEP_KEY=1
  if [[ $OE_EV == 'crash-motionx2 ' && $(cnt 'latched after' $OE_D3/dylib.log) == 0 ]]; then ok "mutant caught: '$OE_EV' ($OE_F), N+2 camera not latched (orient-crash-ensure wants provider/activate and a latched camera)"
  else bad "mutant not caught: events '$OE_EV' ($OE_F); N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log)"; fi
  clear_sentinel $BP
}
c_app-send-crash() {   # the app's first camera call from its CMMotionManager subclass's interval getter, which
  # the stand-in reads inside its motion scope; a fault in that call's trap (camera code, M16_TRAP_FAULT) must be camera, never motion
  orient_early_seq asc M16_TRAP_FAULT=1 M16_TRAP_FAULT=1 motion-subclass-camera; local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $OE_D3/app.log) eng=0; oe_reports
  camera_ev_ok "$OE_EV" && [[ $OE_NP == 2 && $OE_F == 'fault in ActEnsure now (main thread, feature key none); fault in ActEnsure now (main thread, feature key none)' && $(cnt 'latched after' $OE_D3/dylib.log) == 1 && $disc == *=0 ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OE_PIDS): $OE_F; events $OE_EV(no motion), $CR crash reports; N+2 camera-latched ($disc)" \
    "faults '$OE_F' (pids '$OE_PIDS') events '$OE_EV' reports=$CR; N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log) $disc"
  clear_sentinel $BP
}
c_mutation-L65() {   # M17_APP_SEND_KEEP_KEY=1: the app-answerable sends keep the motion key (that guard undone): app-send-crash must catch it
  orient_early_seq l65 M16_TRAP_FAULT=1 M16_TRAP_FAULT=1 motion-subclass-camera M17_APP_SEND_KEEP_KEY=1
  if [[ $OE_EV == 'crash-motionx2 ' && $OE_F == *'feature key motion'* && $(cnt 'latched after' $OE_D3/dylib.log) == 0 ]]; then ok "mutant caught: '$OE_EV' ($OE_F), N+2 camera not latched (app-send-crash wants provider/activate, key none, a latched camera)"
  else bad "mutant not caught: events '$OE_EV' ($OE_F); N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log)"; fi
  clear_sentinel $BP
}
pfrace_seq() {   # pfrace_seq <tag> [K=V ...]: plain writes preflight-fail (a real failure) paused mid-write for 15 s (K=V go to it);
  # meanwhile the canary (forced preflight failure: no marker of its own) runs PREPARE 1's prune. Sets PR_MK PR_UT PR_RM PR_W,
  # and PR_P1 (the canary got past PREPARE 1, its 'TIMING gate entered' line, while the write's file was
  # there before and after the canary's run: "tmp" (the temp file), "inplace" (L64: the marker itself), or what was missing)
  local t=$1; shift; local mk=$STATE/preflight-fail-$BUILD dc tb ta
  clear_sentinel $BP; clear_sentinel $BC
  bg_run plain $t-a M16_ACTIVATE_FAIL=preflight M16_PF_FAIL_REAL=1 M16_SLOW_MARKER_MS=15000 SIMCAM_ACTIVATE_TIMEOUT_MS=40000 M5APP_EXPECT=stock "$@"
  if wait_for $BGDIR/app.log 'M16_SLOW_MARKER_MS: marker write paused' 30; then PR_W=paused
    tb=$(pf_writing $mk)
    run canary $t-b SIMCAM_PF_BREAK=1; dc=$(res canary $t-b)
    ta=$(pf_writing $mk)
    PR_RM=$(cnt "removed expired marker .*preflight-fail-$BUILD" $dc/dylib.log)
    PR_P1="gate=$(cnt 'TIMING gate entered' $dc/dylib.log) before=$tb after=$ta"
  else PR_W=never; PR_RM=?; PR_P1=never; fi
  wait $BGPID
  PR_MK=$(nf $mk(N)); PR_UT=$(( $(mtime $mk) - $(date +%s) ))
}
pf_writing() {   # pf_writing <marker>: "tmp" if a temp file of it exists, "inplace" if only the marker, else "none"
  local -a tm; tm=(${1:h}/.${1:t}.tmp-<->(N))
  (( ${#tm} )) && print tmp || { [[ -f $1 ]] && print inplace || print none; }
}
c_pf-prune-race() {   # a preflight-fail marker another process is writing is never pruned as expired (temp file + rename)
  pfrace_seq prr
  if [[ $PR_W == paused && $PR_RM == 0 && $PR_MK == 1 && $PR_P1 == 'gate=1 before=tmp after=tmp' ]] && (( PR_UT > 86000 )); then ok "the canary got past PREPARE 1 (its gate line) while plain's temp file was there, before and after its run (15 s paused write): nothing pruned, the marker landed with until +$(( PR_UT / 3600 )) h"
  else bad "write $PR_W, canary $PR_P1, prune lines=$PR_RM, marker=$PR_MK until-now=${PR_UT}s"; fi
  clear_sentinel $BP; clear_sentinel $BC
}
c_pf-prune-kept() {   # the prune's "kept" branch: a fresh marker renamed over the expired one after PREPARE 1's stat, before
  # its rename aside, is put back, not unlinked. Plain prunes another build's expired preflight-fail, paused 4 s before the
  # rename (M16_PRUNE_PAUSE_MS); in the pause the host renames a fresh one (until +1 h) over it, as SCWriteMarker does.
  clear_sentinel $BP; local now=$(date +%s) d w=never m=$STATE/preflight-fail-0000cafef00d tmp=$STATE/.preflight-fail-0000cafef00d.tmp-host
  plant_marker $m "preflight failed" $(( now - 3600 ))
  bg_run plain pfk M16_PRUNE_PAUSE_MS=4000 SIMCAM_ACTIVATE_TIMEOUT_MS=40000; d=$BGDIR
  if wait_for $d/app.log 'M16_PRUNE_PAUSE_MS: prune of .*preflight-fail-0000cafef00d paused' 30; then
    plant_marker $tmp "preflight failed (fresh)" $(( now + 3600 )); mv -f $tmp $m; w=replaced; fi
  wait $BGPID
  local ut=$(( $(mtime $m) - now )) kept=$(cnt 're-written while being pruned: kept' $d/dylib.log) rm=$(cnt 'removed expired marker .*preflight-fail-0000cafef00d' $d/dylib.log)
  local c=$(head -1 $m 2>/dev/null) left=$(nf $STATE/.preflight-fail-0000cafef00d.tmp-prune-*(N))
  if [[ $w == replaced && $kept == 1 && $rm == 0 && $c == "preflight failed (fresh)" && $ut == 3600 && $left == 0 ]]; then ok "the fresh marker renamed in during the paused prune was put back: 1 'kept' line, none removed, content '$c', until +$ut s, no temp left"
  else bad "host write $w; kept lines=$kept removed=$rm; marker '$c' until-now=${ut}s; aside files left=$left"; fi
  rm -f $m $tmp; clear_sentinel $BP
}
c_mutation-L64() {   # M17_MARKER_INPLACE=1: markers written in place again (open O_TRUNC, then futimens): pf-prune-race must catch it
  pfrace_seq l64 M17_MARKER_INPLACE=1
  if [[ $PR_W == paused && $PR_RM == 1 && $PR_MK == 0 && $PR_P1 == 'gate=1 before=inplace after=none' ]]; then ok "mutant caught: the canary pruned the marker plain was writing in place (1 'removed expired marker' line, no marker after; $PR_P1)"
  else bad "mutant not caught: write $PR_W, canary $PR_P1, prune lines=$PR_RM, marker=$PR_MK until-now=${PR_UT}s"; fi
  clear_sentinel $BP; clear_sentinel $BC
}

# ---------------------------------------------------------------- op trace file, deferred-stop flush
# The op trace's file (core/trace.inc; ledger L69-L71) and a start's flush of a deferred stop (fig/session_state.inc; L73).
# Traces are read with the CLI (`stuntcam trace --json --pid`), the parser `stuntcam trace` users get.
SCLI=$ROOT/cli/bin/stuntcam
trj() { $SCLI trace --json --udid $U --pid $1 $BP > $2 2>$2.err; }   # trj <pid> <out.json>: rc of the CLI
trsum() {   # trsum <json>: "<location> <n ops> <n crash ops> <ms from activate to the first crash op | -> <ops joined by ,>"
  python3 -I -c '
import json, sys
r = json.load(open(sys.argv[1])); o = r["ops"]; n = [x["op"] for x in o]
a = [x for x in o if x["op"] == "activate"]; c = [x for x in o if x["op"] == "crash"]
ms = "%.1f" % ((c[0]["wall_ns"] - a[0]["wall_ns"]) / 1e6) if a and c else "-"
print(r["location"], len(n), n.count("crash"), ms, ",".join(n))' $1 2>/dev/null || echo "unparseable"
}
lpid() { grep -m1 -oE '\[simcam [0-9]+\]' $1 2>/dev/null | grep -oE '[0-9]+'; }   # lpid <app.log>: the launch's pid
# trace_crash <tag> <M16_ACTIVATE_FAIL value> [K=V ...]: one plain launch that faults in START; sets TC_PID, TC_RC (CLI),
# TC_SUM (trsum), TC_EV (crash lines in events.log since), TC_TMP (temp files of that pid left)
trace_crash() {
  local tag=$1 f=$2; shift 2
  clear_sentinel $BP; local e=$(ev $BP) n0; n0=$(nl $e)
  run plain $tag M16_ACTIVATE_FAIL=$f "$@"; sleep 1; local d=$(res plain $tag)
  TC_PID=$(fault_pid $d/app.log 'M16_ACTIVATE_FAIL=') TC_EV=$(since $e $n0 | grep -c ' crash')
  [[ -n $TC_PID ]] || { TC_RC=x TC_SUM="no fault line in $d/app.log" TC_TMP=0; clear_sentinel $BP; return; }
  trj $TC_PID $d/trace.json; TC_RC=$?; TC_SUM=$(trsum $d/trace.json)
  TC_TMP=$(nf $STATE/$BP/.trace-$TC_PID.*(N) $(tmpdir $BP)/.trace-$TC_PID.*(N))
  clear_sentinel $BP
}
c_trace-crash-start() {   # L69: a crash in START (before the first 150 ms flush) leaves the trace, from the handler
  trace_crash tcs crashstart
  local -a w; w=(${=TC_SUM})
  [[ $TC_RC == 0 && ${w[3]} == 1 && $TC_SUM == *activate*crash* && $TC_EV == 1 && $TC_TMP == 0 ]] && ok "pid $TC_PID: trace written by the crash handler, parsed by the CLI: ${w[1]}, ${w[2]} ops, crash ${w[4]} ms after activate (${w[5]}); 1 crash line" \
    || bad "pid ${TC_PID:-?}: CLI rc=$TC_RC trace '$TC_SUM' crash lines=$TC_EV temp files left=$TC_TMP ($(head -c 200 "$(res plain tcs)/trace.json.err" 2>/dev/null))"
}
c_mutation-L69() {   # M54_LATE_TRACE_DIR=1: the directory picked at the first flush again: the same crash leaves no trace
  trace_crash tcl crashstart M54_LATE_TRACE_DIR=1
  [[ $TC_RC != 0 && $TC_EV == 1 ]] && ok "mutant caught: pid $TC_PID crashed (1 crash line) and left no trace (CLI rc=$TC_RC: $(head -c 120 "$(res plain tcl)/trace.json.err" 2>/dev/null))" \
    || bad "mutant not caught: pid ${TC_PID:-?} CLI rc=$TC_RC trace '$TC_SUM' crash lines=$TC_EV"
}
# crash2_seq <tag> [K=V ...]: up to 6 crash2 launches, until 3 had both threads in the handler (2 crash lines; under heavy
# load the second thread may not run before the process ends: such a launch counts only for the whole-trace check). With
# C2_STOP_BAD=1 (the mutant): up to 10 launches, until one with both threads in the handler leaves no whole trace (C2_CAUGHT:
# the fault line was found, so the CLI ran, and 2 crash lines; a launch without them that isn't whole is no catch, it can't
# have raced). Sets C2_N, C2_BOTH, C2_WHOLE, C2_CAUGHT, C2_DET
crash2_seq() {
  local tag=$1 i mut=${C2_STOP_BAD:-0}; shift; C2_N=0 C2_BOTH=0 C2_WHOLE=0 C2_CAUGHT=0 C2_DET=
  for i in {1..$(( mut ? 10 : 6 ))}; do
    (( ! mut && C2_BOTH >= 3 )) && break
    trace_crash $tag-$i crash2 "$@"; C2_N=$((C2_N + 1))
    local -a w; w=(${=TC_SUM})
    local good=0; [[ $TC_RC == 0 && ${w[3]:-0} -ge 1 && $TC_TMP == 0 ]] && good=1 C2_WHOLE=$((C2_WHOLE + 1))
    [[ $TC_EV == 2 ]] && C2_BOTH=$((C2_BOTH + 1))
    C2_DET="$C2_DET [pid $TC_PID rc=$TC_RC crash lines=$TC_EV crash ops=${w[3]:-?} ops=${w[2]:-?} tmp=$TC_TMP]"
    (( mut && ! good )) && [[ $TC_RC != x && $TC_EV == 2 ]] && { C2_CAUGHT=1; break; }
  done
}
c_trace-crash2() {   # L70: two threads in the crash handler at once: one whole trace (parsed by the CLI). Needs 3 launches in
  # which both threads ran the handler, at most 6 launches; any launch without a whole trace FAILs
  crash2_seq tc2
  if (( C2_WHOLE == C2_N && C2_BOTH >= 3 )); then ok "$C2_N launches, $C2_BOTH with both threads in the handler, a whole trace in every one$C2_DET"
  elif (( C2_WHOLE == C2_N )); then unsure "a whole trace in all $C2_N launches, but both threads reached the handler in only $C2_BOTH$C2_DET"
  else bad "$C2_WHOLE of $C2_N launches left a whole trace$C2_DET"; fi
}
c_mutation-L70() {   # M54_OLD_CRASH=1: no CAS, both threads format into one buffer and write one temp file, 20 ms apart:
  # trace-crash2's check must see a launch without a whole trace (torn, empty, unparseable or a temp file left). A launch
  # tears about half the time (6 of 13 in one run), so up to 10 launches (a miss ~0.1%)
  # A catch is a launch with both threads in the handler (2 crash lines) and no whole trace; see crash2_seq
  C2_STOP_BAD=1 crash2_seq tc2m M54_OLD_CRASH=1
  if (( C2_CAUGHT )); then ok "mutant caught: launch $C2_N of up to 10, both threads in the handler, left no whole trace ($C2_BOTH with both threads)$C2_DET"
  elif (( C2_BOTH == 0 )); then unsure "mutant not seen: the second thread never reached the handler in $C2_N launches$C2_DET"
  else bad "mutant not caught: every launch with both threads in the handler left a whole trace ($C2_WHOLE of $C2_N whole, $C2_BOTH with both threads)$C2_DET"; fi
}
# repick_seq <tag> <removed|unwritable> [K=V ...]: plain takes 2 photos, holds 5 s; at HOLD the state dir is removed (or made
# mode 555); then it stops (session-stop op) and exits. Sets RP_PID, RP_RC, RP_SUM, RP_LINES (re-pick log lines), RP_BEFORE
repick_seq() {
  local tag=$1 how=$2; shift 2; clear_sentinel $BP
  bg_run plain $tag M5APP_PHOTOS=2 M5APP_HOLD=5 "$@"; local d=$BGDIR
  wait_for $d/app.log 'HOLD' 40; RP_PID=$(lpid $d/app.log); sleep 0.5
  RP_BEFORE=$([[ -f $STATE/$BP/trace-$RP_PID.txt ]] && echo yes || echo no)
  # mode 555 is undone on every path out of this block (zsh `always`; an EXIT trap set in a function would fire at its return,
  # and an INT/TERM trap that doesn't exit would let Ctrl-C go on): Ctrl-C / TERM exit the script (130 / 143) through it
  {
    trap 'exit 130' INT; trap 'exit 143' TERM
    if [[ $how == removed ]]; then rm -rf "${STATE:?}/${BP:?}"; else chmod 555 "$STATE/$BP"; fi
    wait $BGPID; sleep 1
  } always { [[ $how == unwritable ]] && chmod 755 "${STATE:?}/${BP:?}" 2>/dev/null; trap - INT TERM; }
  trj $RP_PID $d/trace.json; RP_RC=$?; RP_SUM=$(trsum $d/trace.json)
  RP_LINES=$(cnt 'trace: writing .* failed' $d/dylib.log); RP_LINE=$(grep -m1 -oE 'trace: writing .* failed.*' $d/dylib.log)
  clear_sentinel $BP
}
c_trace-repick() {   # L71: the state dir removed mid-run: the next write re-picks (the dir is made again) and logs it once
  repick_seq rp removed
  local -a w; w=(${=RP_SUM})
  [[ $RP_BEFORE == yes && $RP_RC == 0 && ${w[1]} == state-dir && $RP_SUM == *session-stop* && $RP_LINES == 1 && $RP_LINE == *re-picked* ]] \
    && ok "pid $RP_PID: trace there before, dir removed at HOLD; after exit ${w[2]} ops incl. session-stop in the state dir again; 1 line: $RP_LINE" \
    || bad "pid ${RP_PID:-?}: before=$RP_BEFORE CLI rc=$RP_RC trace '${RP_SUM:0:200}' lines=$RP_LINES '$RP_LINE'"
}
c_trace-unwritable() {   # L71: the state dir made mode 555 mid-run: re-picked into the container-tmp fallback
  repick_seq ru unwritable
  local -a w; w=(${=RP_SUM})
  [[ $RP_BEFORE == yes && $RP_RC == 0 && ${w[1]} == container-tmp && $RP_SUM == *session-stop* && $RP_LINES == 1 && $RP_LINE == *container-tmp* ]] \
    && ok "pid $RP_PID: after exit the newest trace is the container-tmp one, ${w[2]} ops incl. session-stop; 1 line: $RP_LINE" \
    || bad "pid ${RP_PID:-?}: before=$RP_BEFORE CLI rc=$RP_RC trace '${RP_SUM:0:200}' lines=$RP_LINES '$RP_LINE'"
}
c_mutation-L71() {   # M54_NO_REPICK=1: no re-pick: after the removal no trace is written any more (the dir stays gone)
  repick_seq rpm removed M54_NO_REPICK=1
  [[ $RP_BEFORE == yes && $RP_RC != 0 && $RP_LINES == 0 ]] && ok "mutant caught: pid $RP_PID's trace is gone after the removal and never written again (CLI rc=$RP_RC)" \
    || bad "mutant not caught: before=$RP_BEFORE CLI rc=$RP_RC trace '${RP_SUM:0:160}' lines=$RP_LINES"
}
# flush_seq <tag> [K=V ...]: a photo, then stopRunning on plain's thread with the callback 300 ms late, and a start's flush
# forced into the window between M40BeginStop and the waiter's dispatch (M54_FLUSH_RACE); FR_LINE = the engine's verdict line
flush_seq() { local tag=$1; shift; run plain $tag M5APP_PHOTOSTOP=bg M40_FINISH_DELAY_MS=300 M54_FLUSH_RACE=1 "$@"; FR_LINE=$(grep -m1 -oE 'M54_FLUSH_RACE: .*' "$(res plain $tag)/dylib.log" | cut -c1-200); }
c_flush-race() {   # L73: the flush returns only after the stop's DidStopRunning is queued
  flush_seq fr
  [[ $FR_LINE == *"outstanding=1"*"was queued first (in order)"* ]] && ok "$FR_LINE" || bad "${FR_LINE:-no M54_FLUSH_RACE line}"
}
c_mutation-L73() {   # M54_OLD_FLUSH=1: the unfixed dispatch_sync returns before the waiter is queued
  flush_seq frm M54_OLD_FLUSH=1
  [[ $FR_LINE == *"NOT queued yet (out of order)"* ]] && ok "mutant caught: $FR_LINE" || bad "mutant not caught: ${FR_LINE:-no M54_FLUSH_RACE line}"
}
# late_seq <tag> [K=V ...] (L74): flush-race's flush, but the stop's waiter starts 650 ms late (M59_LATE_WAITER), as if
# starved of CPU: the flush gives up at kM40WaitS (500 ms) and the start would go ahead; the photo's didFinishCapture is
# 2 s late, so it is still due when the waiter runs. Sets LW_LATE (the fault's line), LW_TO (the flush's timeout line),
# LW_FR (the M54_FLUSH_RACE line), LW_W (the waiter's verdict line), LW_MS (its wait in ms, or -), LW_STOP (plain's stop line)
late_seq() {
  local tag=$1; shift
  run plain $tag M5APP_PHOTOSTOP=bg M40_FINISH_DELAY_MS=2000 M54_FLUSH_RACE=1 M59_LATE_WAITER=650 "$@"; local d=$(res plain $tag)
  LW_LATE=$(grep -m1 -oE 'M59_LATE_WAITER: .*' $d/dylib.log | cut -c1-120)
  LW_TO=$(grep -m1 -oE 'StartRunning: [0-9]+ deferred stop\(s\) still outstanding after .*' $d/dylib.log | cut -c1-200)
  LW_FR=$(grep -m1 -oE 'M54_FLUSH_RACE: .*' $d/dylib.log | cut -c1-200)
  LW_W=$(grep -m1 -oE 'StopRunning: (flushed by a start|in-flight photo callbacks done|[0-9]+ photo\(s\) without didFinishCapture) .*' $d/dylib.log | cut -c1-160)
  LW_MS=$(print -r -- "$LW_W" | grep -oE 'flushed by a start after [0-9.]+' | grep -oE '[0-9.]+$'); LW_MS=${LW_MS:--}
  LW_STOP=$(grep -m1 -oE 'photostop.stop[^|]*stopRunning took [0-9]+ ms' $d/app.log)
}
c_late-waiter() {   # L74: the flush timed out with the mark kept: the late waiter ends at once when it runs (< 50 ms), its photo still due
  late_seq lw
  if [[ -n $LW_LATE && $LW_TO == *"flush mark kept"* && $LW_FR == *"outstanding=1"* && $LW_W == *"flushed by a start"*"still due"* && $LW_MS != - ]] && (( LW_MS < 50 )); then
    ok "$LW_LATE; flush: $LW_TO; the waiter: $LW_W; plain: ${LW_STOP:-no stop line}"
  else bad "late=${LW_LATE:-none} | flush: ${LW_TO:-no timeout line} | race: ${LW_FR:-none} | waiter: ${LW_W:-none} | plain: ${LW_STOP:-none}"; fi
}
c_mutation-L74() {   # M59_OLD_FLUSHMARK=1: the mark cleared on timeout again: the late waiter waits for its photo again
  late_seq lwm M59_OLD_FLUSHMARK=1
  if [[ $LW_TO == *"M59_OLD_FLUSHMARK"* && -n $LW_W && $LW_W != *"flushed by a start"* ]]; then ok "mutant caught: the late waiter wasn't flushed: $LW_W; plain: ${LW_STOP:-no stop line}"
  else bad "mutant not caught: flush: ${LW_TO:-no timeout line} | waiter: ${LW_W:-none}"; fi
}

# ---------------------------------------------------------------- driver
for c in $CASES; do
  fn=c_$c; (( $+functions[$fn] )) || { echo "FAIL $c — unknown case" | tee -a $OUTF; fails=$((fails + 1)); continue; }
  t0=$SECONDS; CASE_START=$(date +%s); $fn; v1=$VERDICT; e1=$EVID
  if [[ $v1 == FAIL && ${TIMING[(Ie)$c]} -gt 0 ]]; then
    la=$(sysctl -n vm.loadavg | tr -d '{}'); CASE_START=$(date +%s); $fn
    line="$VERDICT $c — rerun after a FAIL (load avg$la): first: $e1 | rerun: $EVID"
  else line="$v1 $c — $e1"; fi
  [[ ${line%% *} == FAIL ]] && fails=$((fails + 1)); [[ ${line%% *} == UNSURE ]] && unsures=$((unsures + 1))
  echo "$line ($(( SECONDS - t0 )) s)" | tee -a $OUTF
done
(( fails )) && { echo "$fails case(s) failed, $unsures UNSURE (results: $OUTF)"; exit 1; }
(( unsures )) && echo "$(( ${#CASES} - unsures )) of ${#CASES} cases passed, $unsures UNSURE, none failed (results: $OUTF)" || echo "all ${#CASES} cases passed (results: $OUTF)"
