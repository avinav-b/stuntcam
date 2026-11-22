# SPDX-License-Identifier: Apache-2.0
# lldb-attach.py <pid> <logfile> <regex> <timeout_s>: attaches lldb to one of our own simulator apps, continues it,
# stays attached until <logfile> matches <regex> or <timeout_s> passes, then detaches.
#
# It uses the SB API from the command line only, as fixtures/pause-probe/hold.py does. It sets no breakpoints: the
# app only has to run traced (P_TRACED), for example after `simctl launch --wait-for-debugger`. It prints "ATTACH| ..."
# lines and exits 1 if the attach failed. Run it with Xcode's python3 and PYTHONPATH=$(xcrun lldb -P).
import re
import sys
import time

import lldb

# Seconds from the Unix epoch to the Core Foundation epoch (2001-01-01). The engine's log uses CFAbsoluteTime.
CF_ABSOLUTE_TIME_EPOCH = 978307200
# How long each wait for a debugger event blocks, in seconds.
EVENT_POLL_S = 1
# How long to wait for the attached process to report that it stopped, and then that it runs again.
ATTACH_STOP_WAIT_S = 30
CONTINUE_WAIT_S = 10
# The pause between log checks while the app runs, in seconds (each check also waits up to EVENT_POLL_S for a
# debugger event, so a full cycle takes about a second).
LOG_CHECK_S = 0.5


def now():
    """Returns the current time as a CFAbsoluteTime, the engine log's clock."""
    return time.time() - CF_ABSOLUTE_TIME_EPOCH


def log(message):
    print("ATTACH| %.3f %s" % (now(), message), flush=True)


def wait_for_state(listener, states, timeout_s):
    """Waits up to timeout_s for a process event into one of the given states. Returns that state, or None."""
    end = time.time() + timeout_s
    while time.time() < end:
        event = lldb.SBEvent()
        if listener.WaitForEvent(EVENT_POLL_S, event) and lldb.SBProcess.EventIsProcessEvent(event):
            state = lldb.SBProcess.GetStateFromEvent(event)
            if state in states:
                return state

    return None


def log_matches(log_path, pattern):
    """Tells whether the log file matches the pattern. A missing log file does not match."""
    try:
        return bool(pattern.search(open(log_path, errors='replace').read()))
    except FileNotFoundError:
        return False


def run_until_log_matches(process, listener, log_path, pattern, timeout_s):
    """Lets the attached process run until the log matches, the timeout passes or the process exits."""
    end = time.time() + timeout_s
    matched = False
    while time.time() < end and not matched:
        state = wait_for_state(listener, [lldb.eStateStopped, lldb.eStateExited], LOG_CHECK_S)
        if state == lldb.eStateStopped:
            # A signal or exception stop: pass it on, and never leave the app stopped.
            reasons = [thread.GetStopReason() for thread in process.threads if thread.GetStopReason()]
            log("stopped (%s); continuing" % reasons)
            process.Continue()

        matched = log_matches(log_path, pattern)
        if state == lldb.eStateExited:
            log("process exited")
            break

    log("log matched" if matched else "no match before the timeout or exit")


def main():
    pid = int(sys.argv[1])
    log_path = sys.argv[2]
    pattern = re.compile(sys.argv[3])
    timeout_s = float(sys.argv[4])

    debugger = lldb.SBDebugger.Create()
    debugger.SetAsync(True)
    target = debugger.CreateTarget('')
    listener = debugger.GetListener()
    error = lldb.SBError()

    attach_start = time.time()
    process = target.AttachToProcessWithID(listener, pid, error)
    if not error.Success():
        log("attach FAILED %s" % error)
        sys.exit(1)

    if process.GetState() != lldb.eStateStopped:
        wait_for_state(listener, [lldb.eStateStopped], ATTACH_STOP_WAIT_S)

    log("attached pid %d in %.2f s (state %d)" % (pid, time.time() - attach_start, process.GetState()))

    try:
        process.Continue()
        wait_for_state(listener, [lldb.eStateRunning], CONTINUE_WAIT_S)
        log("continued")
        run_until_log_matches(process, listener, log_path, pattern, timeout_s)
    finally:
        # Never leave the app stopped, even if something above failed.
        try:
            if process.GetState() == lldb.eStateStopped:
                process.Continue()
        except Exception:  # lldb lost the process (it exited or detached): there is nothing to continue
            pass

    if process.IsValid() and process.GetState() not in (lldb.eStateExited, lldb.eStateDetached):
        process.Detach()
        log("detached")

    lldb.SBDebugger.Destroy(debugger)


if __name__ == '__main__':
    main()
