# SPDX-License-Identifier: Apache-2.0
# rununtilcrash.py: an lldb command script. `command script import tools/rununtilcrash.py` adds the `rucrash`
# command, which continues the selected process (up to 400 times) until it exits, or stops on an exception or on a
# signal other than SIGPIPE and SIGSTOP. It then prints the stop description and the stopped thread's first 30 frames.
import lldb

MAX_CONTINUES = 400
# SIGPIPE and SIGSTOP (their numbers on Darwin): stops the app survives, so the command continues past them.
IGNORED_SIGNALS = (13, 17)
# The longest stop description to ask lldb for, in characters.
STOP_DESCRIPTION_LENGTH = 200
MAX_FRAMES = 30


def run(debugger, command, result, internal_dict):
    """The rucrash command."""
    process = debugger.GetSelectedTarget().GetProcess()
    debugger.SetAsync(False)
    for _ in range(MAX_CONTINUES):
        process.Continue()
        state = process.GetState()
        if state == lldb.eStateExited:
            print("EXITED status", process.GetExitStatus(), process.GetExitDescription())
            return

        thread = process.GetSelectedThread()
        reason = thread.GetStopReason()
        if reason not in (lldb.eStopReasonException, lldb.eStopReasonSignal):
            continue

        if reason == lldb.eStopReasonSignal and thread.GetStopReasonDataAtIndex(0) in IGNORED_SIGNALS:
            continue

        print("STOP", thread.GetStopDescription(STOP_DESCRIPTION_LENGTH))
        for frame in thread.frames[:MAX_FRAMES]:
            print("  ", frame)

        return

    print(f"gave up after {MAX_CONTINUES} continues")


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f rununtilcrash.run rucrash')
