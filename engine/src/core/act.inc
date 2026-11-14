// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): bounded activation, shared state (W4 PR A; the plan is
// the lazy-activation plan, the state machine and phases are in core/lazy.inc).
//
//   IDLE --first trapped call--> RUNNING --prepare OK, CAS--> COMMITTING (SWAP) --> ARMED  (START: a separate work item)
//                                   |-- prepare fails (latch, preflight, exception, missing hook, forced) --> STOCK
//                                   `-- a waiter's deadline passes: CAS RUNNING -> STOCK --------------------> STOCK
//   A valid latched-<build> marker at load (sentinel on, no clear) starts the process in STOCK.
// ARMED and STOCK are terminal. Loads are acquire, the ARMED/STOCK stores release, so a hook that sees ARMED sees every
// IMP and pointer SWAP wrote.
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
enum { ACT_IDLE = 0, ACT_RUNNING, ACT_COMMITTING, ACT_ARMED, ACT_STOCK };
static _Atomic int gAct;
static int ActState(void) { return atomic_load_explicit(&gAct, memory_order_acquire); }
static BOOL ActIsArmed(void) { return ActState() == ACT_ARMED; }
// set on the activation thread while ActRun runs (primary re-entrancy detector, plan §2.4)
static __thread int gActThread;
// prepare is running: the minimal crash handler attributes faults to `activate` only while this is set (plan §2.5)
static _Atomic int gActRunning;
// the step activation is in, for the timeout record's phase= (plan §2.6)
enum { PH_START = 0, PH_TRIM, PH_SENTINEL, PH_LOOKUPS, PH_PREFLIGHT, PH_RESOLVE, PH_SWAP, PH_DONE };
static const char *kActPhaseName[] = {"start", "trim", "sentinel", "lookups", "preflight", "resolve", "swap", "done"};
static _Atomic int gActPhase;
// computed once at load (no setenv after load: plan §1)
static BOOL gActFakeBreak;              // SIMCAM_PF_BREAK / M2_PREFLIGHT_BREAK (was setenv("M2_PF_FAKEBREAK") at activation)
static uint64_t ActNow(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

// ---- W4 off switches (M16_*, parsed at load; docs/ledger.md L56, L57) ----
static long gM16SlowMs; static int gM16SlowAt = PH_PREFLIGHT;   // M16_SLOW_ACTIVATE_MS=<n>[@trim|@sentinel|@lookups|@preflight|@resolve]
static long gM16SlowSwapMs, gM16SlowStartMs;                   // M16_SLOW_SWAP_MS, M16_SLOW_START_MS
static long gM16FigFaultMs;                                     // M16_FIG_FAULT_AT_MS: a CoreMedia fault on a global queue n ms after the first call
static char gM16Fail[16];                                       // M16_ACTIVATE_FAIL=preflight|throw|missing|crash|crashfig|swapthrow|crashstart|crash2
                                                                // (swapthrow: SWAP throws, and START throws too, at its end;
                                                                // crashstart, crash2: W4 e54, a fault in START on 1 / 2 threads)
static BOOL gM16PfReal, gM16NeedsMain, gM16Reenter, gM16NoTimeout, gM16NoUIOrientAtLoad;
static BOOL gM16MotionFault;                                    // M16_MOTION_FAULT=1 (PR B): a fault in a motion tick, 1 s after the first
// M16_ORIENT_FAULT (PR B review; e29 the other three): a fault in the orientation tracker, which must count as camera:
//   1        in upd, once ARMED with a RotationCoordinator, after the coordinators' KVO
//   start    in M5OrientTrackStart, after its key clear (the first motion start or data call), once ARMED (it waits up to 5 s
//            for another thread to arm the camera: the race it stands for is a camera armed while main is in that call)
//   runmain  in M5OrientTrackRunMain's start branch, before upd (reached from M5OrientTrackStart on main), once ARMED (same wait)
//   ensure   ActEnsure's main-thread tracker run starts with the motion key set, as if the first camera call came from inside a
//            motion entry point (the path review low 3 closed), then faults like runmain
enum { ORF_NONE = 0, ORF_UPD, ORF_START, ORF_RUNMAIN, ORF_ENSURE };
static int gM16OrientFault;
static BOOL gM17TrackerMotionScope;                             // M17_TRACKER_MOTION_SCOPE=1 (ledger L61): upd under the motion key again
static BOOL gM17TrackStartKeepKey;                              // M17_TRACKSTART_KEEP_KEY=1 (ledger L62): M5OrientTrackStart keeps the key
static BOOL gM17EnsureKeepKey;                                  // M17_ENSURE_KEEP_KEY=1 (ledger L63): ActEnsure's tracker run keeps the key
// M16_TRAP_FAULT=1 (e31, review L8): a fault in our image in the first camera call's trap (ActEnsure), once ARMED,
// before its main-thread tracker run: camera code, reached from whatever app code made that call. Its line names the feature
// key the thread carried. M17_APP_SEND_KEEP_KEY=1 (ledger L65): motion.inc's sends that app code can answer (an interval
// getter, the handler copy, addOperationWithBlock:) keep the motion key, as before review round 2 low 3
static BOOL gM16TrapFault, gM17AppSendKeepKey;
// PR B off switches (M17_*, ledger L58-L60) are read where they act: M17_NO_MOTION_MARKERS (SCMotionStockAtLoad),
// M17_NO_MOTION_KEY (SCFeatKeyInit), M17_NO_HANDLER_MARKER (SCLoadInit); M17_TRACKER_MOTION_SCOPE (L61) is parsed below
static void M16ParseEnv(void) {
    const char *s = getenv("M16_SLOW_ACTIVATE_MS");
    if (s) {
        gM16SlowMs = atol(s); const char *at = strchr(s, '@');
        if (at) { at++; for (int i = PH_TRIM; i <= PH_RESOLVE; i++) if (!strcmp(at, kActPhaseName[i])) gM16SlowAt = i; }
    }
    if ((s = getenv("M16_SLOW_SWAP_MS"))) gM16SlowSwapMs = atol(s);
    if ((s = getenv("M16_SLOW_START_MS"))) gM16SlowStartMs = atol(s);
    if ((s = getenv("M16_FIG_FAULT_AT_MS"))) gM16FigFaultMs = atol(s);
    if ((s = getenv("M16_ACTIVATE_FAIL"))) snprintf(gM16Fail, sizeof gM16Fail, "%s", s);
    gM16PfReal = getenv("M16_PF_FAIL_REAL") != NULL;
    gM16NeedsMain = getenv("M16_ACTIVATE_NEEDS_MAIN") != NULL;
    gM16Reenter = getenv("M16_REENTER") != NULL;
    gM16NoTimeout = getenv("M16_NO_ACTTIMEOUT") != NULL;
    gM16NoUIOrientAtLoad = getenv("M16_NO_UIORIENT_AT_LOAD") != NULL;   // L57: no UI orientation snapshot at load
    gM16MotionFault = getenv("M16_MOTION_FAULT") != NULL;
    if ((s = getenv("M16_ORIENT_FAULT")))
        gM16OrientFault = !strcmp(s, "start") ? ORF_START : !strcmp(s, "runmain") ? ORF_RUNMAIN : !strcmp(s, "ensure") ? ORF_ENSURE : ORF_UPD;
    gM17TrackerMotionScope = getenv("M17_TRACKER_MOTION_SCOPE") != NULL;
    gM17TrackStartKeepKey = getenv("M17_TRACKSTART_KEEP_KEY") != NULL;
    gM17EnsureKeepKey = getenv("M17_ENSURE_KEEP_KEY") != NULL;
    gM16TrapFault = getenv("M16_TRAP_FAULT") != NULL;
    gM17AppSendKeepKey = getenv("M17_APP_SEND_KEEP_KEY") != NULL;
}
static BOOL M16Fail(const char *what) { return !strcmp(gM16Fail, what); }
// at most one counted activate strike per launch, whatever fails (timeout, prepare throw, SWAP throw, START throw): each
// recorder takes this once flag first, and a recorder that loses it records nothing
static _Atomic int gActRecorded;
static BOOL ActStrikeOnce(void) { return !atomic_exchange(&gActRecorded, 1); }
// sliced sleep: 10 ms slices, counting only slices that took under 50 ms, so a SIGSTOP or a debugger stop doesn't use it up
static void M16Sleep(long ms) {   // counts the measured length of each slice (nanosleep overshoots ~1-2 ms per slice)
    uint64_t done = 0, want = (uint64_t)ms * NSEC_PER_MSEC;
    while (done < want) {
        uint64_t left = want - done, sl = left < 10 * NSEC_PER_MSEC ? left : 10 * NSEC_PER_MSEC;
        uint64_t a = ActNow(); struct timespec ts = {0, (long)sl}; nanosleep(&ts, NULL);
        uint64_t dt = ActNow() - a; if (dt < 50 * NSEC_PER_MSEC) done += dt;
    }
}
static void M16SlowAt(int phase) { if (gM16SlowMs > 0 && gM16SlowAt == phase) M16Sleep(gM16SlowMs); }

// ---- hooks: resolved in PREPARE 4, installed in SWAP (memory-only: no messages, no I/O, no dyld lock) ----
// A hook records its Method (or, for add-style hooks, the class to add on), its replacement and where the hook's original IMP
// lives. SWAP installs group by group (client, session, audio, video: plan §2.3 SWAP 2-5) and chains to the IMP that
// method_setImplementation / class_replaceMethod returned, so a swizzle made by someone else between PREPARE and SWAP is kept.
enum { HG_CLIENT = 0, HG_SESSION, HG_AUDIO, HG_VIDEO, HG_N };
typedef struct { const char *what; Class cls; SEL sel; Method m; IMP repl; IMP *orig; IMP pre; const char *types; BOOL add; int group; int rc; } ActHook;
#define ACT_MAXHOOKS 32
static ActHook gActHooks[ACT_MAXHOOKS]; static int gActNHooks;
static char gActMissing[160];   // the first required hook that wasn't found (PREPARE 4 -> STOCK)
// resolve one hook. add: the class's own method if it has one, else an override added on the class (class_replaceMethod),
// chaining to the inherited IMP. Returns NO when the method is missing (required -> STOCK).
static BOOL ActHookResolve(int group, const char *what, Class cls, const char *sel, BOOL meta, BOOL add, IMP repl, IMP *orig, BOOL required) {
    Class target = cls && meta ? object_getClass(cls) : cls;
    SEL s = sel_registerName(sel);
    Method m = target ? class_getInstanceMethod(target, s) : NULL;
    if (!m || gActNHooks >= ACT_MAXHOOKS) {
        if (required && !gActMissing[0]) snprintf(gActMissing, sizeof gActMissing, "%s: [%s %s]", what, target ? class_getName(target) : "nil", sel);
        return !required;
    }
    gActHooks[gActNHooks++] = (ActHook){what, target, s, m, repl, orig, method_getImplementation(m), method_getTypeEncoding(m), add, group, 0};
    return YES;
}
// SWAP: install one group. rc: 1 installed (chained to the IMP read in PREPARE), 2 installed (chained to a newer IMP)
static void ActHookSwapGroup(int group) {
    for (int i = 0; i < gActNHooks; i++) {
        ActHook *h = &gActHooks[i]; if (h->group != group) continue;
        if (h->orig) *h->orig = h->pre;   // before the hook is live, so a call through it never reads NULL
        IMP r = h->add ? class_replaceMethod(h->cls, h->sel, h->repl, h->types) : method_setImplementation(h->m, h->repl);
        if (!r) r = h->pre;   // class_replaceMethod added an override: chain to the inherited IMP read in PREPARE
        if (h->orig && r != h->pre) *h->orig = r;
        h->rc = r == h->pre ? 1 : 2;
    }
}
