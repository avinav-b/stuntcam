# SPDX-License-Identifier: Apache-2.0
# rununtilcrash.py — an lldb command script: `command script import tools/rununtilcrash.py` adds `rucrash`, which continues
# the selected process (up to 400 times) until it exits or stops on an exception or a signal other than 13 and 17
# (SIGPIPE, SIGSTOP), then prints the stop description and the stopped thread's first 30 frames.
import lldb, sys, time
def run(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget(); p = t.GetProcess(); debugger.SetAsync(False)
    for i in range(400):
        p.Continue()
        st = p.GetState()
        if st == lldb.eStateExited: print("EXITED status", p.GetExitStatus(), p.GetExitDescription()); return
        th = p.GetSelectedThread(); r = th.GetStopReason()
        if r in (lldb.eStopReasonException, lldb.eStopReasonSignal):
            if r == lldb.eStopReasonSignal and th.GetStopReasonDataAtIndex(0) in (13, 17): continue
            print("STOP", th.GetStopDescription(200))
            for f in th.frames[:30]: print("  ", f)
            return
    print("gave up after 400 continues")
def __lldb_init_module(debugger, d): debugger.HandleCommand('command script add -f rununtilcrash.run rucrash')
