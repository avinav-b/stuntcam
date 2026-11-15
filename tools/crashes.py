# SPDX-License-Identifier: Apache-2.0
# crashes.py <since-epoch> — .ips in DiagnosticReports(+Retired) newer than since, from processes running in simulators.
#   our sims = the UDIDs in every $SIMCAM_UDID_<label> variable (e.g. SIMCAM_UDID_263, SIMCAM_UDID_ci).
#   A report is ours if one of those UDIDs appears anywhere in it (image paths, process path, environment): apps and their
#   frameworks carry the device's data path. Simulator daemons (notifyd, syslogd, …) run from the runtime root and often carry
#   no UDID (matching on UDIDs alone misses them); those that are clearly from a simulator (parent launchd_sim, or a path in a
#   simulator runtime) are listed as label "sim?" so they are never silently dropped. "sim?" can be any simulator on the Mac.
#   Each row says whether our loader or engine was loaded in the process (engine=yes|no): a report labelled with our UDID is
#   any process on our sims, including system daemons; engine=yes narrows it to apps stuntcam was in. The boot stub
#   (libsimcamstub.dylib) is in every process of a stub-booted sim, so it doesn't count.
import json, os, sys, glob, re
since = float(sys.argv[1])
udids = {v.strip(): k[len('SIMCAM_UDID_'):] for k, v in os.environ.items() if k.startswith('SIMCAM_UDID_') and v.strip()}
SIM_HINT = re.compile(r'launchd_sim|/CoreSimulator/Volumes/|\.simruntime/|/RuntimeRoot/')
out = []
for d in [os.path.expanduser('~/Library/Logs/DiagnosticReports'), os.path.expanduser('~/Library/Logs/DiagnosticReports/Retired')]:
    for f in glob.glob(d + '/*.ips'):
        if os.path.getmtime(f) < since: continue
        try:
            raw = open(f, errors='replace').read(); t = raw.split('\n', 1); h = json.loads(t[0]); b = json.loads(t[1])
        except Exception:
            continue
        label = next((r for u, r in udids.items() if u in raw), None)
        if not label:
            if not (SIM_HINT.search((b.get('procPath') or '') + ' ' + (b.get('parentProc') or '')) or 'launchd_sim' in raw[:20000]): continue
            label = 'sim?'
        imgs = b.get('usedImages') or []; threads = b.get('threads') or []
        fi = b.get('faultingThread', 0)
        ft = threads[fi] if isinstance(fi, int) and 0 <= fi < len(threads) else {'frames': []}
        name = lambda i: (imgs[i].get('name') or '?') if isinstance(i, int) and 0 <= i < len(imgs) else '?'
        top = [f"{name(fr.get('imageIndex'))}:{fr.get('symbol')}" for fr in (ft.get('frames') or [])[:6]]
        ours = any((i.get('name') or '') in ('libsimcam.dylib', 'libsimcamloader.dylib') for i in imgs)   # the sanitizer builds use the same names
        proc = (b.get('bundleInfo') or {}).get('CFBundleIdentifier') or b.get('procName') or h.get('name') or '?'
        out.append((label, os.path.basename(f), proc, 'engine=yes' if ours else 'engine=no', (b.get('exception') or {}).get('type') or '?', top))
for o in sorted(out, key=lambda o: (o[0], o[1])): print(o)
print('TOTAL', len(out), '(ours:', sum(1 for o in out if o[0] != 'sim?'), ', unattributed simulator processes:', sum(1 for o in out if o[0] == 'sim?'), ', engine loaded:', sum(1 for o in out if o[3] == 'engine=yes'), ')')
