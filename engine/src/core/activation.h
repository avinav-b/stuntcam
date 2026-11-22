// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the bounded activation's internal interface. core/act.inc includes it
// first. It declares the activation's state machine, its phases and the hook list, defines the activation's state
// (gActivation) and the test switches parsed at load, and declares the functions other fragments call.
//
// The state machine:
//   IDLE --first trapped call--> RUNNING --PREPARE OK, CAS--> COMMITTING (SWAP) --> ARMED (START: a separate work item)
//                                   |-- PREPARE fails (latch, preflight, exception, missing hook, forced) --> STOCK
//                                   `-- a waiter's deadline passes: CAS RUNNING -> STOCK --------------------> STOCK
//   A valid latched-<build> marker at load (sentinel on, no clear) starts the process in STOCK.
// ARMED and STOCK are terminal. Loads are acquire and the ARMED and STOCK stores are release, so a hook that sees
// ARMED sees every IMP and pointer SWAP wrote.
//
// Who owns what:
//   Load (the constructor, before any other engine thread runs) sets lazy, the budget and the queues.
//   The first trapped call's dispatch_once body sets the trigger, the start time and the deadline, then starts
//   PREPARE on gActivation.queues.activate. PREPARE and SWAP run there (or inline on the caller's thread in the test
//   modes), so the timing, the hook list and the exception names are written only there.
//   The waiters read state, phase and the deadline through atomics. The one waiter whose compare-and-swap wins
//   moves the deadline or records the timeout.
//   The record queue reads timeoutActivationNumber, exceptionName and the trigger after PREPARE wrote them.
#ifndef SIMCAM_ACTIVATION_H
#define SIMCAM_ACTIVATION_H

// The values of gActivation.state.
enum { kActivationIdle = 0, kActivationRunning, kActivationCommitting, kActivationArmed, kActivationStock };

// The step the activation is in (gActivation.phase), for the timeout record's phase=. kActivationPhaseNames (below)
// holds their names.
enum {
    kActivationPhaseStart = 0,
    kActivationPhaseTrim,
    kActivationPhaseSentinel,
    kActivationPhaseLookups,
    kActivationPhasePreflight,
    kActivationPhaseResolve,
    kActivationPhaseSwap,
    kActivationPhaseDone,
};

// ---- hooks: resolved in PREPARE's resolve step, installed in SWAP ----
// SWAP installs group by group, in this order.
enum { kHookGroupClient = 0, kHookGroupSession, kHookGroupAudio, kHookGroupVideo, kHookGroupCount };
// SWAP's result for one hook (ActivationHook.swapResult)
enum { kHookNotInstalled = 0, kHookChainedToPrepared = 1, kHookChainedToNewer = 2 };
typedef struct {
    const char *what;           // the hook's name in the swap: lines and in gActivation.hooks.missing
    Class cls;                  // the class (or metaclass) the method is on, or is added on
    SEL sel;
    Method method;              // the method read in the resolve step
    IMP replacement;
    IMP *origSlot;              // where the hook's original IMP lives (NULL: the hook doesn't chain)
    IMP preparedImp;            // the IMP read in the resolve step
    const char *typeEncoding;   // the method's type encoding (for class_replaceMethod)
    BOOL addsOverride;          // install with class_replaceMethod on cls, not method_setImplementation
    int group;                  // kHookGroup*
    int swapResult;             // kHookNotInstalled, kHookChainedToPrepared or kHookChainedToNewer
} ActivationHook;
#define kActivationMaxHooks 32

// The activation budget: SIMCAM_ACTIVATE_TIMEOUT_MS sets it within kActivationMinBudgetMs..kActivationMaxBudgetMs
// (the log and the warning in ActivationReadBudget spell out these numbers). The default is scaled up under a debugger
// or sanitizers.
#define kActivationDefaultBudgetMs 1000
#define kActivationMinBudgetMs 1
#define kActivationMaxBudgetMs 60000
#define kActivationSlowedBudgetScale 10

struct Activation {
    _Atomic int state;       // kActivationIdle ... kActivationStock (read with ActivationState)
    _Atomic int preparing;   // PREPARE is running: the minimal crash handler attributes faults to `activate` only then
    _Atomic int phase;       // kActivationPhase*
    int lazy;                // SIMCAM_LAZY: 1 lazy arming (the default), 0 the phases inline at load; -1 before load
    BOOL runsInline;         // the phases ran on the caller's thread (SIMCAM_OFF_ACTIVATION_TIMEOUT, SIMCAM_LAZY=0)

    // The budget in use, and the deadline the waiters give up at.
    struct {
        long ms;
        BOOL isExplicit;               // set by SIMCAM_ACTIVATE_TIMEOUT_MS: never scaled
        _Atomic uint64_t deadlineNs;   // CLOCK_MONOTONIC
    } budget;

    // CLOCK_MONOTONIC ns: when activation began (the first trapped call, or load with SIMCAM_LAZY=0), when each phase
    // began, when SWAP began, and when the camera armed.
    struct {
        uint64_t beganNs;
        uint64_t phaseBeganNs[kActivationPhaseDone + 1];
        uint64_t swapBeganNs;
        uint64_t armedNs;
    } timing;

    // simcam.activate runs PREPARE and START; simcam.record writes the activation's log lines and event records.
    struct {
        dispatch_queue_t activate;
        dispatch_queue_t record;
        dispatch_group_t waiters;   // entered while PREPARE and SWAP run; the waiters wait on it
        dispatch_once_t once;       // the first trapped call's once body
    } queues;

    // The call that triggered activation: its name, its queue's label, whether it was on main, where it came from.
    struct {
        char name[96];
        char queueLabel[96];
        BOOL onMain;
        void *returnAddress;
    } trigger;

    // The lines logged only once.
    struct {
        _Atomic int stock;            // the marker-STOCK line
        _Atomic int reentry;          // a re-entrant call forwarded to stock
        _Atomic int swapOverrun;      // SWAP overran the budget
        _Atomic int mainTrackerRun;   // the first armed main-thread call ran the orientation tracker
    } noted;

    char exceptionName[128];       // what PREPARE or START threw
    char swapExceptionName[128];   // what SWAP threw (core/sentinel_activate.inc records it at START)
    // At most one counted activate strike per launch, whatever fails (timeout, PREPARE throw, SWAP throw, START
    // throw): each recorder takes this once-flag first (ActivationStrikeOnce), and a recorder that loses it records
    // nothing.
    _Atomic int strikeRecorded;
    _Atomic int timeoutActivationNumber;   // the activation number the record queue's timeout line used (0 = none)

    // The hooks resolved in PREPARE (reset by its resolve step) and installed by SWAP.
    struct {
        ActivationHook list[kActivationMaxHooks];
        int count;
        char missing[160];   // the first required hook that wasn't found (the resolve step then goes STOCK)
    } hooks;
};

// ---- the activation's state

// The activation's state machine and timing. struct Activation says who writes each field.
static struct Activation gActivation = {.lazy = -1, .budget.ms = kActivationDefaultBudgetMs};

// set on the activation thread while ActivationRun runs (the primary re-entrancy detector)
static __thread int gActivationThread;

// the phases' names, as the timeout record's phase= and SIMCAM_TEST_SLOW_ACTIVATE_MS spell them
static const char *kActivationPhaseNames[] = {
    [kActivationPhaseStart] = "start",
    [kActivationPhaseTrim] = "trim",
    [kActivationPhaseSentinel] = "sentinel",
    [kActivationPhaseLookups] = "lookups",
    [kActivationPhasePreflight] = "preflight",
    [kActivationPhaseResolve] = "resolve",
    [kActivationPhaseSwap] = "swap",
    [kActivationPhaseDone] = "done",
};

// ---- the activation's off switches and test faults (SIMCAM_OFF_* / SIMCAM_TEST_*)
// ActivationParseTestEnv (core/act.inc) sets them once at load; nothing calls setenv after load.

static BOOL gTestPreflightBreak;   // SIMCAM_PF_BREAK

// SIMCAM_TEST_SLOW_ACTIVATE_MS=<n>[@trim|@sentinel|@lookups|@preflight|@resolve]
static long gTestSlowActivateMs;
static int gTestSlowActivateAt = kActivationPhasePreflight;
static long gTestSlowSwapMs;    // SIMCAM_TEST_SLOW_SWAP_MS
static long gTestSlowStartMs;   // SIMCAM_TEST_SLOW_START_MS

// SIMCAM_TEST_FIG_FAULT_AT_MS: a CoreMedia fault on a global queue n ms after the first call
static long gTestFigFaultAtMs;

// SIMCAM_TEST_ACTIVATE_FAIL=preflight|throw|missing|crash|crashfig|swapthrow|crashstart|crash2
// (swapthrow: SWAP throws, and START throws too, at its end; crashstart and crash2: a fault in START on one or two
// threads)
static char gTestActivateFail[16];

static BOOL gTestPfFailReal;           // SIMCAM_TEST_PF_FAIL_REAL
static BOOL gTestActivateNeedsMain;    // SIMCAM_TEST_ACTIVATE_NEEDS_MAIN
static BOOL gTestReenter;              // SIMCAM_TEST_REENTER
static BOOL gOffActivationTimeout;     // SIMCAM_OFF_ACTIVATION_TIMEOUT
static BOOL gOffUIOrientationAtLoad;   // SIMCAM_OFF_UI_ORIENTATION_AT_LOAD

// The CoreMotion and orientation-tracker switches.
static BOOL gTestMotionFault;   // SIMCAM_TEST_MOTION_FAULT=1: a fault in a motion tick, 1 s after the first

// SIMCAM_TEST_ORIENT_FAULT: a fault in the orientation tracker, which must count as camera:
//   1        in upd, once ARMED with a RotationCoordinator, after the coordinators' KVO
//   start    in OrientTrackStart, after its key clear (the first motion start or data call), once ARMED. It waits up
//            to 5 s for another thread to arm the camera: the race it stands for is a camera armed while main is in
//            that call.
//   runmain  in OrientTrackRunMain's start branch, before upd (reached from OrientTrackStart on main), once ARMED
//            (with the same wait)
//   ensure   ActivationEnsure's main-thread tracker run starts with the motion key set, as if the first camera call
//            came from inside a motion entry point (ActivationEnsure clears the key for that run), then faults like
//            runmain
enum { kOrientFaultNone = 0, kOrientFaultUpd, kOrientFaultStart, kOrientFaultRunMain, kOrientFaultEnsure };
static int gTestOrientFault;

// The orientation tracker is camera code, so a fault in it must count against the camera, not CoreMotion: it clears
// the motion key wherever it can be reached from a motion entry point. Each switch below keeps the key in one place
// (the mutant that shows the clear is needed).
// SIMCAM_OFF_TRACKER_KEY_CLEAR=1: upd runs under the motion key
static BOOL gOffTrackerKeyClear;
static BOOL gOffTrackStartKeyClear;    // SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1: OrientTrackStart keeps the key
// SIMCAM_OFF_ENSURE_KEY_CLEAR=1: ActivationEnsure's tracker run keeps the key
static BOOL gOffEnsureKeyClear;

// SIMCAM_TEST_TRAP_FAULT=1: a fault in our image in the first camera call's trap (ActivationEnsure), once ARMED, before
// its main-thread tracker run. That is camera code, reached from whatever app code made that call; its line names the
// feature key the thread carried.
static BOOL gTestTrapFault;

// SIMCAM_OFF_APP_SEND_KEY_CLEAR=1: client/motion.inc's sends that app code can answer (an interval getter, the
// handler copy, addOperationWithBlock:) keep the motion key instead of clearing it around the app's code, so a camera
// fault that app code leads to would count against CoreMotion.
static BOOL gOffAppSendKeyClear;

// ---- functions other fragments call (defined in core/act.inc unless the line says otherwise)

// The state machine (core/lazy.inc, core/sentinel_activate.inc, client/).
static int ActivationState(void);
static BOOL ActivationIsArmed(void);
static BOOL ActivationStrikeOnce(void);

// The activation's steps and their test faults (core/lazy.inc, the sentinel fragments).
static void ActivationParseTestEnv(void);
static void ActivationTestSlowAt(int phase);
static BOOL ActivationTestFailIs(const char *what);
static void TestFaultNullWrite(int value);
static uint64_t MonotonicNowNs(void);
static void SlicedSleepMs(long ms);

// The hook list: each fragment that resolves a hook adds it here; core/lazy.inc's SWAP installs them group by group.
static BOOL ActivationHookResolve(int group, const char *what, Class cls, const char *selName, BOOL meta,
                                  BOOL addsOverride, IMP replacement, IMP *origSlot, BOOL required);
static void ActivationHookSwapGroup(int group);

static void ActivationMotionStockNote(void);   // client/motion.inc (defined in core/lazy.inc)

#endif
