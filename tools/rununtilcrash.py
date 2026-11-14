# SPDX-License-Identifier: Apache-2.0
import lldb, sys, time
def run(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget(); p = t.GetProcess(); debugger.SetAsync(False)
    for i in range(400):
        p.Continue()
        st = p.GetState()
        if st == lldb.eStateExited: print("EXITED status", p.GetExitStatus(), p.GetExitDescription()); return
        th = p.GetSelectedThread(); r = th.GetStopReason()
        if r in (lldb.eStopReasonException, lldb.eStopReasonSignal) or (r == lldb.eStopReasonInstrumentation and False):
            if r == lldb.eStopReasonSignal and th.GetStopReasonDataAtIndex(0) in (13, 17): continue
            print("STOP", th.GetStopDescription(200))
            for t2 in [th]:
                for f in t2.frames[:30]: print("  ", f)
            return
    print("gave up after 400 continues")
def __lldb_init_module(debugger, d): debugger.HandleCommand('command script add -f rununtilcrash.run rucrash')
