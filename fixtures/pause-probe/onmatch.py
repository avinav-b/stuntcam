# SPDX-License-Identifier: Apache-2.0
# onmatch.py <app.log> <regex> <timeout_s> <pid|0>: wait (2 ms polls) for a line matching regex; then SIGSTOP pid if not 0.
# The caller checks first that pid is our own probe on our own simulator. Prints the match time (CFAbsoluteTime).
import os, re, signal, sys, time
f, rx, to, pid = sys.argv[1], re.compile(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
end = time.time() + to
while time.time() < end:
    with open(f, errors='replace') as h:
        if any(rx.search(l) for l in h):
            if pid: os.kill(pid, signal.SIGSTOP)
            print("%.3f matched %s%s" % (time.time() - 978307200, sys.argv[2], " -> SIGSTOP %d" % pid if pid else "")); sys.exit(0)
    time.sleep(0.002)
print("timeout"); sys.exit(1)
