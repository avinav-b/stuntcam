# SPDX-License-Identifier: Apache-2.0
# hold.py <pid> <symbol> <count> <hold_s> <cmdfile> <trigger|-> <after|-> <settle_s>
# Attach lldb (SB API, command line only) to our own probe, then <count> times: enable a breakpoint on <symbol>, write
# <trigger> to the probe's command file, wait for the breakpoint, hold the process stopped for <hold_s>, disable the
# breakpoint, continue, write <after>, let it run <settle_s>. Then detach. Run with Xcode's python3 and PYTHONPATH=$(lldb -P).
# Exit 0; 2 if a breakpoint was not hit; 1 if the attach failed or the process exited; 130/143 on SIGINT/SIGTERM/SIGHUP. On
# every exit after the attach (signals included) it disables the breakpoint, continues the app if stopped and detaches.
import lldb, signal, sys, time
class Interrupted(Exception): pass
def on_signal(signum, frame):   # SIGTERM's default would kill python without running finally
    # ignore further signals before raising: Ctrl-C (SIGINT to the group) is followed ~50 ms later by drive.sh's SIGTERM,
    # and if both are pending when an lldb call returns, a second handler run would raise inside finally and skip the cleanup
    for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP): signal.signal(s, signal.SIG_IGN)
    raise Interrupted(signum)
for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP): signal.signal(s, on_signal)
pid, sym, count, hold, cmdfile, trig, after, settle = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), sys.argv[5], sys.argv[6], sys.argv[7], float(sys.argv[8])
def now(): return time.time() - 978307200   # CFAbsoluteTime, the probe's and the engine log's clock
def log(s): print("HOLD| %.3f %s" % (now(), s), flush=True)
def send(c):
    if c == '-': return
    try: n = sum(1 for _ in open(cmdfile))
    except FileNotFoundError: n = 0
    a, _, b = c.partition(':')
    with open(cmdfile, 'a') as f: f.write("%d %s %s\n" % (n + 1, a, b))
    log("sent %s %s" % (a, b))
dbg = lldb.SBDebugger.Create(); dbg.SetAsync(True)
target = dbg.CreateTarget(''); listener = dbg.GetListener(); err = lldb.SBError()
def wait(states, timeout):
    end = time.time() + timeout
    while time.time() < end:
        ev = lldb.SBEvent()
        if listener.WaitForEvent(1, ev) and lldb.SBProcess.EventIsProcessEvent(ev):
            st = lldb.SBProcess.GetStateFromEvent(ev)
            if st in states: return st
    return None
rc = 0; proc = None; bp = None
try:
    t = time.time(); proc = target.AttachToProcessWithID(listener, pid, err)
    if not err.Success(): log("attach FAILED %s" % err); rc = 1; raise SystemExit
    if proc.GetState() != lldb.eStateStopped: wait([lldb.eStateStopped], 30)
    log("attached pid %d in %.2f s (state %d)" % (pid, time.time() - t, proc.GetState()))
    bp = target.BreakpointCreateByName(sym); log("breakpoint %s locations=%d" % (sym, bp.GetNumLocations()))
    bp.SetEnabled(False); proc.Continue(); wait([lldb.eStateRunning], 10); time.sleep(1)
    for i in range(1, count + 1):
        bp.SetEnabled(True); send(trig)
        hit = False; end = time.time() + 30
        while time.time() < end:
            st = wait([lldb.eStateStopped, lldb.eStateExited], max(1, end - time.time()))
            if st == lldb.eStateExited: log("process exited"); rc = 1; raise SystemExit
            if st != lldb.eStateStopped: break
            th = [x for x in proc.threads if x.GetStopReason() == lldb.eStopReasonBreakpoint]
            if th: hit = True; break
            log("stopped for another reason (%s); continuing" % [x.GetStopReason() for x in proc.threads if x.GetStopReason()]); proc.Continue()
        if not hit: log("rep %d: breakpoint NOT hit in 30 s" % i); rc = 2; continue
        th = th[0]; log("rep %d: STOPPED at %s on thread '%s' queue '%s'; holding %.0f s" % (i, th.GetFrameAtIndex(0).GetFunctionName(), th.GetName(), th.GetQueueName(), hold))
        time.sleep(hold)
        bp.SetEnabled(False); proc.Continue(); wait([lldb.eStateRunning], 10); log("rep %d: CONTINUED" % i)
        send(after); time.sleep(settle)
except SystemExit: pass
except Interrupted as e:
    rc = 128 + e.args[0]; log("interrupted by signal %d" % e.args[0])
finally:   # never leave the app stopped at a breakpoint or attached; a second signal can't cut this short
    for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP): signal.signal(s, signal.SIG_IGN)
    try:
        if bp is not None: bp.SetEnabled(False)
        if proc is not None and proc.IsValid() and proc.GetState() == lldb.eStateStopped: proc.Continue(); log("continued the app")
        if proc is not None and proc.IsValid() and proc.GetState() not in (lldb.eStateExited, lldb.eStateDetached, lldb.eStateInvalid):
            proc.Detach(); log("detached")
    except Exception as e: log("cleanup error %s" % e)
    lldb.SBDebugger.Destroy(dbg)
sys.exit(rc)
