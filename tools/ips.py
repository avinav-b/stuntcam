# SPDX-License-Identifier: Apache-2.0
import json,sys
for f in sys.argv[1:]:
    t=open(f).read().split('\n',1); h=json.loads(t[0]); b=json.loads(t[1])
    print('==', f, h.get('app_name'), b.get('procLaunch'), b.get('exception',{}).get('type'), b.get('exception',{}).get('signal'), b.get('termination',{}).get('indicator'))
    imgs=b['usedImages']; ft=b['threads'][b.get('faultingThread',0)]
    print('  env DYLD:', [i.get('name') for i in imgs if 'simcam' in (i.get('name') or '') or 'fig' in (i.get('name') or '')][:3])
    for fr in ft['frames'][:18]:
        im=imgs[fr['imageIndex']]; print('   ', im.get('name'), fr.get('symbol'), hex(fr.get('imageOffset',0)))
