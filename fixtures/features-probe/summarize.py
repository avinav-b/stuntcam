# SPDX-License-Identifier: Apache-2.0
# summarize.py <app.log> — the read-out of one features-probe launch as a matrix: one row per (object, key), one column
# per camera; FMTCOUNT rows (formats that read YES / all formats). Keys absent on this OS are listed once at the end.
import sys, re, collections
rows = collections.OrderedDict(); cams = []; absent = set(); fmt = collections.OrderedDict(); other = []
for ln in open(sys.argv[1], errors='replace'):
    ln = ln.rstrip('\r\n')
    m = re.match(r'PROBE\| \S+ +\S+ (.*)$', ln)
    if not m: continue
    t = m.group(1)
    if t.startswith('R '):
        p = t.split(' ', 4)
        if len(p) < 5: other.append(t); continue
        _, cam, obj, key, val = p
        if obj.startswith('session'): other.append(t); continue
        if cam not in cams: cams.append(cam)
        if val == 'absent': absent.add(f'{obj}.{key}'); continue
        rows.setdefault((obj, key), {})[cam] = val
    elif t.startswith('FMTCOUNT '):
        _, cam, key, val = t.split(' ', 3)
        if cam not in cams: cams.append(cam)
        if val.endswith(' absent'): absent.add(f'format.{key}'); continue
        fmt.setdefault(key, {})[cam] = val
    elif t.startswith(('CAMERAS', 'AUTH', 'pid=')): other.append(t)
print('| object.key | ' + ' | '.join(cams) + ' |'); print('|---|' + '---|' * len(cams))
for (obj, key), v in rows.items():
    vals = [v.get(c, '') for c in cams]
    if len(set(x for x in vals if x)) == 1 and all(vals[i] for i in range(len(cams)) if cams[i] in v):
        pass
    print(f'| {obj}.{key} | ' + ' | '.join(vals) + ' |')
for key, v in fmt.items():
    print(f'| formats.{key} | ' + ' | '.join(v.get(c, '') for c in cams) + ' |')
print(); print('absent on this OS:', ', '.join(sorted(absent)) or 'none')
for o in other: print('#', o)
