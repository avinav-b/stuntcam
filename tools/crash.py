# SPDX-License-Identifier: Apache-2.0
# crash.py <report.ips> [n]: one crash report's file name, exception and asi, then the first n frames (default 16) of
# its triggered thread: image, symbol, offset in the symbol
import json,sys
t=open(sys.argv[1]).read().split('\n',1)[1]; j=json.loads(t)
print(sys.argv[1].split('/')[-1], j.get('exception'), j.get('asi'))
imgs=j['usedImages']
for th in j['threads']:
    if th.get('triggered'):
        for f in th['frames'][:int(sys.argv[2]) if len(sys.argv)>2 else 16]:
            im=imgs[f['imageIndex']]; print(' ', im.get('name'), f.get('symbol'), f.get('symbolLocation'))
