# calls.py <condense.py output> (w3 e20) SPDX-License-Identifier: Apache-2.0
# per run: each app call after BG-REQUEST, whether an engine Start/StopRunning line followed within 50 ms,
# and the AVF notifications that followed within 100 ms; engine lifecycle lines; after-END pump state
import re, sys
runs = {}; cur = None
for l in open(sys.argv[1]):
    if l.startswith('==='): cur = l[4:].split('  ')[0]; runs[cur] = {'hdr': l.strip(), 'ev': []}; continue
    m = re.match(r'\s+([\d.]+) ([AEH]) (.*)', l)
    if m and cur: runs[cur]['ev'].append((float(m.group(1)), m.group(2), m.group(3)))
    elif cur and 'frames after fg' in l: runs[cur]['fr'] = l.strip()
for k, r in runs.items():
    ev = r['ev']; out = []
    for i, (t, s, x) in enumerate(ev):
        if s == 'A' and x.startswith('CALL'):
            what = re.match(r'CALL (\w+) \(([^)]*\)?)\)', x); call, why = what.group(1), what.group(2)
            before = re.search(r'before: (.*)', x).group(1).replace('isRunning=', 'R').replace(' isInterrupted=', '/I')
            eng = any(s2 == 'E' and abs(t2 - t) < 0.05 and ('StartRunning' if call == 'startRunning' else 'StopRunning') in x2 and 'posted' not in x2 for t2, s2, x2 in ev)
            notes = [re.match(r'NOTE (\w+?)Notification', x2).group(1) for t2, s2, x2 in ev if s2 == 'A' and x2.startswith('NOTE') and t - 0.001 <= t2 < t + 0.15]
            ret = next((x2 for t2, s2, x2 in ev if s2 == 'A' and x2.startswith('RET  ' + call) and t2 >= t), '')
            after = re.search(r'\| (.*)', ret).group(1).replace('isRunning=', 'R').replace(' isInterrupted=', '/I') if ret else '?'
            out.append(f"{call[:-7]}({why}) {before}->{after} eng={'Y' if eng else 'N'} notes={','.join(notes) or '-'}")
        elif s == 'E' and ('m12' in x):
            out.append('ENG ' + re.sub(r'session 0x[0-9a-f]+ ', '', x).replace(', 0 recordings stopped', ''))
        elif s == 'A' and x.startswith('NOTE') and not any(abs(t - t2) < 0.15 and s2 == 'A' and x2.startswith('CALL') for t2, s2, x2 in ev if t2 <= t):
            out.append('note ' + re.match(r'NOTE (\w+?)Notification', x).group(1) + ' ' + x.split('| ')[1].replace('isRunning=', 'R').replace(' isInterrupted=', '/I'))
        elif s == 'A' and x.startswith('SCENE') and ('Background' in x or 'Foreground' in x or '#2' in x):
            out.append(x.split(' |')[0].replace('SCENE ', '').replace('didBecomeActive #2', 'active2') + ' ' + x.split('| ')[1].replace('isRunning=', 'R').replace(' isInterrupted=', '/I'))
        elif s == 'H' and 'CPU' in x:
            out.append('CPU ' + '%.2f' % float(re.search(r'CPU ([\d.]+)', x).group(1)) + 's/3s')
        elif s == 'H' and 'sample' in x:
            out.append('pump-stacks ' + re.search(r'(\d+) naming', x).group(1))
    print(r['hdr']); print('    ' + '\n    '.join(out)); print('    ' + r.get('fr', ''))
