# SPDX-License-Identifier: Apache-2.0
# merge.py <run dir> — one timeline from app.log (PROBE| lines), host.log (HOST| lines) and dylib.log (engine LOG lines),
# sorted by CFAbsoluteTime. Columns: seconds since the probe's first line, source (APP/HOST/ENG), text. Engine lines are cut
# to 200 characters; multi-line engine entries (configuration dumps) keep their first line only.
import os, re, sys
d = sys.argv[1]; rows = []
def rd(n):
    p = os.path.join(d, n)
    return open(p, errors='replace').read().splitlines() if os.path.exists(p) else []
for l in rd('app.log'):
    m = re.match(r'.*?PROBE\| (\d+\.\d+)\s+(\S+) (.*)', l)
    if m: rows.append((float(m.group(1)), 'APP ', m.group(3)))
for l in rd('host.log'):
    m = re.match(r'HOST\| (\d+\.\d+) (.*)', l)
    if m: rows.append((float(m.group(1)), 'HOST', m.group(2)))
for l in rd('dylib.log'):
    m = re.match(r'(\d{9}\.\d{3}) \[simcam \d+\] (.*)', l)
    if m: rows.append((float(m.group(1)), 'ENG ', m.group(2)[:200]))
rows.sort(key=lambda r: (r[0], r[1] != 'ENG '))
apps = [r[0] for r in rows if r[1] == 'APP ']
t0 = apps[0] if apps else (rows[0][0] if rows else 0)
print(f'# {os.path.basename(d)}  t0 = {t0:.3f} (CFAbsoluteTime of the probe\'s first line)')
for t, s, x in rows: print(f'{t - t0:8.3f} {s} {x}')
