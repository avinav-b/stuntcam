// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): bounded activation, shared state (design §4.1
// "Activation"; the state machine and phases are in core/lazy.inc).
//
//   IDLE --first trapped call--> RUNNING --prepare OK, CAS--> COMMITTING (SWAP) --> ARMED (START: a separate work item)
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
// set on the activation thread while ActRun runs (primary re-entrancy detector)
static __thread int gActThread;
// prepare is running: the minimal crash handler attributes faults to `activate` only while this is set
static _Atomic int gActRunning;
// the step activation is in, for the timeout record's phase=
enum { PH_START = 0, PH_TRIM, PH_SENTINEL, PH_LOOKUPS, PH_PREFLIGHT, PH_RESOLVE, PH_SWAP, PH_DONE };
static const char *kActPhaseName[] = {"start", "trim", "sentinel", "lookups", "preflight", "resolve", "swap", "done"};
static _Atomic int gActPhase;
// computed once at load (no setenv after load)
static BOOL gActFakeBreak;              // SIMCAM_PF_BREAK
static uint64_t ActNow(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

// ---- activation off switches and test faults (SIMCAM_OFF_* / SIMCAM_TEST_*, parsed at load; ledger L56, L57) ----
// SIMCAM_TEST_SLOW_ACTIVATE_MS=<n>[@trim|@sentinel|@lookups|@preflight|@resolve]
static long gTestSlowActivateMs;
static int gTestSlowActivateAt = PH_PREFLIGHT;
static long gTestSlowSwapMs, gTestSlowStartMs;   // SIMCAM_TEST_SLOW_SWAP_MS, SIMCAM_TEST_SLOW_START_MS
// SIMCAM_TEST_FIG_FAULT_AT_MS: a CoreMedia fault on a global queue n ms after the first call
static long gTestFigFaultAtMs;
// SIMCAM_TEST_ACTIVATE_FAIL=preflight|throw|missing|crash|crashfig|swapthrow|crashstart|crash2
// (swapthrow: SWAP throws, and START throws too, at its end; crashstart, crash2: a fault in START on 1 / 2 threads)
static char gTestActivateFail[16];
static BOOL gTestPfFailReal, gTestActivateNeedsMain, gTestReenter, gOffActivationTimeout, gOffUIOrientationAtLoad;
static BOOL gTestMotionFault;   // SIMCAM_TEST_MOTION_FAULT=1: a fault in a motion tick, 1 s after the first
// SIMCAM_TEST_ORIENT_FAULT: a fault in the orientation tracker, which must count as camera:
//   1        in upd, once ARMED with a RotationCoordinator, after the coordinators' KVO
//   start    in OrientTrackStart, after its key clear (the first motion start or data call), once ARMED (it waits up
//            to 5 s for another thread to arm the camera: the race it stands for is a camera armed while main is in
//            that call)
//   runmain  in OrientTrackRunMain's start branch, before upd (reached from OrientTrackStart on main), once ARMED
//            (same wait)
//   ensure   ActEnsure's main-thread tracker run starts with the motion key set, as if the first camera call came from
//            inside a motion entry point (ActEnsure clears the key for that run; ledger L63), then faults like runmain
enum { ORF_NONE = 0, ORF_UPD, ORF_START, ORF_RUNMAIN, ORF_ENSURE };
static int gTestOrientFault;
// SIMCAM_OFF_TRACKER_KEY_CLEAR=1 (ledger L61's mutant): upd runs under the motion key
static BOOL gOffTrackerKeyClear;
static BOOL gOffTrackStartKeyClear;    // SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1 (ledger L62): OrientTrackStart keeps the key
// SIMCAM_OFF_ENSURE_KEY_CLEAR=1 (ledger L63): ActEnsure's tracker run keeps the key
static BOOL gOffEnsureKeyClear;
// SIMCAM_TEST_TRAP_FAULT=1: a fault in our image in the first camera call's trap (ActEnsure), once ARMED,
// before its main-thread tracker run: camera code, reached from whatever app code made that call. Its line names the
// feature key the thread carried. SIMCAM_OFF_APP_SEND_KEY_CLEAR=1 (ledger L65): motion.inc's sends that app code can
// answer (an interval getter, the handler copy, addOperationWithBlock:) keep the motion key instead of clearing it
// around the app's code
static BOOL gTestTrapFault, gOffAppSendKeyClear;
// CoreMotion off switches (ledger L58-L60) are read where they act: SIMCAM_OFF_MOTION_MARKERS (SCMotionStockAtLoad),
// SIMCAM_OFF_MOTION_KEY (SCFeatKeyInit), SIMCAM_OFF_HANDLER_MOTION_MARKER (SCLoadInit); SIMCAM_OFF_TRACKER_KEY_CLEAR
// (L61) is parsed below
static void ActParseTestEnv(void) {
    const char *s = getenv(ENV_TEST_SLOW_ACTIVATE_MS);
    if (s) {
        gTestSlowActivateMs = atol(s);
        const char *at = strchr(s, '@');
        if (at) {
            at++;
            for (int i = PH_TRIM; i <= PH_RESOLVE; i++)
                if (!strcmp(at, kActPhaseName[i])) gTestSlowActivateAt = i;
        }
    }
    if ((s = getenv(ENV_TEST_SLOW_SWAP_MS))) gTestSlowSwapMs = atol(s);
    if ((s = getenv(ENV_TEST_SLOW_START_MS))) gTestSlowStartMs = atol(s);
    if ((s = getenv(ENV_TEST_FIG_FAULT_AT_MS))) gTestFigFaultAtMs = atol(s);
    if ((s = getenv(ENV_TEST_ACTIVATE_FAIL))) snprintf(gTestActivateFail, sizeof gTestActivateFail, "%s", s);
    gTestPfFailReal = getenv(ENV_TEST_PF_FAIL_REAL) != NULL;
    gTestActivateNeedsMain = getenv(ENV_TEST_ACTIVATE_NEEDS_MAIN) != NULL;
    gTestReenter = getenv(ENV_TEST_REENTER) != NULL;
    gOffActivationTimeout = getenv(ENV_OFF_ACTIVATION_TIMEOUT) != NULL;
    // L57: no UI orientation snapshot at load
    gOffUIOrientationAtLoad = getenv(ENV_OFF_UI_ORIENTATION_AT_LOAD) != NULL;
    gTestMotionFault = getenv(ENV_TEST_MOTION_FAULT) != NULL;
    if ((s = getenv(ENV_TEST_ORIENT_FAULT)))
        gTestOrientFault = !strcmp(s, "start")     ? ORF_START
                          : !strcmp(s, "runmain") ? ORF_RUNMAIN
                          : !strcmp(s, "ensure")  ? ORF_ENSURE
                                                  : ORF_UPD;
    gOffTrackerKeyClear = getenv(ENV_OFF_TRACKER_KEY_CLEAR) != NULL;
    gOffTrackStartKeyClear = getenv(ENV_OFF_TRACKSTART_KEY_CLEAR) != NULL;
    gOffEnsureKeyClear = getenv(ENV_OFF_ENSURE_KEY_CLEAR) != NULL;
    gTestTrapFault = getenv(ENV_TEST_TRAP_FAULT) != NULL;
    gOffAppSendKeyClear = getenv(ENV_OFF_APP_SEND_KEY_CLEAR) != NULL;
}
static BOOL ActTestFailIs(const char *what) { return !strcmp(gTestActivateFail, what); }
// test faults only: a write of value through a null pointer, a SIGSEGV whose PC is in our image. The pointer comes
// from atoi("0") so the compiler can't prove it null and drop the write.
static void TestFaultNullWrite(int value) {
    volatile int *z = (volatile int *)(uintptr_t)atoi("0");
    *z = value;
}
// at most one counted activate strike per launch, whatever fails (timeout, prepare throw, SWAP throw, START throw):
// each recorder takes this once flag first, and a recorder that loses it records nothing
static _Atomic int gActRecorded;
static BOOL ActStrikeOnce(void) { return !atomic_exchange(&gActRecorded, 1); }
// sliced sleep: 10 ms slices, counting only slices that took under 50 ms, so a SIGSTOP or a debugger stop doesn't use
// it up. Counts the measured length of each slice (nanosleep overshoots ~1-2 ms per slice).
static void ActSleepMs(long ms) {
    uint64_t done = 0, want = (uint64_t)ms * NSEC_PER_MSEC;
    while (done < want) {
        uint64_t left = want - done;
        uint64_t slice = left < 10 * NSEC_PER_MSEC ? left : 10 * NSEC_PER_MSEC;
        uint64_t sliceStart = ActNow();
        struct timespec ts = {0, (long)slice};
        nanosleep(&ts, NULL);
        uint64_t slept = ActNow() - sliceStart;
        if (slept < 50 * NSEC_PER_MSEC) done += slept;
    }
}
static void ActTestSlowAt(int phase) {
    if (gTestSlowActivateMs > 0 && gTestSlowActivateAt == phase) ActSleepMs(gTestSlowActivateMs);
}

// ---- hooks: resolved in PREPARE 4, installed in SWAP (memory-only: no messages, no I/O, no dyld lock) ----
// A hook records its Method (or, for add-style hooks, the class to add on), its replacement and where the hook's
// original IMP lives. SWAP installs group by group (client, session, audio, video: SWAP 2-5) and chains to the IMP that
// method_setImplementation / class_replaceMethod returned, so a swizzle made by someone else between PREPARE and SWAP
// is kept.
enum { HG_CLIENT = 0, HG_SESSION, HG_AUDIO, HG_VIDEO, HG_N };
typedef struct {
    const char *what;    // the hook's name in the swap: lines and in gActMissing
    Class cls;           // the class (or metaclass) the method is on, or is added on
    SEL sel;
    Method m;            // the method read in PREPARE 4
    IMP repl;            // the replacement
    IMP *orig;           // where the hook's original IMP lives (NULL: the hook doesn't chain)
    IMP pre;             // the IMP read in PREPARE 4
    const char *types;   // the method's type encoding (class_replaceMethod)
    BOOL add;            // install with class_replaceMethod on cls, not method_setImplementation
    int group;           // HG_*
    int rc;              // SWAP's result: 0 not installed, 1 chained to pre, 2 chained to a newer IMP
} ActHook;
#define ACT_MAXHOOKS 32
static ActHook gActHooks[ACT_MAXHOOKS];
static int gActNHooks;
static char gActMissing[160];   // the first required hook that wasn't found (PREPARE 4 -> STOCK)
// resolve one hook. add: the class's own method if it has one, else an override added on the class
// (class_replaceMethod), chaining to the inherited IMP. Returns NO when the method is missing (required -> STOCK).
static BOOL ActHookResolve(int group, const char *what, Class cls, const char *sel, BOOL meta, BOOL add, IMP repl,
                           IMP *orig, BOOL required) {
    Class target = cls && meta ? object_getClass(cls) : cls;
    SEL s = sel_registerName(sel);
    Method m = target ? class_getInstanceMethod(target, s) : NULL;
    if (!m || gActNHooks >= ACT_MAXHOOKS) {
        if (required && !gActMissing[0])
            snprintf(gActMissing, sizeof gActMissing, "%s: [%s %s]", what, target ? class_getName(target) : "nil", sel);
        return !required;
    }
    gActHooks[gActNHooks++] = (ActHook){what, target, s, m, repl, orig, method_getImplementation(m),
                                        method_getTypeEncoding(m), add, group, 0};
    return YES;
}
// SWAP: install one group. rc: 1 installed (chained to the IMP read in PREPARE), 2 installed (chained to a newer IMP)
static void ActHookSwapGroup(int group) {
    for (int i = 0; i < gActNHooks; i++) {
        ActHook *h = &gActHooks[i];
        if (h->group != group) continue;
        if (h->orig) *h->orig = h->pre;   // before the hook is live, so a call through it never reads NULL
        IMP replaced = h->add ? class_replaceMethod(h->cls, h->sel, h->repl, h->types)
                              : method_setImplementation(h->m, h->repl);
        // class_replaceMethod added an override: chain to the inherited IMP read in PREPARE
        if (!replaced) replaced = h->pre;
        if (h->orig && replaced != h->pre) *h->orig = replaced;
        h->rc = replaced == h->pre ? 1 : 2;
    }
}
