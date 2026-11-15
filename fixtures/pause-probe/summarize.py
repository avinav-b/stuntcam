# SPDX-License-Identifier: Apache-2.0
# summarize.py <results dir> <case-rt>... : one markdown row per run: pauses seen by the watchdog, sentinel events,
# latches, and whether every operation completed. Run with python3 -I.
import os, re, sys
base = sys.argv[1]
def rd(p):
    try: return open(p, errors='replace').read().splitlines()
    except FileNotFoundError: return []
def t(l):
    try: return float(l.split()[1 if l.startswith(('PP|', 'HOST|', 'HOLD|')) else 0])
    except Exception: return 0.0
print("| run | pauses (watchdog gaps, s) | debugger | hang / unknown-hang / crash | latched | ops open ≥ 5 s through a pause | photos req / done ok / err | movies started / finalised readable / error codes | frames back after each pause (s) | postcheck |")
print("|---|---|---|---|---|---|---|---|---|---|")
for name in sys.argv[2:]:
    d = os.path.join(base, os.environ.get('PP_RUN', 'e25') + '-' + name)
    app = [l for l in rd(d + '/app.log') if l.startswith('PP|')]; dy = rd(d + '/dylib.log'); ev = [l.split() for l in rd(d + '/events.txt')]; host = rd(d + '/host.log')
    gaps = [(t(l), float(re.search(r'stopped for ([\d.]+) s', l).group(1))) for l in dy if 'process was stopped for' in l]
    dbg = sum('debugger attached' in l for l in dy), sum('debugger detached' in l for l in dy)
    kinds = [e[2] for e in ev if len(e) > 2]
    latched = [l for l in dy if 'latched' in l]
    opens, spans = {}, []
    for e in ev:
        if len(e) < 6: continue
        if e[2] == 'op-begin': opens[e[5]] = (int(e[0]), e[3])
        elif e[2] == 'op-end' and e[5] in opens:
            b, f = opens.pop(e[5]); dur = int(e[0]) - b
            if dur >= 5: spans.append('%s %ds' % (f, dur))
    spans += ['%s never ended' % f for _, f in opens.values()]
    req = sum(' PHOTO-REQ ' in l for l in app); ok = sum(' PHOTO-DONE ' in l and 'err=nil' in l for l in app); perr = sum(' PHOTO-DONE ' in l and 'err=nil' not in l for l in app)
    ms = sum(' MOVIE-STARTED ' in l for l in app); md = sum(' MOVIE-DONE ' in l and 'readable=true' in l for l in app)
    codes = sorted({re.search(r'err=(\S+)', l).group(1) for l in app if 'MOVIE-FINISH-CB' in l})
    fps = [(t(l), int(l.split()[4])) for l in app if ' FPS ' in l]
    back = []
    for g, _ in gaps:
        nxt = next((ft for ft, n in fps if ft > g and n > 0), None); back.append('%.1f' % (nxt - g) if nxt else 'never')
    pc = [l for l in host if 'postcheck start' in l]
    post = 'n/a'
    if pc:
        t0 = t(pc[0]); after = [l for l in app if t(l) > t0]
        post = 'ok' if any('PHOTO-DONE' in l and 'err=nil' in l for l in after) and any('MOVIE-DONE' in l and 'readable=true' in l for l in after) and any('RET start' in l and 'running=1' in l for l in after) else 'FAIL'
    print('| %s | %d: %s | attached %d / detached %d | %d / %d / %d | %s | %s | %d / %d / %d | %d / %d / %s | %s | %s |' % (
        name, len(gaps), ' '.join('%.1f' % s for _, s in gaps) or '-', dbg[0], dbg[1], kinds.count('hang'), kinds.count('unknown-hang'), kinds.count('crash'),
        len(latched) or 0, ', '.join(spans) or '-', req, ok, perr, ms, md, ' '.join(codes) or '-', ' '.join(back) or '-', post))
