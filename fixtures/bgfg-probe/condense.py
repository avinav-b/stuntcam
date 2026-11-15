# SPDX-License-Identifier: Apache-2.0
# condense.py <results dir> — one row per run: the facts needed to compare the runs
import os, re, sys, glob
R = sys.argv[1]
def L(d): return open(os.path.join(d, 'timeline.txt'), errors='replace').read().splitlines()[1:]
rows = []
for d in sorted(glob.glob(os.path.join(R, 'bgfg-*-e20-*'))):
    n = os.path.basename(d); m = re.match(r'bgfg-(\d+)-(boot|none)-(\w+)-e20-(noint-)?r(\d)', n)
    if not m: continue
    rt, inj, mode, noint, rep = m.groups()
    var = 'unarmed' if inj == 'none' else ('noint' if noint else 'default')
    ev = []
    for l in L(d):
        p = l.split(None, 2)
        if len(p) < 3: continue
        t = float(p[0]); src = p[1]; x = p[2]
        ev.append((t, src, x))
    def ts(pred, after=-1e9):
        for t, s, x in ev:
            if t > after and pred(s, x): return t
        return None
    bg = ts(lambda s, x: s == 'APP' and x.startswith('SCENE didEnterBackground'))
    fg = ts(lambda s, x: s == 'APP' and x.startswith('SCENE willEnterForeground'), bg or 0)
    act2 = ts(lambda s, x: s == 'APP' and 'didBecomeActive #2' in x)
    # condensed event list after BG-REQUEST: calls, notes, engine session lines, scene
    keep = []
    br = ts(lambda s, x: 'BG-REQUEST' in x) or 0
    for t, s, x in ev:
        if t < br - 0.001: continue
        if s == 'APP' and (x.startswith(('CALL', 'RET', 'NOTE', 'SCENE', 'END', 'no WasInterrupted'))):
            x = re.sub(r' thread=\w+', '', x); keep.append(f'{t:7.3f} A {x}')
        elif s == 'ENG' and re.search(r'StartRunning|StopRunning|m12 interrupt|interruption ended|posted DidStartRunning|posted DidStopRunning|watchdog', x):
            keep.append(f'{t:7.3f} E {x}')
        elif s == 'HOST' and ('after END' in x or 'Preferences' in x and 'returned' not in x or 'foreground' in x):
            keep.append(f'{t:7.3f} H {x}')
        elif s == 'APP' and x.startswith('frames') and (x.startswith('frames 0') or True):
            pass
    # frame windows: list of (t, n)
    fr = [(t, int(re.match(r'frames (\d+)', x).group(1))) for t, s, x in ev if s == 'APP' and x.startswith('frames')]
    rows.append(dict(name=n, rt=rt, var=var, mode=mode, rep=rep, bg=bg, fg=fg, act2=act2, keep=keep, fr=fr))
for r in rows:
    print(f"=== {r['rt']} {r['mode']} {r['var']} r{r['rep']}  bg={r['bg']} fg={r['fg']} act2={r['act2']}")
    for k in r['keep']: print('   ', k[:190])
    # frame summary after foreground: first non-zero window after fg, last non-zero window
    fr = r['fr']; 
    if r['fg']:
        nz = [t for t, n in fr if t > r['fg'] and n > 0]
        print(f"    frames after fg: first>0 at {nz[0] if nz else None}, last>0 at {nz[-1] if nz else None}; in bg: {sum(n for t,n in fr if r['bg'] and r['bg']+0.3 < t < r['fg'])}")
