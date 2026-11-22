// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): lazy arming with bounded activation. Must stay last.
//
// Provides: ActivationMotionStockNote (client/motion.inc). Its constructor, ActivationInstallAtLoad, runs everything
//           else: the load-time steps, then the activation phases (PREPARE, SWAP, START), which call each fragment's
//           steps.
// Owns:     nothing shared (the activation's timing and bookkeeping, in core/activation.h, are written here).
// Uses:     every core/ fragment, fig/session_hook.inc, every client/ fragment
//
// At load: traps on the public capture entry points and the CoreMotion stand-in's hooks; no preflight, no private call.
// The first trapped call starts activation once, on the private serial queue simcam.activate. Every caller waits at
// most SIMCAM_ACTIVATE_TIMEOUT_MS (default 1000; 10 times that under a debugger or sanitizers) from that first call,
// against one shared deadline, and then forwards to stock for the rest of the process (the state machine is in
// core/act.inc).
//   PREPARE  the sentinel (a latch makes the launch STOCK before any preflight), the lookups, the preflight and the
//            resolve step. It may do I/O and send messages, but installs nothing visible to the process except the
//            minimal crash handler.
//   SWAP     the installs (memory only: no messages, no I/O, no dyld lock). Callers wait for it as for a lock and
//            never time out.
//   START    a separate work item after ARMED: the sentinel goes live, then observers, timers and logging.
// Apps that never touch the camera are never armed. SIMCAM_LAZY=0 runs the same phases inline at load (no traps, no
// timeout). That is a test mode: it runs inside dlopen with the dyld lock held, where a private queue would always time
// out.
// Trampolines take void * arguments: strong id parameters made ARC retain argument registers the real method never set
// (30 of 50 test launches crashed).
#import <objc/runtime.h>
#include "activation.h"
#include "sentinel.h"
#include "../fig/session_hook.h"
#include "../client/motion.h"
#include "../client/rotation.h"
#include "../client/vision.h"
#include "trace.h"
#include "control.h"
#include "init.h"
#include "preflight.h"
#include "../client/photo.h"
#include "../client/output_settings.h"

// ---- activation state ----
// gActivation (core/activation.h) holds the budget and deadline, the phase timings, the queues and the first caller.
// dispatch_queue_set_specific's key on simcam.activate (the secondary re-entrancy check): only its address is used.
static char kActivationQueueKey;
// the longest feature list read from a marker file
#define kActivationMarkerTextMax 200

static long ActivationBudget(void) { return gActivation.budget.ms; }   // the effective budget (set by the once body)

// ---- the record queue (simcam.record, serial) ----
// Everything the engine writes about activation goes through it, so callers never do I/O.
static void ActivationRecordLogF(void *ctx) {
    @autoreleasepool {
        char *line = ctx;
        OpenLog();
        LOG(@"%s", line);
        free(line);
    }
}

static void ActivationRecordStderrF(void *ctx) {
    @autoreleasepool {
        char *line = ctx;
        OpenLog();
        fprintf(stderr, "%s\n", line);
        LOG(@"%s", line);
        free(line);
    }
}

static void ActivationRecordLog(BOOL toStderr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void ActivationRecordLog(BOOL toStderr, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *line = NULL;
    vasprintf(&line, fmt, args);
    va_end(args);
    if (line) {
        dispatch_async_f(gActivation.queues.record, line, toStderr ? ActivationRecordStderrF : ActivationRecordLogF);
    }
}

// the timeout record: exactly one line per process (guarded by ActivationStrikeOnce); publishes the activation
// number it used
static void ActivationRecordTimeoutF(void *ctx) {
    @autoreleasepool {
        long phase = (long)ctx;
        char detail[96];
        snprintf(detail, sizeof detail, "ms=%ld phase=%s", ActivationBudget(), kActivationPhaseNames[phase]);
        BOOL traced = atomic_load(&gSentinel.traced);
        int activation = SentinelRecordEvent(traced ? "unknown-hang" : "hang", "activate", detail);
        if (activation) {
            int none = 0;
            atomic_compare_exchange_strong(&gActivation.timeoutActivationNumber, &none, activation);
        }
        OpenLog();
        LOG(@"activation: %s activate recorded (activation %d, %s)%s",
            traced ? "unknown-hang (traced, not counted)" : "hang", activation, detail,
            activation ? "" : " — could not write events.log");
    }
}

static void ActivationRecordExceptionF(void *ctx) {
    @autoreleasepool {
        char detail[160];
        snprintf(detail, sizeof detail, "exception=%s", gActivation.exceptionName);
        int activation =
            SentinelRecordEvent(atomic_load(&gSentinel.traced) ? "crash-traced" : "crash", "activate", detail);
        OpenLog();
        LOG(@"activation: crash activate %s recorded (activation %d)", detail, activation);
    }
}

// ---- the marker-STOCK line (stock is never silent: a launch without a camera always says why) ----
// The features and the until come from one open file (fstat on the read's fd): a stat by path could see a marker that
// another process renamed into place after the read.
static void ActivationMarkerNoteF(void *ctx) {
    char features[kActivationMarkerTextMax] = "?";
    char when[64] = "?";
    int fd = open(gSentinel.atLoad.latchMarker, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        ssize_t bytesRead = read(fd, features, sizeof features - 1);
        if (bytesRead > 0) {
            features[bytesRead] = 0;
            char *newline = strchr(features, '\n');
            if (newline) {
                *newline = 0;
            }
        }
        struct stat markerStat;
        if (fstat(fd, &markerStat) == 0) {
            struct tm localTime;
            localtime_r(&markerStat.st_mtime, &localTime);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &localTime);
        }
        close(fd);
    }

    char *line = NULL;
    asprintf(&line, "simcam: %s latched until %s; no camera. Relaunch with SIMCAM_SENTINEL=clear to retry (marker %s)",
             features, when, gSentinel.atLoad.latchMarker);
    if (line) {
        ActivationRecordStderrF(line);
    }
}

// ---- the motion-stock line (stock is never silent) ----
// Queued by the first call into a CoreMotion forwarding trap. The until comes from the load-time stat
// (SentinelMotionStockAtLoad), and so does the kind: by this first motion call, a passing preflight or the sentinel
// step's marker update may have unlinked the marker. Its feature list is read only from the version that the load-time
// stat saw (same inode and mtime); a rewritten marker (another until) gives the kind instead, so the features and the
// until always come from one snapshot.
static void ActivationMotionStockNoteF(void *ctx) {
    const char *marker = gSentinel.atLoad.motionStockMarker;
    char features[kActivationMarkerTextMax];
    snprintf(features, sizeof features, "%s", gSentinel.atLoad.motionStockKind ?: "?");
    int fd = open(marker, O_RDONLY | O_CLOEXEC);
    struct stat markerStat;
    BOOL opened = fd >= 0 && fstat(fd, &markerStat) == 0;
    BOOL sameVersion = opened && markerStat.st_ino == gSentinel.atLoad.motionStockInode &&
                       markerStat.st_mtime == gSentinel.atLoad.motionStockUntil;
    if (sameVersion) {
        char firstLine[kActivationMarkerTextMax];
        ssize_t bytesRead = read(fd, firstLine, sizeof firstLine - 1);
        if (bytesRead > 0) {
            firstLine[bytesRead] = 0;
            char *newline = strchr(firstLine, '\n');
            if (newline) {
                *newline = 0;
            }
            if (firstLine[0]) {
                snprintf(features, sizeof features, "%s", firstLine);
            }
        }
    }

    if (fd >= 0) {
        close(fd);
    }
    char when[64] = "?";
    time_t until = gSentinel.atLoad.motionStockUntil;

    if (until) {
        struct tm localTime;
        localtime_r(&until, &localTime);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &localTime);
    }

    const char *base = strrchr(marker, '/');
    base = base ? base + 1 : marker;
    char *line = NULL;
    asprintf(&line,
             "simcam: CoreMotion is stock this launch (%s: %s until %s). Relaunch with SIMCAM_SENTINEL=clear to "
             "retry",
             base, features, when);
    if (line) {
        ActivationRecordStderrF(line);
    }
}

static void ActivationMotionStockNote(void) {
    dispatch_async_f(gActivation.queues.record, NULL, ActivationMotionStockNoteF);
}

// ---- PREPARE (on simcam.activate) ----
// ActivationPrepare's results: armed path (kPrepareOk) or why the launch stays stock
enum {
    kPrepareOk = 0,
    kPrepareLatched,
    kPreparePreflightFailed,
    kPrepareHookMissing,
    kPrepareThrew,
    kPreparePreflightForced
};

static void ActivationEnterPhase(int phase) {
    gActivation.timing.phaseBeganNs[phase] = MonotonicNowNs();
    atomic_store(&gActivation.phase, phase);
}

// Test faults in START (armed, the full crash handler installed, the trace ring on, its directory picked, no flush
// yet): SIMCAM_TEST_ACTIVATE_FAIL=crashstart faults here, on the activation queue; =crash2 faults on two threads at
// once (both released by one store, both PCs in our image), so two threads run the crash handler together. The trace
// must still be written, once and whole.
#define kActivationCrashThreadsMax 2
#define kActivationCrashReleaseDelayUs 20000   // lets both threads reach their spin loop before the store releases them
#define kActivationCrashWaitSliceUs 100000
static _Atomic int gTestCrashGo;

static void *ActivationTestCrashThread(void *ctx) {
    while (!atomic_load(&gTestCrashGo)) {}
    TestFaultNullWrite((int)(intptr_t)ctx);
    return NULL;
}

static void ActivationTestCrashInStart(int threadCount) {
    LOG(@"SIMCAM_TEST_ACTIVATE_FAIL=%s: fault in START on %d thread(s)", threadCount > 1 ? "crash2" : "crashstart",
        threadCount);
    if (threadCount == 1) {
        TestFaultNullWrite(1);
    }
    pthread_t threads[kActivationCrashThreadsMax];
    for (int i = 0; i < threadCount && i < kActivationCrashThreadsMax; i++) {
        // a test fault: a thread that doesn't start just doesn't fault
        (void)pthread_create(&threads[i], NULL, ActivationTestCrashThread, (void *)(intptr_t)(i + 1));
    }
    usleep(kActivationCrashReleaseDelayUs);
    atomic_store(&gTestCrashGo, 1);
    for (;;) {
        usleep(kActivationCrashWaitSliceUs);   // the process ends in the handler's chain
    }
}

// an address no CoreMedia object lives at: CMBaseObjectGetVTable faults on it
#define kActivationInvalidFigObjectAddress 0x10

// SIMCAM_TEST_ACTIVATE_FAIL=crashfig: a CoreMedia call on an invalid object, on a global queue
static void ActivationCrashFigF(void *ctx) {
    const void *(*getVTable)(const void *) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    if (getVTable) {
        getVTable((const void *)(uintptr_t)kActivationInvalidFigObjectAddress);
    }
}

static void ActivationNop(void *ctx) {}

// SIMCAM_TEST_FIG_FAULT_AT_MS (act-fake-sdk): a related-image (CoreMedia) fault, independent of PREPARE
static void ActivationFigFaultF(void *ctx) {
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_FIG_FAULT_AT_MS: CoreMedia fault now, %.0f ms after the first call\n",
            getpid(), (MonotonicNowNs() - gActivation.timing.beganNs) / 1e6);
    ActivationCrashFigF(NULL);
}

// PREPARE's lookups step: the dlopens, dlsyms, the profile plist, the log, SIMCAM_DIAG_DUMP_KEYS, the control file
static void ActivationLookups(void) {
    ActivationEnterPhase(kActivationPhaseLookups);
    ActivationTestSlowAt(kActivationPhaseLookups);
    ActivationLoadFrameworks();
    ControlReadAtPrepare();
    SentinelCacheRanges();   // again, after the dlopens
    LOG(@"TIMING gate entered %.1f ms after process start (%.1f ms after the first call)",
        (CFAbsoluteTimeGetCurrent() - ProcessStartTime()) * 1000,
        (MonotonicNowNs() - gActivation.timing.beganNs) / 1e6);
}

// A test switch is set that makes the preflight fail, or changes what it accepts: its failure is "forced" and writes
// no preflight-fail marker (unless SIMCAM_TEST_PF_FAIL_REAL).
static BOOL PreflightForcedByTest(void) {
    return gTestPreflightBreak || ActivationTestFailIs("preflight") || TestFaultOn(ENV_SLOT_COUNTS) ||
           TestFaultOn(ENV_PF_NOSYMOK);
}

// PREPARE's preflight step: kPrepareOk, or kPreparePreflightFailed / kPreparePreflightForced (a test switch made it
// fail)
static int ActivationPreflight(void) {
    ActivationEnterPhase(kActivationPhasePreflight);
    ActivationTestSlowAt(kActivationPhasePreflight);
    uint64_t preflightStart = MonotonicNowNs();
    BOOL forced = PreflightForcedByTest();
    BOOL ok = ActivationTestFailIs("preflight") ? NO : PreflightRun();
    LOG(@"preflight took %.1f ms", (MonotonicNowNs() - preflightStart) / 1e6);
    if (!ok) {
        // a forced preflight failure never writes preflight-fail, unless SIMCAM_TEST_PF_FAIL_REAL asks for it
        BOOL forcedOnly = forced && !gTestPfFailReal;
        if (!forcedOnly) {
            SentinelPreflightMarker(NO, "preflight failed");
        }
        return forcedOnly ? kPreparePreflightForced : kPreparePreflightFailed;
    }
    SentinelPreflightMarker(YES, NULL);
    return kPrepareOk;
}

#define kActivationCrashFigWaitMs 500   // how long PREPARE waits for the crashfig fault (well under the budget)

// the resolve step's test faults, before anything is resolved (SIMCAM_TEST_ACTIVATE_FAIL=throw|crash|crashfig,
// SIMCAM_TEST_ACTIVATE_NEEDS_MAIN, SIMCAM_TEST_REENTER, SIMCAM_TEST_SLOW_ACTIVATE_MS=<n>@resolve)
static void ActivationResolveTestFaults(void) {
    if (ActivationTestFailIs("throw")) {
        @throw [NSException exceptionWithName:@"SimcamTestActivateThrow"
                                       reason:@"SIMCAM_TEST_ACTIVATE_FAIL=throw"
                                     userInfo:nil];
    }
    if (ActivationTestFailIs("crash")) {
        TestFaultNullWrite(1);
    }

    // the fault's PC is in CoreMedia, on another thread: wait for it
    if (ActivationTestFailIs("crashfig")) {
        dispatch_async_f(dispatch_get_global_queue(0, 0), NULL, ActivationCrashFigF);
        SlicedSleepMs(kActivationCrashFigWaitMs);
    }

    if (gTestActivateNeedsMain) {
        LOG(@"SIMCAM_TEST_ACTIVATE_NEEDS_MAIN: prepare waits on the main queue");
        dispatch_sync_f(dispatch_get_main_queue(), NULL, ActivationNop);
    }

    if (gTestReenter) {
        LOG(@"SIMCAM_TEST_REENTER: prepare calls a trapped entry point");
        (void)[[AVCaptureSession alloc] init];
    }

    ActivationTestSlowAt(kActivationPhaseResolve);
}

// SIMCAM_TEST_ACTIVATE_FAIL=missing: a required hook on a method that doesn't exist (NO: activation fails, stock)
static BOOL ActivationResolveTestMissingHook(void) {
    if (!ActivationTestFailIs("missing")) {
        return YES;
    }
    return ActivationHookResolve(kHookGroupClient, "SIMCAM_TEST_ACTIVATE_FAIL=missing",
                                 objc_getClass("AVCaptureSession"), "simcamTestNoSuchMethod", NO, NO,
                                 (IMP)ActivationNop, NULL, YES);
}

// PREPARE's resolve step: every hook's Method and replacement (SWAP installs them). Returns NO if a required one is
// missing.
static BOOL ActivationResolveHooks(void) {
    gActivation.hooks.count = 0;
    gActivation.hooks.missing[0] = 0;
    BOOL allRequired = YES;
    DiagResolveSessionHooks();
    DiagResolveMetadataHooks();
    allRequired = PhotoResolveDataFix() && allRequired;
    allRequired = CoordResolveHooks() && allRequired;
    allRequired = PreviewResolveRotation() && allRequired;
    allRequired = RecSettingsResolveFix() && allRequired;
    VisionResolveFix();
    PhotoResolveUnsupported();
    StopWaitResolveHooks();
    PhotoResolveShutter();
    allRequired = SessResolveHook() && allRequired;
    allRequired = ActivationResolveSourceHooks() && allRequired;
    allRequired = ActivationResolveTestMissingHook() && allRequired;
    return allRequired;
}

static int ActivationPrepare(void) {
    // The log first (it belongs to the lookups step), so the sentinel step's lines (trim, stale markers, latch) reach
    // it too.
    OpenLog();

    // 1. sentinel: read the past, decide latches (a latched launch never reaches the preflight)
    ActivationEnterPhase(kActivationPhaseSentinel);
    if (SentinelPrepare() == kSentinelPrepareLatched) {
        return kPrepareLatched;
    }

    // 2. lookups: the dlopens, dlsyms, the profile plist, the log, SIMCAM_DIAG_DUMP_KEYS, the control file
    ActivationLookups();

    // 3. preflight
    int preflight = ActivationPreflight();
    if (preflight != kPrepareOk) {
        return preflight;
    }

    // 4. resolve and allocate: everything SWAP needs, so SWAP only stores
    ActivationEnterPhase(kActivationPhaseResolve);
    ActivationResolveTestFaults();
    if (!ActivationResolveHooks()) {
        return kPrepareHookMissing;
    }
    ControlResolve();
    return kPrepareOk;
}

// SWAP's test faults, after the client hooks: SIMCAM_TEST_SLOW_SWAP_MS, SIMCAM_TEST_ACTIVATE_FAIL=swapthrow
static void ActivationSwapTestFaults(void) {
    if (gTestSlowSwapMs > 0) {
        SlicedSleepMs(gTestSlowSwapMs);
    }
    if (ActivationTestFailIs("swapthrow")) {
        @throw [NSException exceptionWithName:@"SimcamTestSwapThrow"
                                       reason:@"SIMCAM_TEST_ACTIVATE_FAIL=swapthrow"
                                     userInfo:nil];
    }
}

// ---- SWAP (waiters block on it) ----
// Allowed: method_setImplementation / class_replaceMethod, plain and atomic stores, sigaction, vm_protect,
// dispatch_async_f. Not allowed: ObjC messages, LOG or file I/O, dlopen/dlsym or an image walk, Fig/AVF calls, any of
// our locks. The order makes sure a caller that reaches AVF mid-SWAP without a trap never sees fake sources without
// our session.
static void ActivationSwap(void) {
    atomic_store(&gActivation.phase, kActivationPhaseSwap);
    gActivation.timing.swapBeganNs = MonotonicNowNs();
    ControlSwap();                                 // 1. the state the first frame needs
    if (gVisionInSwap) {
        atomic_store(&gVisionState, 1);
    }
    ActivationHookSwapGroup(kHookGroupClient);     // 2. client hooks: photo, coordinator, preview, recfix, Vision, diag
    ActivationSwapTestFaults();
    if (gSessionHook.mode == kSessionHookGot) {   // 3. session hook
        SessGOTSwap();
    } else {
        ActivationHookSwapGroup(kHookGroupSession);
    }
    ActivationHookSwapGroup(kHookGroupAudio);      // 4. audio source hook, isConnected
    ActivationHookSwapGroup(kHookGroupVideo);      // 5. video source hook, last
    SentinelInstallFull();                         // 6. full crash handler
    ControlEnqueue();                              // 7. orientation apply (main), QR warm-up (global)
    CoordPublish();                                // 8. publish the coordinator table (release)
}

// START's test faults once the sentinel is live: SIMCAM_TEST_SLOW_START_MS, then SIMCAM_TEST_ACTIVATE_FAIL=crashstart
// or crash2 (a fault on one or two threads, for the trace)
static void ActivationStartTestFaults(void) {
    if (gTestSlowStartMs > 0) {
        LOG(@"SIMCAM_TEST_SLOW_START_MS: START sleeps %ld ms (the sentinel is live)", gTestSlowStartMs);
        SlicedSleepMs(gTestSlowStartMs);
    }
    if (ActivationTestFailIs("crashstart")) {
        ActivationTestCrashInStart(1);
    } else if (ActivationTestFailIs("crash2")) {
        ActivationTestCrashInStart(2);
    }
}

// SIMCAM_TEST_ACTIVATE_FAIL=swapthrow: START throws too, at its end, after the armed line
static void ActivationStartTestThrow(void) {
    if (ActivationTestFailIs("swapthrow")) {
        @throw [NSException exceptionWithName:@"SimcamTestStartThrow"
                                       reason:@"SIMCAM_TEST_ACTIVATE_FAIL=swapthrow (START)"
                                     userInfo:nil];
    }
}

// START's last step, first part: what SWAP did with every hook, then the session hook and the motion stand-in
static void ActivationLogSwapResults(void) {
    static const char *kHookGroupNames[] = {
        [kHookGroupClient] = "client",
        [kHookGroupSession] = "session",
        [kHookGroupAudio] = "audio",
        [kHookGroupVideo] = "video",
    };

    for (int i = 0; i < gActivation.hooks.count; i++) {
        ActivationHook *hook = &gActivation.hooks.list[i];
        LOG(@"swap: %s %s[%s %s] (%s)%s%s", kHookGroupNames[hook->group], class_isMetaClass(hook->cls) ? "+" : "-",
            class_getName(hook->cls), sel_getName(hook->sel), hook->what,
            hook->swapResult == kHookChainedToNewer ? " chained to an IMP changed since prepare" : "",
            hook->swapResult == kHookNotInstalled ? " NOT INSTALLED" : "");
    }

    if (gSessionHook.mode == kSessionHookGot) {
        LOG(@"session hook: got, %d of %d entries rebound", gSessionHook.got.patched, gSessionHook.got.count);
    }
    if (gActivation.swapExceptionName[0]) {
        LOG(@"SWAP threw %s: armed but degraded; recorded as crash activate (a second one latches the next "
            @"launch)",
            gActivation.swapExceptionName);
    }
    if (gMotion.installedAt && !gMotion.stockTrapCount) {
        LOG(@"motion: stand-in hooks installed at load, %.1f ms after process start (before arming)",
            (gMotion.installedAt - ProcessStartTime()) * 1000);
    }
    if (gMotion.stockTrapCount) {
        LOG(@"motion: CoreMotion stock this launch from %s (decided at load; the camera is armed)",
            gSentinel.atLoad.motionStockMarker);
    }
}

// the time from the start of one phase to the start of the next, in ms
static double ActivationPhaseMs(int phase, int nextPhase) {
    return (gActivation.timing.phaseBeganNs[nextPhase] - gActivation.timing.phaseBeganNs[phase]) / 1e6;
}

// START's last step, second part: the armed line, with the call that armed it and how long each phase took
static void ActivationLogArmed(void) {
    Dl_info callerInfo = {0};
    const char *callerImage = "?";
    BOOL callerFound = gActivation.trigger.returnAddress && dladdr(gActivation.trigger.returnAddress, &callerInfo) &&
                       callerInfo.dli_fname;
    if (callerFound) {
        callerImage = callerInfo.dli_fname;
    }
    const char *callerBase = strrchr(callerImage, '/');
    callerBase = callerBase ? callerBase + 1 : callerImage;
    const char *inlineNote = "";

    if (gActivation.runsInline && gActivation.lazy) {
        inlineNote = " (inline: SIMCAM_OFF_ACTIVATION_TIMEOUT)";
    } else if (gActivation.runsInline) {
        inlineNote = " (inline: SIMCAM_LAZY=0)";
    }

    double armingMs = (gActivation.timing.armedNs - gActivation.timing.beganNs) / 1e6;
    double sentinelMs = ActivationPhaseMs(kActivationPhaseSentinel, kActivationPhaseLookups);
    double lookupsMs = ActivationPhaseMs(kActivationPhaseLookups, kActivationPhasePreflight);
    double preflightMs = ActivationPhaseMs(kActivationPhasePreflight, kActivationPhaseResolve);
    uint64_t resolveBeganNs = gActivation.timing.phaseBeganNs[kActivationPhaseResolve];
    double resolveMs = (gActivation.timing.swapBeganNs - resolveBeganNs) / 1e6;
    double swapMs = (gActivation.timing.armedNs - gActivation.timing.swapBeganNs) / 1e6;
    LOG(@"TIMING constructors (gate..prevrot) done %.1f ms after process start",
        (CFAbsoluteTimeGetCurrent() - ProcessStartTime()) * 1000);
    LOG(@"LAZY armed by %s on %s thread in %.1f ms (%.1f ms after process start); caller %s, queue %s; "
        @"prepare: sentinel %.1f lookups %.1f preflight %.1f resolve %.1f ms; swap %.2f ms; budget %ld ms%s",
        gActivation.trigger.name, gActivation.trigger.onMain ? "main" : "bg", armingMs,
        (CFAbsoluteTimeGetCurrent() - ProcessStartTime()) * 1000, callerBase,
        gActivation.trigger.queueLabel[0] ? gActivation.trigger.queueLabel : "-", sentinelMs, lookupsMs, preflightMs,
        resolveMs, swapMs, ActivationBudget(), inlineNote);
}

// ---- START (a separate work item on simcam.activate; nobody waits for it) ----
static void ActivationStart(void *ctx) {
    @autoreleasepool {
        @try {
            SentinelStart();              // 1-3. activate line, gSentinel.live, watchdog, atexit
            ActivationStartTestFaults();
            ControlStart();               // 4. control-file timer, observers
            VisionStart();                // 5. Vision's add-image hook, if it wasn't loaded at PREPARE
            ActivationLogSwapResults();   // 6. SWAP's results, then the armed line, last
            ActivationLogArmed();
            ActivationStartTestThrow();
        } @catch (NSException *exception) {
            snprintf(gActivation.exceptionName, sizeof gActivation.exceptionName, "%s",
                     exception.name.UTF8String ?: "?");
            LOG(@"START threw %@: %@", exception.name, exception.reason);
            if (ActivationStrikeOnce()) {
                ActivationRecordExceptionF(NULL);
            } else {
                LOG(@"activation: START threw %s; not recorded (SWAP's exception is this launch's strike)",
                    gActivation.exceptionName);
            }
        }
    }
}

// ---- ActivationRun: PREPARE, the compare-and-swap, SWAP, publish ARMED ----
// It runs on simcam.activate, or inline in the test modes.
// A launch that stays stock writes no more events (the record queue appends through its own fd).
static void SentinelCloseEventsFd(void) {
    int fd = gSentinel.events.fd;
    gSentinel.events.fd = -1;
    if (fd >= 0) {
        close(fd);
    }
}

static void ActivationStockAfterPrepare(int result) {
    SentinelRestoreConditional();   // before STOCK is published
    atomic_store(&gActivation.preparing, 0);
    int expected = kActivationRunning;
    BOOL won = atomic_compare_exchange_strong_explicit(&gActivation.state, &expected, kActivationStock,
                                                       memory_order_release, memory_order_relaxed);
    SentinelCloseEventsFd();
    uint64_t elapsedMs = (MonotonicNowNs() - gActivation.timing.beganNs) / NSEC_PER_MSEC;

    switch (result) {
        case kPrepareLatched:
            ActivationRecordLog(YES, "%s", gSentinel.latchMessage);
            break;
        case kPreparePreflightFailed:
        case kPreparePreflightForced:
            ActivationRecordLog(
                NO,
                "SIMCAM DISABLED: preflight failed%s, no hooks installed; the app sees the stock simulator (no "
                "camera)",
                result == kPreparePreflightForced ? " (forced)" : "");
            ActivationRecordLog(
                YES, "simcam: DISABLED - preflight failed (see log); the app sees the stock simulator (no camera)");
            break;
        case kPrepareHookMissing:
            ActivationRecordLog(YES, "simcam: activation failed: %s not found; this launch has no camera",
                                gActivation.hooks.missing);
            break;
        // recorded only if this compare-and-swap won: after a timeout the waiter already recorded this launch's one
        // strike
        case kPrepareThrew:
            if (won) {
                (void)ActivationStrikeOnce();
                dispatch_async_f(gActivation.queues.record, NULL, ActivationRecordExceptionF);
                ActivationRecordLog(YES, "simcam: activation threw %s; this launch has no camera",
                                    gActivation.exceptionName);
            } else {
                ActivationRecordLog(NO,
                                    "activation: prepare threw %s after the timeout; not recorded (the timeout is this "
                                    "launch's strike)",
                                    gActivation.exceptionName);
            }
            break;
    }

    if (!won) {
        ActivationRecordLog(
            NO, "activation: prepare failed after %llu ms, after a caller had already given up; staying stock",
            elapsedMs);
    }
}

static void ActivationRun(void *ctx) {
    gActivationThread = 1;
    atomic_store(&gActivation.preparing, 1);
    int result;

    @autoreleasepool {
        @try {
            result = ActivationPrepare();
        } @catch (NSException *exception) {
            snprintf(gActivation.exceptionName, sizeof gActivation.exceptionName, "%s",
                     exception.name.UTF8String ?: "?");
            result = kPrepareThrew;
            LOG(@"activation: prepare threw %@: %@", exception.name, exception.reason);
        }
    }

    if (result != kPrepareOk) {
        ActivationStockAfterPrepare(result);
        if (!gActivation.runsInline) {
            dispatch_group_leave(gActivation.queues.waiters);
        }
        gActivationThread = 0;
        return;
    }

    int expected = kActivationRunning;

    if (!atomic_compare_exchange_strong(&gActivation.state, &expected,
                                        kActivationCommitting)) {   // a waiter timed out: install nothing
        atomic_store(&gActivation.preparing, 0);
        SentinelRestoreConditional();
        SentinelCloseEventsFd();
        ActivationRecordLog(NO, "activation: finished after %llu ms; staying stock (a caller gave up at %ld ms)",
                            (MonotonicNowNs() - gActivation.timing.beganNs) / NSEC_PER_MSEC, ActivationBudget());
        if (!gActivation.runsInline) {
            dispatch_group_leave(gActivation.queues.waiters);
        }
        gActivationThread = 0;
        return;
    }

    @try {
        ActivationSwap();
    } @catch (NSException *exception) {   // ARMED-degraded
        snprintf(gActivation.swapExceptionName, sizeof gActivation.swapExceptionName, "%s",
                 exception.name.UTF8String ?: "?");
    }

    gActivation.timing.armedNs = MonotonicNowNs();
    atomic_store_explicit(&gActivation.state, kActivationArmed, memory_order_release);
    atomic_store(&gActivation.preparing, 0);
    atomic_store(&gActivation.phase, kActivationPhaseDone);
    TraceArm();   // the op ring records from here (the activate op)
    if (gActivation.runsInline) {
        gActivationThread = 0;
        TraceArmFile();
        ActivationStart(NULL);
        return;
    }

    dispatch_group_leave(gActivation.queues.waiters);
    TraceArmFile();   // the trace's directory, after the waiters are released (file I/O; nobody waits for it)
    gActivationThread = 0;
    dispatch_async_f(gActivation.queues.activate, NULL, ActivationStart);
}

// ---- callers ----
// ActivationTimedOut: the waiter whose compare-and-swap won. It clears gActivation.preparing first, then does the
// conditional restore (before it returns to its caller: this covers a PREPARE that never finishes), then dispatches the
// record at once.
static void ActivationTimedOut(void) {
    atomic_store(&gActivation.preparing, 0);
    SentinelRestoreConditional();
    int phase = atomic_load(&gActivation.phase);
    if (ActivationStrikeOnce()) {
        dispatch_async_f(gActivation.queues.record, (void *)(long)phase, ActivationRecordTimeoutF);
    }
    ActivationRecordLog(YES, "simcam: activation took over %ld ms (in %s); this launch has no camera",
                        ActivationBudget(), kActivationPhaseNames[phase]);
}

// A waiter wakes at least every kActivationWaitSliceMaxMs to re-read the state and the deadline.
#define kActivationWaitSliceMinMs 1
#define kActivationWaitSliceMaxMs 100
// A slice that overran by more than this means the process was stopped (lldb, SIGSTOP, sleep), not just busy.
#define kActivationStoppedSlackMs 500

// one wait slice: the time left until the deadline, kept within kActivationWaitSliceMinMs..kActivationWaitSliceMaxMs
static int64_t ActivationWaitSlice(uint64_t deadline, uint64_t sliceStart) {
    int64_t left = (int64_t)(deadline - sliceStart);
    int64_t minSlice = kActivationWaitSliceMinMs * (int64_t)NSEC_PER_MSEC;
    int64_t maxSlice = kActivationWaitSliceMaxMs * (int64_t)NSEC_PER_MSEC;
    if (left < minSlice) {
        return minSlice;
    }
    if (left > maxSlice) {
        return maxSlice;
    }
    return left;
}

// YES if the process was stopped during this slice. Exactly one waiter (the one whose compare-and-swap wins) moves the
// deadline later by the length of the stop; every waiter that saw the stop loops again.
static BOOL ActivationWaiterWasStopped(uint64_t deadline, uint64_t sliceStart, int64_t slice) {
    uint64_t gap = MonotonicNowNs() - sliceStart;
    if (gap <= (uint64_t)slice + kActivationStoppedSlackMs * NSEC_PER_MSEC) {
        return NO;
    }
    if (atomic_compare_exchange_strong(&gActivation.budget.deadlineNs, &deadline, deadline + gap)) {
        ActivationRecordLog(NO, "activation: process was stopped for %.1f s; deadline +%.1f s", gap / 1e9, gap / 1e9);
    }
    return YES;
}

// YES once the shared deadline has passed (a stop may have moved it since the slice began)
static BOOL ActivationDeadlinePassed(void) {
    return MonotonicNowNs() >= atomic_load(&gActivation.budget.deadlineNs);
}

// SWAP (COMMITTING) never times out: the first waiter to see the deadline pass logs the overrun, once.
static void ActivationNoteSwapOverrun(void) {
    BOOL overran = ActivationDeadlinePassed();
    if (overran && !atomic_exchange(&gActivation.noted.swapOverrun, 1)) {
        ActivationRecordLog(YES,
                            "simcam: activation's install (SWAP) overran the %ld ms budget; waiting for it "
                            "(COMMITTING overran)",
                            ActivationBudget());
    }
}

// YES if this waiter moved the state from RUNNING to STOCK; NO if the state had already moved on.
static BOOL ActivationClaimTimeout(void) {
    int expected = kActivationRunning;
    return atomic_compare_exchange_strong_explicit(&gActivation.state, &expected, kActivationStock,
                                                   memory_order_release, memory_order_relaxed);
}

// One slice of waiting, for a waiter that saw RUNNING or COMMITTING: YES if this waiter timed the activation out,
// NO to look at the state again.
static BOOL ActivationWaitSliceTimedOut(int state) {
    uint64_t deadline = atomic_load(&gActivation.budget.deadlineNs);
    uint64_t sliceStart = MonotonicNowNs();
    int64_t slice = ActivationWaitSlice(deadline, sliceStart);
    if (dispatch_group_wait(gActivation.queues.waiters, dispatch_time(DISPATCH_TIME_NOW, slice)) == 0) {
        return NO;   // the activation left the group: the state is final
    }
    if (ActivationWaiterWasStopped(deadline, sliceStart, slice)) {
        return NO;
    }
    if (state == kActivationCommitting) {
        ActivationNoteSwapOverrun();
        return NO;
    }
    if (!ActivationDeadlinePassed()) {
        return NO;
    }
    // Measure again right before the compare-and-swap: a waiter stopped after its first check (SIGSTOP, lldb) must
    // not go STOCK before the waiter that saw the stop moves the deadline; it treats the stop as its own and loops.
    if (ActivationWaiterWasStopped(deadline, sliceStart, slice)) {
        return NO;
    }
    return ActivationClaimTimeout();
}

// Waits until the activation is ARMED or STOCK, and returns which. Every waiter shares one deadline and wakes at least
// once a slice; the activation leaving the waiters' group wakes them all at once. After the deadline, the waiters race
// one compare-and-swap from RUNNING to STOCK, and the winner records the timeout; the others see STOCK on their next
// look. A stopped process moves the deadline later (ActivationWaiterWasStopped), so a stop never counts.
static int ActivationWait(void) {
    for (;;) {
        int state = ActivationState();
        if (state == kActivationArmed || state == kActivationStock) {
            return state;
        }
        if (ActivationWaitSliceTimedOut(state)) {
            ActivationTimedOut();
            return kActivationStock;
        }
    }
}

// SIMCAM_TEST_FIG_FAULT_AT_MS=<ms>: the CoreMedia fault, that long after the first call, whatever PREPARE is doing
static void ActivationScheduleFigFaultTest(void) {
    if (gTestFigFaultAtMs <= 0) {
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, gTestFigFaultAtMs * (int64_t)NSEC_PER_MSEC),
                     dispatch_get_global_queue(0, 0), NULL, ActivationFigFaultF);
}

// the first trapped call, handed to the dispatch_once body
typedef struct {
    const char *why;       // the trapped selector's name
    void *returnAddress;   // where the trapped call came from
} ActivationCaller;

static void ActivationBegin(void *ctx) {   // the dispatch_once body: microseconds, never waits
    ActivationCaller *caller = ctx;
    gActivation.timing.beganNs = MonotonicNowNs();
    gActivation.timing.phaseBeganNs[kActivationPhaseStart] = gActivation.timing.beganNs;
    atomic_store(&gSentinel.traced, ProcessIsTraced());   // one sysctl, no lock

    // Under a debugger or sanitizers everything runs slower: the default budget is scaled up, an explicit one never is.
    if (!gActivation.budget.isExplicit) {
        BOOL slowedDown = atomic_load(&gSentinel.traced) || gSentinel.scale > 1;
        if (slowedDown) {
            gActivation.budget.ms *= kActivationSlowedBudgetScale;
        }
    }

    atomic_store(&gActivation.budget.deadlineNs,
                 gActivation.timing.beganNs + (uint64_t)gActivation.budget.ms * NSEC_PER_MSEC);
    snprintf(gActivation.trigger.name, sizeof gActivation.trigger.name, "%s", caller->why);
    gActivation.trigger.onMain = pthread_main_np();
    gActivation.trigger.returnAddress = caller->returnAddress;
    const char *queueLabel = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
    snprintf(gActivation.trigger.queueLabel, sizeof gActivation.trigger.queueLabel, "%s", queueLabel ? queueLabel : "");
    OrientTrackStartForCamera();   // enqueued, so the tracker runs while PREPARE does
    ActivationScheduleFigFaultTest();
    atomic_store(&gActivation.state, kActivationRunning);

    if (gOffActivationTimeout) {   // the mutant: activation inline on the caller's thread, no bound
        gActivation.runsInline = YES;
        ActivationRun(NULL);
        return;
    }

    dispatch_group_enter(gActivation.queues.waiters);
    dispatch_async_f(gActivation.queues.activate, NULL, ActivationRun);
}

// SIMCAM_TEST_TRAP_FAULT=1 (act.inc): once per process, in the first camera call's trap after it armed. Its line still
// says "ActEnsure", this function's old name: scripts/activation/orientation.sh matches that text.
__attribute__((noinline)) static void ActivationTrapFault(void) {
    static _Atomic int once;
    if (atomic_exchange(&once, 1)) {
        return;
    }
    const void *featureKey =
        atomic_load(&gSentinel.featureKey.on) ? pthread_getspecific(gSentinel.featureKey.key) : NULL;
    const char *featureName = "none";
    if (featureKey == (const void *)kMotionFeatureKey) {
        featureName = "motion";
    } else if (featureKey) {
        featureName = "other";
    }
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_TRAP_FAULT: fault in ActEnsure now (%s thread, feature key %s)\n",
            getpid(), pthread_main_np() ? "main" : "bg", featureName);
    TestFaultNullWrite(1);
}

// The first armed call on the main thread runs the orientation tracker once, so a RotationCoordinator built in the
// same run-loop turn reads the real orientation. The tracker is camera code, so it runs with the feature key cleared,
// whatever the calling thread carries. SIMCAM_TEST_ORIENT_FAULT=ensure sets the motion key first, as a call from
// inside a motion entry point would carry it; SIMCAM_OFF_ENSURE_KEY_CLEAR=1 skips the clear.
static void ActivationRefreshTrackerOnMain(void) {
    void *injected = gTestOrientFault == kOrientFaultEnsure ? SentinelFeatureEnter(kMotionFeatureKey) : NULL;
    void *saved = SentinelFeatureClearUnless(gOffEnsureKeyClear);
    OrientTrackRunMain();
    SentinelFeatureRestoreUnless(gOffEnsureKeyClear, saved);
    if (gTestOrientFault == kOrientFaultEnsure) {
        SentinelFeatureLeave(injected);
    }
}

#define kActivationSlowFirstCallMs 50   // a first camera call that waited longer than this gets a TIMING line

// Every trapped call: returns the state the caller proceeds in. The trap forwards to the saved original either way:
// when ARMED it reaches the SWAPped hooks, when STOCK Apple's unhooked methods.
static int ActivationEnsure(const char *why, void *returnAddress) {
    int state = ActivationState();
    if (state == kActivationArmed) {
        return state;
    }

    if (state == kActivationStock) {
        if (gSentinel.atLoad.latchMarker[0] && !atomic_exchange(&gActivation.noted.stock, 1)) {
            dispatch_async_f(gActivation.queues.record, NULL, ActivationMarkerNoteF);
        }
        return state;
    }

    // re-entrant (activation's own call, or AVF's dispatch_sync onto its queue from it)
    if (gActivationThread || dispatch_get_specific(&kActivationQueueKey)) {
        if (!atomic_exchange(&gActivation.noted.reentry, 1)) {
            ActivationRecordLog(NO, "activation: re-entrant call %s forwarded to stock (no wait)", why);
        }
        return state;
    }

    uint64_t callStart = MonotonicNowNs();
    ActivationCaller caller = {why, returnAddress};
    dispatch_once_f(&gActivation.queues.once, &caller, ActivationBegin);
    state = gActivation.runsInline ? ActivationState() : ActivationWait();
    uint64_t waited = MonotonicNowNs() - callStart;
    if (waited > kActivationSlowFirstCallMs * NSEC_PER_MSEC) {
        ActivationRecordLog(NO, "TIMING first camera call %s on %s thread waited %.1f ms -> %s", why,
                            pthread_main_np() ? "main" : "bg", waited / 1e6,
                            state == kActivationArmed ? "armed" : "stock");
    }
    if (gTestTrapFault && state == kActivationArmed) {
        ActivationTrapFault();
    }
    BOOL armedOnMain = state == kActivationArmed && pthread_main_np();
    if (armedOnMain && !atomic_exchange(&gActivation.noted.mainTrackerRun, 1)) {
        ActivationRefreshTrackerOnMain();
    }
    return state;
}

static void ActivationInstallTrap(Class trappedClass, SEL sel, BOOL isClassMethod) {
    Class target = isClassMethod ? object_getClass(trappedClass) : trappedClass;
    Method method = target ? class_getInstanceMethod(target, sel) : NULL;
    if (!method) {
        return;
    }
    IMP orig = method_getImplementation(method);
    const char *name = sel_getName(sel);
    // every trapped method takes at most 3 object or integer arguments and returns an object pointer
    IMP trap = imp_implementationWithBlock(^void *(void *receiver, void *arg1, void *arg2, void *arg3) {
        (void)ActivationEnsure(name, __builtin_return_address(0));
        return ((void *(*)(void *, SEL, void *, void *, void *))orig)(receiver, sel, arg1, arg2, arg3);
    });
    // The class's own method only: if the class inherits this selector (for example -[AVCaptureSession init] from
    // NSObject), add an override on the class instead of swizzling the superclass's method for every object in the
    // process.
    if (!class_addMethod(target, sel, trap, method_getTypeEncoding(method))) {
        method_setImplementation(method, trap);
    }
}

// SIMCAM_ACTIVATE_TIMEOUT_MS=<1-60000>: an explicit budget (never scaled); anything else keeps the default, with a line
static void ActivationReadBudget(void) {
    const char *text = getenv(ENV_ACTIVATE_TIMEOUT_MS);
    if (!text) {
        return;
    }
    char *end = NULL;
    long budget = strtol(text, &end, 10);
    BOOL isInteger = *text && end && !*end;
    BOOL inRange = budget >= kActivationMinBudgetMs && budget <= kActivationMaxBudgetMs;
    if (isInteger && inRange) {
        gActivation.budget.ms = budget;
        gActivation.budget.isExplicit = YES;
    } else {
        fprintf(stderr,
                "simcam: SIMCAM_ACTIVATE_TIMEOUT_MS=%s is not an integer in 1-60000; using the default 1000 ms\n",
                text);
    }
}

// the private serial queues (simcam.activate, marked for the re-entrancy check; simcam.record) and the waiters' group
static void ActivationCreateQueues(void) {
    gActivation.queues.activate = dispatch_queue_create(
        "simcam.activate", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    dispatch_queue_set_specific(gActivation.queues.activate, &kActivationQueueKey, &kActivationQueueKey, NULL);
    gActivation.queues.record = dispatch_queue_create(
        "simcam.record", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    gActivation.queues.waiters = dispatch_group_create();
}

// CoreMotion is a public class, so its stand-in's hooks go in at load: an app that asks for the accelerometer before
// touching the camera must get the same answer as one that asks after. SIMCAM_OFF_MOTION=1 keeps CoreMotion stock.
// The choice is made once, here, from markers only: this build's camera latch, motion latch or real preflight failure
// (sentinel on, no clear) makes CoreMotion stock for the process, with one-shot forwarding traps that say so once.
static void MotionInstallAtLoad(void) {
    SentinelFeatureKeyInit();   // the feature key that attributes stand-in crashes to `motion` (core/safety.inc)
    if (MotionStandInOn()) {
        if (SentinelMotionStockAtLoad()) {
            MotionInstallStockTraps();
        } else {
            MotionInstallHooks();
        }
    }
}

// SIMCAM_LAZY=0: the phases inline at load, no timeout (test mode)
static void ActivationRunAtLoad(void) {
    gActivation.runsInline = YES;
    gActivation.timing.beganNs = MonotonicNowNs();
    gActivation.timing.phaseBeganNs[kActivationPhaseStart] = gActivation.timing.beganNs;
    snprintf(gActivation.trigger.name, sizeof gActivation.trigger.name, "SIMCAM_LAZY=0 (load)");
    gActivation.trigger.onMain = pthread_main_np();
    atomic_store(&gSentinel.traced, ProcessIsTraced());
    if (SentinelLatchMarkerValidAtLoad()) {
        atomic_store(&gActivation.state, kActivationStock);
        ActivationMarkerNoteF(NULL);
        return;
    }
    OrientTrackStartForCamera();
    atomic_store(&gActivation.state, kActivationRunning);
    ActivationRun(NULL);
}

// the traps on the public capture entry points: the first call into one starts activation
static void ActivationInstallTraps(void) {
    Class device = objc_getClass("AVCaptureDevice");
    Class discovery = objc_getClass("AVCaptureDeviceDiscoverySession");
    Class session = objc_getClass("AVCaptureSession");
    SEL deviceSelectors[] = {@selector(defaultDeviceWithMediaType:),
                             @selector(defaultDeviceWithDeviceType:mediaType:position:),
                             @selector(devices),
                             @selector(devicesWithMediaType:),
                             @selector(deviceWithUniqueID:),
                             @selector(systemPreferredCamera)};
    for (unsigned i = 0; i < sizeof deviceSelectors / sizeof *deviceSelectors; i++) {
        ActivationInstallTrap(device, deviceSelectors[i], YES);
    }
    ActivationInstallTrap(discovery, @selector(discoverySessionWithDeviceTypes:mediaType:position:), YES);
    ActivationInstallTrap(session, @selector(init), NO);
}

__attribute__((constructor)) static void ActivationInstallAtLoad(void) {
    const char *simcamEnv = getenv(ENV_SIMCAM);
    if (simcamEnv && simcamEnv[0] == '0') {
        return;
    }
    if (!IsPrimaryEngine()) {
        return;   // a second engine in this process stays inert (core/init.inc)
    }
    {
        const char *lazyEnv = getenv(ENV_LAZY);
        gActivation.lazy = (lazyEnv && lazyEnv[0] == '0') ? 0 : 1;
    }

    // computed once, at load: budget, sanitizer scale, flags, bundle id, build, both state paths, marker stat()s
    ActivationReadBudget();
    ActivationParseTestEnv();
    SentinelLoadInit();

    // SIMCAM_SENTINEL=clear: the clear line, markers unlinked (an explicit action, so I/O at load is fine)
    SentinelClearAtLoad();
    ActivationCreateQueues();
    MotionInstallAtLoad();

    if (!gActivation.lazy) {
        ActivationRunAtLoad();
        return;
    }

    // A valid latched-<build> marker (sentinel on, no clear): STOCK from the start, no PREPARE (one line at the first
    // call).
    if (SentinelLatchMarkerValidAtLoad()) {
        atomic_store(&gActivation.state, kActivationStock);
    }
    ActivationInstallTraps();

    // One main-queue block that records the UI orientation once (no timer, no notifications), so a coordinator built
    // off main while main is busy reads the right preview angle at once. Not in a marker-STOCK launch.
    if (atomic_load(&gActivation.state) != kActivationStock) {
        OrientUISnapAtLoad();
    }
}
