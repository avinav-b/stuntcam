# SPDX-License-Identifier: Apache-2.0
# detail.py <results dir> <tag> — per runtime and arming mode (lazy | eager | unarmed), pooled over camera and order:
#   the timestamp clock check, per-manager gap and interval spread, first-update latency, gravity, deviceMotion deliveries,
#   and whether any armed launch gave answers that differ from the rest. Run with python3 -I.
import os, re, sys, collections, statistics
root, tag = sys.argv[1], sys.argv[2]
rx = re.compile(r'^motion-(\w+)-' + re.escape(tag) + r'-(\w+)-(c\d)-(\w+)-(\d+)$')
kv = lambda s: dict(re.findall(r'(\w+)=(\S+)', s))
num = lambda x: float(x.rstrip('ms')) if x not in ('nan', 'nanms') else float('nan')
g = collections.defaultdict(list)
for d in sorted(os.listdir(root)):
    m = rx.match(d)
    if m: g[(m[1], m[2])].append((m[3], m[4], open(os.path.join(root, d, 'app.log'), errors='replace').read()))
def pct(xs, p): xs = sorted(xs); return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else float('nan')
for (rt, arm) in sorted(g, key=lambda k: (k[0], {'lazy': 0, 'eager': 1, 'unarmed': 2}[k[1]])):
    L = g[(rt, arm)]; gaps, mins, means, lat, clk, offs, grav, dmc, answers, rates = [], [], [], [], [], [], collections.Counter(), [], collections.Counter(), []
    for cam, od, t in L:
        for mg in 'AB':
            m = re.search(r'MGR ' + mg + r' (.*)', t); v = kv(m[1])
            rates.append(num(v['rate']))
            if v['count'] == '0': continue
            gaps.append(num(v['maxGap'])); mins.append(num(v['minIv'])); means.append(num(v['meanIv'])); lat.append(num(v['firstLatency']))
            clk.append(abs(num(v['tsFirst']) - num(v['upAtFirst']))); offs.append(num(v['tsOffMax'])); grav[v['gravity']] += 1
        m = re.search(r'DELIVERED@attempt 2s (.*)', t); dmc.append(int(kv(m[1])['dm']))
        ans = ' | '.join(re.search(k + r' ([^\n]*)', t)[1].strip() for k in ('AVAIL@first', 'ACTIVE@start', 'AVAIL@end', 'ACTIVE@attempt'))
        answers[ans + ' | pull=' + kv(re.search(r'MGR A (.*)', t)[1])['pull']] += 1
    up = [num(kv(re.search(r'MGR A (.*)', t)[1])['upAtFirst']) for _, _, t in L]
    print(f'## {rt} {arm}: {len(L)} launches, {len(rates)} manager-runs')
    print(f'  rate Hz: min {min(rates):.1f}, mean {statistics.mean(rates):.2f}')
    if gaps:
        print(f'  maxGap ms per manager-run: median {statistics.median(gaps):.1f}, p95 {pct(gaps, .95):.1f}, max {max(gaps):.1f}; minIv ms: min {min(mins):.2f}; meanIv ms: {min(means):.2f}..{max(means):.2f}')
        print(f'  first update after start ms: median {statistics.median(lat):.2f}, max {max(lat):.2f}')
        print(f'  clock: |handler systemUptime - CMLogItem.timestamp| max {max(offs):.3f} ms (tsFirst vs upAtFirst as logged, 1 ms resolution: max {1000 * max(clk):.0f} ms); systemUptime at first sample {min(up):.0f}..{max(up):.0f} s')
        print(f'  gravity (first sample): {dict(grav)}')
    print(f'  deviceMotion updates delivered in the 2 s start attempt: {min(dmc)}..{max(dmc)}')
    for a, n in answers.most_common(): print(f'  answers ×{n}: {a}')
