#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# bgfg.sh <rt> [behaviour ...] [-n N] — does the engine stay in step with the app's own startRunning/stopRunning across
#   background and foreground? [L52] (w3 e20 measured the bug, e21 added this guard)
#   On a sim armed by scripts/boot.sh <rt> stub, the bg/fg probe (fixtures/bgfg-probe, `-mode <behaviour>`) runs N times
#   (default 1) per behaviour (default all: stopInBackground keepRunning stopWhileInterrupted appNotif; see its README).
#   Each run launches the probe, takes it to the background at its BG-REQUEST (`simctl launch com.apple.Preferences`) and
#   back after BGS s (default 6: on 27.0 didEnterBackground came 2.2–4.8 s after Preferences launched, e20) with
#   `simctl launch <probe>`, waits for its END, and checks the probe log against the engine log (dylib-<pid>.log):
#     (a) every startRunning/stopRunning the probe makes reaches the engine: matched in order, each CALL to an engine
#         "session … StartRunning" / "StopRunning" line logged between that CALL and its RET (or the next CALL);
#     (b) the engine never restarts a session the app stopped: no "m12 interruption ended session …" whose last app call
#         before it was a stopRunning;
#     (c) after the probe's final stopRunning the engine has stopped that session: the session's last engine Start/Stop/
#         interruption-ended line is the StopRunning that call made, and no "interruption ended" follows it.
#   Also required: the probe reached END, was backgrounded and foregrounded (didEnterBackground, willEnterForeground), and
#   left an engine log; for stopWhileInterrupted and appNotif, that an app stop reached the engine while it had the session
#   interrupted (else the run didn't exercise L52: "not exercised"); no more redundant calls than the behaviour makes; and
#   the probe's CPU over 2 s after END under 0.1 s (an engine still pumping uses 0.28-0.36 s; stopped: <= 0.01 s, e21).
#   The foreground comes BGS s after the background, and for those two behaviours not before the probe's background stop
#   has returned (up to 15 s more), so a slow didEnterBackground can't put the stop after the interruption's end.
#   Prints one PASS/FAIL line per behaviour and repetition with the reason; exits 1 if any failed (2 on a usage error).
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to the probe launch, as scripts/run.sh does (e.g. XENV=M14_OLD_ENDINT=1).
#   UDID: $SIMCAM_UDID_<rt>. Needs fixtures/bgfg-probe/build/bgfgprobe.app (fixtures/bgfg-probe/build.sh); installed here.
#   Writes $SIMCAM_RESULTS/bgfg-<rt>-<behaviour>-<tag>-r<i>/: app.log, host.log, dylib.log, timeline.txt, check.txt.
set -u; zmodload zsh/datetime
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh
(( $# )) || { echo "usage: $0 <rt> [behaviour ...] [-n N]" >&2; exit 2; }
RT=$1; shift; N=1; typeset -a MODES; MODES=()
while (( $# )); do case $1 in -n) (( $# > 1 )) || { echo "$0: -n needs a count" >&2; exit 2; }; N=$2; shift 2;; -n*) N=${1#-n}; shift;; *) MODES+=($1); shift;; esac; done
[[ $N == <1-> ]] || { echo "$0: -n needs a positive count (got $N)" >&2; exit 2; }
(( ${#MODES} )) || MODES=(stopInBackground keepRunning stopWhileInterrupted appNotif)
for m in $MODES; do [[ $m == (stopInBackground|keepRunning|stopWhileInterrupted|appNotif) ]] || { echo "$0: unknown behaviour $m" >&2; exit 2; }; done
V=SIMCAM_UDID_$RT; UDID=${(P)V:-}; [[ -n $UDID ]] || { echo "$0: set $V to the UDID of your iOS $RT simulator" >&2; exit 2; }
BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null); [[ -n $BL && -d $BL ]] || { echo "$0: no boot log dir: boot with scripts/boot.sh $RT stub first" >&2; exit 2; }
B=dev.simcam.e20.bgfgprobe; APP=$ROOT/fixtures/bgfg-probe/build/bgfgprobe.app; BGS=${BGS:-6}; TAG=bgfg-$(date +%H%M%S)
[[ -d $APP ]] || { echo "$0: $APP missing: run fixtures/bgfg-probe/build.sh" >&2; exit 2; }
xcrun simctl install $UDID $APP || { echo "$0: installing the probe failed" >&2; exit 2; }
typeset -a ENVS; ENVS=(); for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done

# check.py <run dir>: the three assertions; prints "PASS <info>" or "FAIL <reason>"
CHECK=$(cat <<'PY'
import os, re, sys
d, mode = sys.argv[1], sys.argv[2]
def rd(n):
    p = os.path.join(d, n)
    return open(p, errors='replace').read().splitlines() if os.path.exists(p) else []
app = []
for l in rd('app.log'):
    m = re.match(r'.*?PROBE\| (\d+\.\d+)\s+\S+ (.*)', l)
    if m: app.append((float(m.group(1)), m.group(2)))
eng = []
for l in rd('dylib.log'):
    m = re.match(r'(\d{9}\.\d{3}) \[simcam \d+\] (.*)', l)
    if m: eng.append((float(m.group(1)), m.group(2)))
# the engine log is appended to and named by pid: drop lines from an earlier process with the same pid
if app: eng = [(t, x) for t, x in eng if t >= app[0][0] - 1.0]
def has(s): return any(s in x for _, x in app)
miss = [w for w, s in (('END', 'END |'), ('didEnterBackground', 'SCENE didEnterBackground')) if not has(s)]
# the scene's first willEnterForeground is at launch; the second is the return from the background
if sum('SCENE willEnterForeground' in x for _, x in app) < 2: miss.append('willEnterForeground after the background')
if not eng: miss.append('engine log')
if miss: print('FAIL setup: no ' + ', '.join(miss)); sys.exit()
# the probe's calls: (t_call, kind, why, t_ret or None)
calls = []
for i, (t, x) in enumerate(app):
    m = re.match(r'CALL (start|stop)Running \((.*?)\) \| before: (.*)', x)
    if m: calls.append([t, m.group(1), m.group(2), None, m.group(3)])
    m = re.match(r'RET  (start|stop)Running \((.*?)\)', x)
    if m:
        for c in reversed(calls):
            if c[1] == m.group(1) and c[2] == m.group(2) and c[3] is None: c[3] = t; break
t_end = next(t for t, x in app if x.startswith('END |'))
# engine session events: (t, kind, session)
ev = []
for t, x in eng:
    m = re.match(r'session (0x[0-9a-f]+) (Start|Stop)Running', x)
    if m: ev.append((t, m.group(2).lower(), m.group(1))); continue
    m = re.match(r'm12 interruption ended session (0x[0-9a-f]+)', x)
    if m: ev.append((t, 'ended', m.group(1))); continue
    m = re.match(r'm12 interrupt session (0x[0-9a-f]+)', x)
    if m: ev.append((t, 'interrupt', m.group(1)))
EPS = 0.005
fails = []
# (a) in order: each call takes the first unconsumed engine Start/Stop line at or after it; it must be of its kind and come
# before the call returned (or before the next call, if it never returned)
# A call of the same kind as the app's previous one is redundant (the probe's launch-time start in the first
# willEnterForeground, while already running): AVF drops a start on a running session itself (e20 §5), so it need not
# reach the engine. A call that changes what the app asked for must, whatever isRunning reads (after an unasked restart,
# AVF reads isRunning=1 and drops the app's start: that is the bug).
runs = [e for e in ev if e[1] in ('start', 'stop')]; j = 0; sess = None; redundant = 0
for k, (t, kind, why, tr, before) in enumerate(calls):
    dl = (tr + 0.05) if tr is not None else (calls[k + 1][0] if k + 1 < len(calls) else t_end)
    while j < len(runs) and runs[j][0] < t - EPS: j += 1
    if j < len(runs) and runs[j][1] == kind and runs[j][0] <= dl:
        sess = sess or runs[j][2]; j += 1
    elif k > 0 and calls[k - 1][1] == kind:
        redundant += 1
    else:
        nxt = f'next engine line {runs[j][1]} at {runs[j][0] - t:+.3f} s' if j < len(runs) else 'no later engine Start/Stop line'
        fails.append(f'(a) {kind}Running ({why}) at {t - app[0][0]:.3f} s [before: {before}] did not reach the engine ({nxt})')
sess = sess or (runs[0][2] if runs else None)
# (b) an interruption end for a session whose last app call before it was a stop. "Before" is the engine log's line order
# (one file, written in order), not the clock: the probe's CALL and the engine's line often share a millisecond (e21: on
# 27.0 the app's foreground start reached the engine 1 ms after the restart, on 26.3 never)
for i, (t, kind, s) in enumerate(ev):
    if kind != 'ended' or (sess and s != sess): continue
    prev = [e for e in ev[:i] if e[1] in ('start', 'stop') and e[2] == s]
    if prev and prev[-1][1] == 'stop':
        why = next((c[2] for c in reversed(calls) if c[1] == 'stop' and c[0] <= prev[-1][0] + EPS), '?')
        fails.append(f'(b) engine restarted session {s} at {t - app[0][0]:.3f} s ("interruption ended") although the app\'s last call was stopRunning ({why}) at {prev[-1][0] - app[0][0]:.3f} s')
# (c) after the final stop the engine has that session stopped
if not calls or calls[-1][1] != 'stop':
    fails.append('(c) the probe\'s last call was not a stopRunning')
else:
    tf = calls[-1][0]; mine = [e for e in ev if e[1] in ('start', 'stop', 'ended') and (not sess or e[2] == sess)]
    if not mine or mine[-1][1] != 'stop' or mine[-1][0] < tf - EPS:
        last = f'{ {"start": "StartRunning", "stop": "StopRunning", "ended": "interruption ended"}[mine[-1][1]]} at {mine[-1][0] - app[0][0]:.3f} s' if mine else 'none'
        fails.append(f'(c) after the final stopRunning ({calls[-1][2]}) at {tf - app[0][0]:.3f} s the session\'s last engine line is {last}, not that StopRunning')
    late = [e for e in mine if e[1] == 'ended' and e[0] >= tf - EPS]
    if late: fails.append(f'(c) "interruption ended" after the final stopRunning at {late[0][0] - app[0][0]:.3f} s')
# L52 exercised: an engine StopRunning (from the app) while the session was interrupted, i.e. after an interrupt line with no
# "interruption ended" in between (engine log order). Or just before the interrupt line: the engine logs StopRunning on entry,
# then waits for the pump queue, where the interrupt may already be queued (appNotif's stop in didEnterBackground, e21b); a
# session whose stop had already run isn't running, so it isn't interrupted: the interrupt line proves the stop ran during it
if mode in ('stopWhileInterrupted', 'appNotif'):
    state, hit = None, False
    for t, kind, s in ev:
        if sess and s != sess: continue
        if (kind, state) in (('stop', 'interrupt'), ('interrupt', 'stop')): hit = True; break
        state = kind if kind in ('interrupt', 'stop') else None
    if not hit: fails.append('not exercised: no app stopRunning reached the engine while it had the session interrupted')
# the only redundant call a behaviour makes is the launch-time start (stopInBackground, appNotif); more would hide a dropped call
expect = {'stopInBackground': 1, 'keepRunning': 0, 'stopWhileInterrupted': 0, 'appNotif': 1}.get(mode, 0)
if redundant > expect: fails.append(f'{redundant} redundant calls not forwarded (this behaviour makes {expect})')
info = f'calls={len(calls)} (redundant, not forwarded: {redundant}) engine start/stop={len(runs)} interrupts={sum(e[1] == "interrupt" for e in ev)} ended={sum(e[1] == "ended" for e in ev)}'
print(('FAIL ' + '; '.join(fails) + ' | ' + info) if fails else 'PASS ' + info)
PY
)

hlog() { printf 'HOST| %.3f %s\n' $(( EPOCHREALTIME - 978307200 )) "$*" >> $OUT/host.log; }
fails=0; total=0
for m in $MODES; do for i in {1..$N}; do
  OUT=$SIMCAM_RESULTS/bgfg-$RT-$m-$TAG-r$i; rm -rf $OUT; mkdir -p $OUT; total=$((total + 1))
  xcrun simctl terminate $UDID $B 2>/dev/null
  xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
  hlog "launch $B -mode $m ${XENV:-}"
  env $ENVS xcrun simctl launch --console-pty $UDID $B -mode $m > $OUT/app.log 2>&1 &
  LP=$!; BG=0
  for k in {1..600}; do sleep 0.1
    grep -q "END |" $OUT/app.log && break
    kill -0 $LP 2>/dev/null || { hlog "console closed (app gone)"; break; }
    if [[ $BG == 0 ]] && grep -q "BG-REQUEST" $OUT/app.log; then BG=1
      hlog "simctl launch com.apple.Preferences"; xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1
      sleep $BGS
      if [[ $m == (stopWhileInterrupted|appNotif) ]]; then   # not before the background stop has returned
        for w in {1..150}; do awk '/SCENE didEnterBackground/{b=1} b&&/RET  stopRunning/{f=1} END{exit !f}' $OUT/app.log && break; sleep 0.1; done
      fi
      hlog "simctl launch $B (foreground)"; xcrun simctl launch $UDID $B >/dev/null 2>&1
    fi
  done
  grep -q "END |" $OUT/app.log || hlog "no END after 60 s"
  # a late restart or pump shows after END: wait 2 s (the engine log keeps it), and read the probe's CPU over that time
  PID=$(grep -oE 'pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2); CPU=?
  if [[ -n $PID ]] && kill -0 $PID 2>/dev/null; then
    cpu() { ps -o time= -p $PID | awk -F'[:.]' '{print ($1*60+$2)+("0."$3)}'; }
    C1=$(cpu); sleep 2; C2=$(cpu); [[ -n $C1 && -n $C2 ]] && CPU=$(printf '%.2f' $(( C2 - C1 ))); hlog "after END: probe CPU $CPU s in 2 s"
  else sleep 2; fi
  kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null; hlog "terminated"
  if [[ -n $PID && -f $BL/dylib-$PID.log ]]; then cp $BL/dylib-$PID.log $OUT/dylib.log; else : > $OUT/dylib.log; fi
  python3 -I $ROOT/fixtures/bgfg-probe/merge.py $OUT > $OUT/timeline.txt 2>/dev/null
  R=$(python3 -I -c "$CHECK" $OUT $m 2>&1 | tail -1); [[ $R == PASS* || $R == FAIL* ]] || R="FAIL checker: $R"
  if [[ $CPU != '?' ]] && (( CPU > 0.1 )); then   # the engine still pumping after the app's last stop (L52's symptom)
    [[ $R == PASS* ]] && R="FAIL probe CPU ${CPU} s in 2 s after END (engine still pumping?) | ${R#PASS }" || R="$R; probe CPU ${CPU} s after END"
  fi
  echo "$R" > $OUT/check.txt
  [[ $R == PASS* ]] || fails=$((fails + 1))
  printf '%s %s %s r%d%s :: %s cpu-after-END=%ss\n' ${R%% *} $RT $m $i "${XENV:+ ($XENV)}" "${R#* }" $CPU
done; done
echo "bgfg $RT: $((total - fails))/$total PASS${XENV:+ ($XENV)} (results: $SIMCAM_RESULTS/bgfg-$RT-*-$TAG-r*)"
(( fails )) && exit 1 || exit 0
