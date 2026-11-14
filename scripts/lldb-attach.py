# SPDX-License-Identifier: Apache-2.0
# lldb-attach.py <pid> <logfile> <regex> <timeout_s> — attach lldb (SB API, command line only; the method of
# fixtures/pause-probe/hold.py on main) to one of our own sim apps, continue it, stay attached until <logfile> matches <regex>
# or <timeout_s> passes, then detach. No breakpoints: the app only has to run traced (P_TRACED), e.g. after
# `simctl launch --wait-for-debugger`. Prints "ATTACH| ..." lines; exit 1 if the attach failed.
# Run with Xcode's python3 and PYTHONPATH=$(xcrun lldb -P).
import lldb, re, sys, time
pid, logf, pat, timeout = int(sys.argv[1]), sys.argv[2], re.compile(sys.argv[3]), float(sys.argv[4])
def now(): return time.time() - 978307200   # CFAbsoluteTime, the engine log's clock
def log(s): print("ATTACH| %.3f %s" % (now(), s), flush=True)
dbg = lldb.SBDebugger.Create(); dbg.SetAsync(True)
target = dbg.CreateTarget(''); listener = dbg.GetListener(); err = lldb.SBError()
def wait(states, t):
    end = time.time() + t
    while time.time() < end:
        ev = lldb.SBEvent()
        if listener.WaitForEvent(1, ev) and lldb.SBProcess.EventIsProcessEvent(ev):
            st = lldb.SBProcess.GetStateFromEvent(ev)
            if st in states: return st
    return None
t = time.time(); proc = target.AttachToProcessWithID(listener, pid, err)
if not err.Success(): log("attach FAILED %s" % err); sys.exit(1)
if proc.GetState() != lldb.eStateStopped: wait([lldb.eStateStopped], 30)
log("attached pid %d in %.2f s (state %d)" % (pid, time.time() - t, proc.GetState()))
try:
    proc.Continue(); wait([lldb.eStateRunning], 10); log("continued")
    end = time.time() + timeout; matched = False
    while time.time() < end and not matched:
        st = wait([lldb.eStateStopped, lldb.eStateExited], 0.5)
        if st == lldb.eStateStopped:   # a signal or exception stop: pass it on, never leave the app stopped
            log("stopped (%s); continuing" % [x.GetStopReason() for x in proc.threads if x.GetStopReason()]); proc.Continue()
        try: matched = bool(pat.search(open(logf, errors='replace').read()))
        except FileNotFoundError: pass
        if st == lldb.eStateExited: log("process exited"); break
    log("log matched" if matched else "no match before the timeout or exit")
finally:
    try:
        if proc.GetState() == lldb.eStateStopped: proc.Continue()
    except Exception: pass
if proc.IsValid() and proc.GetState() not in (lldb.eStateExited, lldb.eStateDetached): proc.Detach(); log("detached")
lldb.SBDebugger.Destroy(dbg)
