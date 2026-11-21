# SPDX-License-Identifier: Apache-2.0
# bgfg-check.py <run dir> <behaviour>: scripts/bgfg.sh's check of one bg/fg probe run (its app.log against the engine's
# dylib.log): the three assertions of bgfg.sh's header and the run's other requirements. Prints "PASS <info>" or
# "FAIL <reason>".
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
    m = re.match(r'interruption ended session (0x[0-9a-f]+)', x)
    if m: ev.append((t, 'ended', m.group(1))); continue
    m = re.match(r'interrupt session (0x[0-9a-f]+)', x)
    if m: ev.append((t, 'interrupt', m.group(1)))
EPS = 0.005
fails = []
# (a) in order: each call takes the first unconsumed engine Start/Stop line at or after it; it must be of its kind and come
# before the call returned (or before the next call, if it never returned)
# A call of the same kind as the app's previous one is redundant (the probe's launch-time start in the first
# willEnterForeground, while already running): AVF drops a start on a running session itself, so it need not
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
# (one file, written in order), not the clock: the probe's CALL and the engine's line often share a millisecond (seen: on
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
# then waits for the pump queue, where the interrupt may already be queued (appNotif's stop in didEnterBackground); a
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
