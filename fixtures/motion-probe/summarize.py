# SPDX-License-Identifier: Apache-2.0
# summarize.py <results dir> <tag> — one table row per runtime and variant from $SIMCAM_RESULTS/motion-<rt>-<tag>-<variant>-<rep>/
#   per manager: rate mean / min over launches (Hz, % of the requested rate), worst max gap, timestamp offset (handler
#   systemUptime - CMLogItem.timestamp) worst |first|, |last| and max; availability and start-attempt answers (distinct values
#   with counts); camera fps; how each launch armed (dylib.log). Run with python3 -I.
import os, re, sys, collections
root, tag = sys.argv[1], sys.argv[2]
rx = re.compile(r'^motion-(\w+)-' + re.escape(tag) + r'-(\w+)-(c\d)-(\w+)-(\d+)$')
runs = collections.defaultdict(list)
for d in sorted(os.listdir(root)):
    m = rx.match(d)
    if m: runs[(m[1], m[2], m[3], m[4])].append(os.path.join(root, d))
kv = lambda s: dict(re.findall(r'(\w+)=(\S+)', s))
def num(x):
    try: return float(x.rstrip('ms'))
    except ValueError: return float('nan')
order = {'lazy': 0, 'eager': 1, 'unarmed': 2}
print('| rt | arm | cam | order | n | END | A rate mean / min (Hz) | B rate mean / min (Hz) | min % | worst gap A/B (ms) | ts off max (ms) | nonMono | avail@first | avail@end | active@attempt | delivered 2 s | cam fps mean/min | armed by |')
print('|' + '---|' * 18)
for k in sorted(runs, key=lambda k: (k[0], order.get(k[1], 9), k[2], k[3])):
    rt, arm, cam, od = k; ds = runs[k]
    rate = {'A': [], 'B': []}; gap = {'A': [], 'B': []}; off = []; nm = 0; ends = 0
    af, ae, aa, dl, fps, armed = (collections.Counter() for _ in range(6))
    for d in ds:
        t = open(os.path.join(d, 'app.log'), errors='replace').read()
        ends += 'PROBE|' in t and re.search(r'PROBE\|\s+\S+ END', t) is not None
        for mgr in 'AB':
            m = re.search(r'MGR ' + mgr + r' (.*)', t)
            if not m: continue
            v = kv(m[1]); rate[mgr].append(num(v['rate'])); gap[mgr].append(num(v['maxGap']))
            off += [abs(num(v[x])) for x in ('tsOffFirst', 'tsOffLast', 'tsOffMax') if v[x] != 'nanms']; nm += int(v['nonMonotonic'])
        for c, key in ((af, 'AVAIL@first'), (ae, 'AVAIL@end'), (aa, 'ACTIVE@attempt'), (dl, 'DELIVERED@attempt 2s')):
            m = re.search(re.escape(key) + r' ([^\n]*)', t)
            s = m[1].strip() if m else '-'
            if key.startswith('DELIVERED'):   # counts vary run to run: bucket dm by >0 / 0
                v = kv(s); s = ' '.join(f'{n}={"0" if v.get(n) == "0" else ">0"}' for n in ('gyro', 'mag', 'dm')) if v else '-'
            c[s] += 1
        m = re.search(r'CAM frames=\d+ inWindow=\d+ fps=(\S+)', t)
        if m: fps[m[1]] += 1
        dy = open(os.path.join(d, 'dylib.log'), errors='replace').read() if os.path.exists(os.path.join(d, 'dylib.log')) else ''
        m = re.search(r'LAZY armed by (\S+)', dy)
        armed['lazy:' + m[1] if m else ('eager (gate at load)' if 'TIMING gate entered' in dy else 'not armed')] += 1
    fmt = lambda c: '; '.join(f'{s} ×{n}' if len(c) > 1 else s for s, n in c.most_common())
    mm = lambda xs: f'{sum(xs) / len(xs):.1f} / {min(xs):.1f}' if xs else '-'
    allr = rate['A'] + rate['B']; pct = f'{min(allr):.1f}' if allr else '-'
    fl = [float(x) for x in fps.elements()]; fs = f'{sum(fl) / len(fl):.1f} / {min(fl):.1f}' if fl and cam == 'c1' else '-'
    print(f'| {rt} | {arm} | {cam[1]} | {od} | {len(ds)} | {ends} | {mm(rate["A"])} | {mm(rate["B"])} | {pct} | '
          f'{max(gap["A"], default=float("nan")):.1f} / {max(gap["B"], default=float("nan")):.1f} | {max(off, default=float("nan")):.3f} | {nm} | '
          f'{fmt(af)} | {fmt(ae)} | {fmt(aa)} | {fmt(dl)} | {fs} | {fmt(armed)} |')
