# SPDX-License-Identifier: Apache-2.0
# ipspid.py <proc> <since> <pid> ... — crash reports of process <proc> written for one of the pids at or after <since> (epoch
#   seconds): prints "<pid>\t<path>" per match (tab-separated: paths may hold spaces; activation.sh reads it with awk -F'\t').
#   Looks in ~/Library/Logs/DiagnosticReports and its Retired/ (reports move there within minutes). Matching by pid, not by
#   count or mtime, so old reports whose mtime moved and other agents' crashes of the same app never count.
#   The report's capture time (the body's "captureTime", else the file's mtime) must be >= <since>, so a reused pid can't
#   match an older report.
import datetime, glob, json, os, re, sys
proc, since, pids = sys.argv[1], float(sys.argv[2]), set(sys.argv[3:])
PID = re.compile(r'"pid" : (\d+),')
CAP = re.compile(r'"captureTime" : "([^"]+)"')
def captured(raw, f):   # "2026-11-13 18:45:38.1512 -0500" -> epoch; the file's mtime if absent or unparsable
    m = CAP.search(raw)
    if m:
        try: return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f %z').timestamp()
        except ValueError:
            try: return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S %z').timestamp()
            except ValueError: pass
    return os.path.getmtime(f)
for d in ('~/Library/Logs/DiagnosticReports', '~/Library/Logs/DiagnosticReports/Retired'):
    for f in glob.glob(os.path.expanduser(d) + '/' + glob.escape(proc) + '*.ips'):
        try:
            raw = open(f, errors='replace').read()
            m = PID.search(raw)
            pid = m.group(1) if m else str(json.loads(raw.split('\n', 1)[1]).get('pid'))
            t = captured(raw, f)
        except Exception:
            continue
        if pid in pids and t >= since: print(pid, f, sep='\t')
