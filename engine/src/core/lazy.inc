// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): lazy arming with bounded activation
// (design §4.1 "Activation"). Must stay last.
// At load: traps on the public capture entry points and the CoreMotion stand-in's hooks; no preflight, no private call.
// The first trapped call starts activation once on the private serial queue simcam.activate; every caller waits at most
// SIMCAM_ACTIVATE_TIMEOUT_MS (default 1000; x10 under a debugger or sanitizers) from that first call, against one
// shared deadline, then forwards to stock for the rest of the process (state machine: core/act.inc).
//   PREPARE  sentinel (latch -> STOCK, before any preflight), lookups, preflight, resolve: may do I/O and send
//            messages, installs nothing process-visible but the minimal crash handler
//   SWAP     installs (memory-only: no messages, no I/O, no dyld lock); callers wait for it like for a lock, never
//            time out
//   START    a separate work item after ARMED: the sentinel goes live, then observers, timers, logging
// Apps that never touch the camera are never armed. SIMCAM_LAZY=0 runs the same phases inline at load (no traps, no
// timeout; a test mode: it runs inside dlopen with the dyld lock held, where a private queue would always time out).
// Trampolines take void * arguments: strong id parameters made ARC retain argument registers the real method never set
// (30 of 50 test launches crashed).
#import <objc/runtime.h>
// ---- activation state: the budget and deadline, phase timings, the queues, the first caller ----
static long gActBudgetMs = 1000;
static BOOL gActBudgetExplicit;
static _Atomic uint64_t gActDeadline;       // CLOCK_MONOTONIC ns
static uint64_t gActT0, gActTPrep[PH_DONE + 1], gActTSwap0, gActTArmed;
static dispatch_queue_t gActQ, gActRecQ;
static dispatch_group_t gActGroup;
static char kActQKey;   // dispatch_queue_set_specific on simcam.activate (secondary re-entrancy check)
static dispatch_once_t gActOnce;
// SIMCAM_OFF_ACTIVATION_TIMEOUT / SIMCAM_LAZY=0: the phases ran on the caller's thread
static BOOL gActInline;
static _Atomic int gActStockNoted, gActReenterNoted, gActOverrunNoted, gActMainUpd;
static char gActWhy[96], gActQueueLabel[96];
static BOOL gActWhyMain;
static void *gActCallerRA;
static char gActExc[128];
static long ActBudget(void) { return gActBudgetMs; }   // the effective budget (set by the once body)

// ---- the record queue (simcam.record, serial) ----
// Everything the engine writes about activation, so callers never do I/O.
static void ActRecLogF(void *ctx) {
    @autoreleasepool {
        char *s = ctx;
        ActOpenLog();
        LOG(@"%s", s);
        free(s);
    }
}
static void ActRecStderrF(void *ctx) {
    @autoreleasepool {
        char *s = ctx;
        ActOpenLog();
        fprintf(stderr, "%s\n", s);
        LOG(@"%s", s);
        free(s);
    }
}
static void ActRecLog(BOOL toStderr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ActRecLog(BOOL toStderr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    vasprintf(&s, fmt, ap);
    va_end(ap);
    if (s) dispatch_async_f(gActRecQ, s, toStderr ? ActRecStderrF : ActRecLogF);
}
// the timeout record: exactly one line per process (guarded by ActStrikeOnce); publishes the activation number it used
static void ActRecordTimeoutF(void *ctx) {
    @autoreleasepool {
        long phase = (long)ctx;
        char detail[96];
        snprintf(detail, sizeof detail, "ms=%ld phase=%s", ActBudget(), kActPhaseName[phase]);
        BOOL traced = atomic_load(&gSCTraced);
        int act = SCRecordEvent(traced ? "unknown-hang" : "hang", "activate", detail);
        if (act) {
            int none = 0;
            atomic_compare_exchange_strong(&gActRecordedAct, &none, act);
        }
        ActOpenLog();
        LOG(@"activation: %s activate recorded (activation %d, %s)%s",
            traced ? "unknown-hang (traced, not counted)" : "hang", act, detail,
            act ? "" : " — could not write events.log");
    }
}
static void ActRecordExceptionF(void *ctx) {
    @autoreleasepool {
        char detail[160];
        snprintf(detail, sizeof detail, "exception=%s", gActExc);
        int act = SCRecordEvent(atomic_load(&gSCTraced) ? "crash-traced" : "crash", "activate", detail);
        ActOpenLog();
        LOG(@"activation: crash activate %s recorded (activation %d)", detail, act);
    }
}

// ---- the marker-STOCK line (no silent stock, design §5.1) ----
// The features and the until come from one open file (fstat on the read's fd): a stat by path could see
// a marker another process renamed in after the read.
static void ActMarkerNoteF(void *ctx) {
    char feats[200] = "?", when[64] = "?";
    int fd = open(gSCMarkerAtLoad, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, feats, sizeof feats - 1);
        if (n > 0) {
            feats[n] = 0;
            char *nl = strchr(feats, '\n');
            if (nl) *nl = 0;
        }
        struct stat st;
        if (fstat(fd, &st) == 0) {
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        }
        close(fd);
    }
    char *s = NULL;
    asprintf(&s, "simcam: %s latched until %s; no camera. Relaunch with SIMCAM_SENTINEL=clear to retry (marker %s)",
             feats, when, gSCMarkerAtLoad);
    if (s) ActRecStderrF(s);
}

// ---- the motion-stock line (no silent stock, design §5.1) ----
// Queued by the first call into a CoreMotion forwarding trap.
// The until comes from the load-time stat (SCMotionStockAtLoad), and so does the kind: by this first motion call a
// passing preflight or PREPARE 1's marker update may have unlinked the marker. Its feature list is read only from the
// version that load-time stat saw (same inode and mtime); a re-written marker (another until) gives the
// kind instead, so the features and the until always come from one snapshot.
static void ActMotionStockNoteF(void *ctx) {
    const char *marker = gSCMotionStockAt;
    char feats[200];
    snprintf(feats, sizeof feats, "%s", gSCMotionStockKind ?: "?");
    int fd = open(marker, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_ino == gSCMotionStockIno && st.st_mtime == gSCMotionStockUntil) {
        char b[200];
        ssize_t n = read(fd, b, sizeof b - 1);
        if (n > 0) {
            b[n] = 0;
            char *nl = strchr(b, '\n');
            if (nl) *nl = 0;
            if (b[0]) snprintf(feats, sizeof feats, "%s", b);
        }
    }
    if (fd >= 0) close(fd);
    char when[64] = "?";
    time_t until = gSCMotionStockUntil;
    if (until) {
        struct tm tm;
        localtime_r(&until, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    }
    const char *base = strrchr(marker, '/');
    base = base ? base + 1 : marker;
    char *s = NULL;
    asprintf(&s,
             "simcam: CoreMotion is stock this launch (%s: %s until %s). Relaunch with SIMCAM_SENTINEL=clear to "
             "retry",
             base, feats, when);
    if (s) ActRecStderrF(s);
}
static void ActMotionStockNote(void) { dispatch_async_f(gActRecQ, NULL, ActMotionStockNoteF); }

// ---- PREPARE (on simcam.activate) ----
enum { AP_OK = 0, AP_LATCHED, AP_PREFLIGHT, AP_MISSING, AP_THROW, AP_FORCED };
static void ActPhase(int ph) {
    gActTPrep[ph] = ActNow();
    atomic_store(&gActPhase, ph);
}
// Test faults in START (armed, the full crash handler installed, the trace ring on, its directory picked, no flush
// yet): SIMCAM_TEST_ACTIVATE_FAIL=crashstart faults here, on the activation queue; =crash2 faults on two threads at
// once (both released by one store, both PCs in our image), so two threads run the crash handler together. The trace
// must still be written, once and whole (ledger L69, L70).
static _Atomic int gTestCrashGo;
static void *ActTestCrashThread(void *ctx) {
    while (!atomic_load(&gTestCrashGo)) {}
    TestFaultNullWrite((int)(intptr_t)ctx);
    return NULL;
}
static void ActTestCrashInStart(int n) {
    LOG(@"SIMCAM_TEST_ACTIVATE_FAIL=%s: fault in START on %d thread(s)", n > 1 ? "crash2" : "crashstart", n);
    if (n == 1) TestFaultNullWrite(1);
    pthread_t t[2];
    for (int i = 0; i < n && i < 2; i++) pthread_create(&t[i], NULL, ActTestCrashThread, (void *)(intptr_t)(i + 1));
    usleep(20000);
    atomic_store(&gTestCrashGo, 1);
    for (;;) usleep(100000);   // the process ends in the handler's chain
}
// SIMCAM_TEST_ACTIVATE_FAIL=crashfig: a CoreMedia call on an invalid object, on a global queue
static void ActCrashFigF(void *ctx) {
    const void *(*getVT)(const void *) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    if (getVT) getVT((const void *)(uintptr_t)0x10);
}
static void ActNop(void *ctx) {}
// SIMCAM_TEST_FIG_FAULT_AT_MS (act-fake-sdk): a related-image (CoreMedia) fault, independent of prepare
static void ActFigFaultF(void *ctx) {
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_FIG_FAULT_AT_MS: CoreMedia fault now, %.0f ms after the first call\n",
            getpid(), (ActNow() - gActT0) / 1e6);
    ActCrashFigF(NULL);
}
// PREPARE 2 (lookups): the dlopens, dlsyms, the profile plist, the log, SIMCAM_DIAG_DUMP_KEYS, the control file
static void ActLookups(void) {
    ActPhase(PH_LOOKUPS);
    ActTestSlowAt(PH_LOOKUPS);
    ActLoadFrameworks();
    CtlReadAtPrepare();
    SCCacheRanges();   // again, after the dlopens
    LOG(@"TIMING gate entered %.1f ms after process start (%.1f ms after the first call)",
        (CFAbsoluteTimeGetCurrent() - ActProcessStartTime()) * 1000, (ActNow() - gActT0) / 1e6);
}
// PREPARE 3 (preflight): AP_OK, or AP_PREFLIGHT / AP_FORCED (forced: a test switch made it fail)
static int ActPreflight(void) {
    ActPhase(PH_PREFLIGHT);
    ActTestSlowAt(PH_PREFLIGHT);
    uint64_t preflightStart = ActNow();
    BOOL forced = gActFakeBreak || ActTestFailIs("preflight") || getenv(ENV_SLOT_COUNTS) || getenv(ENV_PF_NOSYMOK);
    BOOL ok = ActTestFailIs("preflight") ? NO : PreflightRun();
    LOG(@"preflight took %.1f ms", (ActNow() - preflightStart) / 1e6);
    if (!ok) {
        // a forced preflight failure never writes preflight-fail
        if (!forced || gTestPfFailReal) SCPreflightMarker(NO, "preflight failed");
        return forced && !gTestPfFailReal ? AP_FORCED : AP_PREFLIGHT;
    }
    SCPreflightMarker(YES, NULL);
    return AP_OK;
}
// PREPARE 4's test faults, before anything is resolved (SIMCAM_TEST_ACTIVATE_FAIL=throw|crash|crashfig,
// SIMCAM_TEST_ACTIVATE_NEEDS_MAIN, SIMCAM_TEST_REENTER, SIMCAM_TEST_SLOW_ACTIVATE_MS=<n>@resolve)
static void ActResolveTestFaults(void) {
    if (ActTestFailIs("throw"))
        @throw [NSException exceptionWithName:@"SimcamTestActivateThrow"
                                       reason:@"SIMCAM_TEST_ACTIVATE_FAIL=throw"
                                     userInfo:nil];
    if (ActTestFailIs("crash")) TestFaultNullWrite(1);
    // the fault's PC is in CoreMedia, on another thread; wait for it (well under the budget)
    if (ActTestFailIs("crashfig")) {
        dispatch_async_f(dispatch_get_global_queue(0, 0), NULL, ActCrashFigF);
        ActSleepMs(500);
    }
    if (gTestActivateNeedsMain) {
        LOG(@"SIMCAM_TEST_ACTIVATE_NEEDS_MAIN: prepare waits on the main queue");
        dispatch_sync_f(dispatch_get_main_queue(), NULL, ActNop);
    }
    if (gTestReenter) {
        LOG(@"SIMCAM_TEST_REENTER: prepare calls a trapped entry point");
        (void)[[AVCaptureSession alloc] init];
    }
    ActTestSlowAt(PH_RESOLVE);
}
// PREPARE 4: every hook's Method and replacement (SWAP installs them). Returns NO if a required one is missing.
static BOOL ActResolveHooks(void) {
    gActNHooks = 0;
    gActMissing[0] = 0;
    BOOL r = YES;
    DiagResolveSessionHooks();
    DiagResolveMetadataHooks();
    r = PhotoResolveDataFix() && r;
    r = CoordResolveHooks() && r;
    r = PreviewResolveRotation() && r;
    r = RecSettingsResolveFix() && r;
    VisionResolveFix();
    PhotoResolveUnsupported();
    StopWaitResolveHooks();
    PhotoResolveShutter();
    r = SessResolveHook() && r;
    r = ActResolveSourceHooks() && r;
    if (ActTestFailIs("missing"))
        r = ActHookResolve(HG_CLIENT, "SIMCAM_TEST_ACTIVATE_FAIL=missing", objc_getClass("AVCaptureSession"),
                           "simcamTestNoSuchMethod", NO, NO, (IMP)ActNop, NULL, YES) &&
            r;
    return r;
}
static int ActPrepare(void) {
    // the log first (it belongs to PREPARE 2), so PREPARE 1's lines (trim, stale markers, latch) reach it too
    ActOpenLog();
    // 1. sentinel: read the past, decide latches (a latched launch never reaches the preflight)
    ActPhase(PH_SENTINEL);
    if (SCPrepare() == SCP_LATCHED) return AP_LATCHED;
    // 2. lookups: the dlopens, dlsyms, the profile plist, the log, SIMCAM_DIAG_DUMP_KEYS, the control file
    ActLookups();
    // 3. preflight
    int preflight = ActPreflight();
    if (preflight != AP_OK) return preflight;
    // 4. resolve and allocate: everything SWAP needs, so SWAP only stores
    ActPhase(PH_RESOLVE);
    ActResolveTestFaults();
    if (!ActResolveHooks()) return AP_MISSING;
    CtlResolve();
    return AP_OK;
}

// ---- SWAP (waiters block on it): method_setImplementation / class_replaceMethod, plain/atomic stores, sigaction,
// vm_protect, dispatch_async_f. No ObjC messages, no LOG or file I/O, no dlopen/dlsym/image walk, no Fig/AVF calls,
// none of our locks. Order: a non-trap caller reaching AVF mid-SWAP never sees fake sources without our session.
static void ActSwap(void) {
    atomic_store(&gActPhase, PH_SWAP);
    gActTSwap0 = ActNow();
    CtlSwap();                       // 1. the state the first frame needs
    if (gVisionInSwap) atomic_store(&gVisionState, 1);
    ActHookSwapGroup(HG_CLIENT);     // 2. client hooks (photo, coordinator, preview, recfix, Vision, diag)
    if (gTestSlowSwapMs > 0) ActSleepMs(gTestSlowSwapMs);
    if (ActTestFailIs("swapthrow"))
        @throw [NSException exceptionWithName:@"SimcamTestSwapThrow"
                                       reason:@"SIMCAM_TEST_ACTIVATE_FAIL=swapthrow"
                                     userInfo:nil];
    if (gSessHookMode == SESS_HOOK_GOT) SessGOTSwap();   // 3. session hook
    else ActHookSwapGroup(HG_SESSION);
    ActHookSwapGroup(HG_AUDIO);      // 4. audio source hook, isConnected
    ActHookSwapGroup(HG_VIDEO);      // 5. video source hook, last
    SCInstallFull();                 // 6. full crash handler
    CtlEnqueue();                    // 7. orientation apply (main), QR warm-up (global)
    CoordPublish();                // 8. publish gCoords (release)
}

// ---- START (a separate work item on simcam.activate; nobody waits for it) ----
static void ActStart(void *ctx) {
    @autoreleasepool {
        @try {
            SCStart();               // 1-3. activate line, gSCOn, watchdog, atexit
            if (gTestSlowStartMs > 0) {
                LOG(@"SIMCAM_TEST_SLOW_START_MS: START sleeps %ld ms (the sentinel is live)", gTestSlowStartMs);
                ActSleepMs(gTestSlowStartMs);
            }
            if (ActTestFailIs("crashstart") || ActTestFailIs("crash2"))   // test faults (trace)
                ActTestCrashInStart(ActTestFailIs("crash2") ? 2 : 1);
            CtlStart();              // 4. control-file timer, observers
            VisionStart();         // 5. Vision's add-image hook, if it wasn't loaded at prepare
            // 6. SWAP's results, then the armed line, last
            static const char *gname[] = {"client", "session", "audio", "video"};
            for (int i = 0; i < gActNHooks; i++) {
                ActHook *h = &gActHooks[i];
                LOG(@"swap: %s %s[%s %s] (%s)%s%s", gname[h->group], class_isMetaClass(h->cls) ? "+" : "-",
                    class_getName(h->cls), sel_getName(h->sel), h->what,
                    h->rc == 2 ? " chained to an IMP changed since prepare" : "", h->rc == 0 ? " NOT INSTALLED" : "");
            }
            if (gSessHookMode == SESS_HOOK_GOT) LOG(@"session hook: got, %d of %d entries rebound", gGotPatched, gGotN);
            if (gActSwapExc[0])
                LOG(@"SWAP threw %s: armed but degraded; recorded as crash activate (a second one latches the next "
                    @"launch)",
                    gActSwapExc);
            if (gMotionAt && !gMotionStockN)
                LOG(@"motion: stand-in hooks installed at load, %.1f ms after process start (before arming)",
                    (gMotionAt - ActProcessStartTime()) * 1000);
            if (gMotionStockN)
                LOG(@"motion: CoreMotion stock this launch from %s (decided at load; the camera is armed)",
                    gSCMotionStockAt);
            Dl_info di = {0};
            const char *callerImage =
                gActCallerRA && dladdr(gActCallerRA, &di) && di.dli_fname ? di.dli_fname : "?";
            const char *callerBase = strrchr(callerImage, '/');
            callerBase = callerBase ? callerBase + 1 : callerImage;
            uint64_t t0 = gActT0;
            LOG(@"TIMING constructors (gate..prevrot) done %.1f ms after process start",
                (CFAbsoluteTimeGetCurrent() - ActProcessStartTime()) * 1000);
            LOG(@"LAZY armed by %s on %s thread in %.1f ms (%.1f ms after process start); caller %s, queue %s; "
                @"prepare: sentinel %.1f lookups %.1f preflight %.1f resolve %.1f ms; swap %.2f ms; budget %ld ms%s",
                gActWhy, gActWhyMain ? "main" : "bg", (gActTArmed - t0) / 1e6,
                (CFAbsoluteTimeGetCurrent() - ActProcessStartTime()) * 1000, callerBase,
                gActQueueLabel[0] ? gActQueueLabel : "-", (gActTPrep[PH_LOOKUPS] - gActTPrep[PH_SENTINEL]) / 1e6,
                (gActTPrep[PH_PREFLIGHT] - gActTPrep[PH_LOOKUPS]) / 1e6,
                (gActTPrep[PH_RESOLVE] - gActTPrep[PH_PREFLIGHT]) / 1e6, (gActTSwap0 - gActTPrep[PH_RESOLVE]) / 1e6,
                (gActTArmed - gActTSwap0) / 1e6, ActBudget(),
                gActInline ? (gActLazy ? " (inline: SIMCAM_OFF_ACTIVATION_TIMEOUT)" : " (inline: SIMCAM_LAZY=0)") : "");
            if (ActTestFailIs("swapthrow"))
                @throw [NSException exceptionWithName:@"SimcamTestStartThrow"
                                               reason:@"SIMCAM_TEST_ACTIVATE_FAIL=swapthrow (START)"
                                             userInfo:nil];
        } @catch (NSException *e) {
            snprintf(gActExc, sizeof gActExc, "%s", e.name.UTF8String ?: "?");
            LOG(@"START threw %@: %@", e.name, e.reason);
            if (ActStrikeOnce()) ActRecordExceptionF(NULL);
            else LOG(@"activation: START threw %s; not recorded (SWAP's exception is this launch's strike)", gActExc);
        }
    }
}

// ---- ActRun: PREPARE, the CAS, SWAP, publish ARMED; on simcam.activate (or inline in the test modes) ----
// a launch that stays stock writes no more events (the record queue appends through its own fd)
static void SCCloseEventsFd(void) {
    int fd = gSCFd;
    gSCFd = -1;
    if (fd >= 0) close(fd);
}
static void ActStockAfterPrepare(int r) {
    SCRestoreConditional();   // before STOCK is published
    atomic_store(&gActRunning, 0);
    int expected = ACT_RUNNING;
    BOOL won = atomic_compare_exchange_strong_explicit(&gAct, &expected, ACT_STOCK, memory_order_release,
                                                       memory_order_relaxed);
    SCCloseEventsFd();
    uint64_t ms = (ActNow() - gActT0) / NSEC_PER_MSEC;
    switch (r) {
        case AP_LATCHED:
            ActRecLog(YES, "%s", gSCLatchMsg);
            break;
        case AP_PREFLIGHT:
        case AP_FORCED:
            ActRecLog(NO,
                      "SIMCAM DISABLED: preflight failed%s, no hooks installed; the app sees the stock simulator (no "
                      "camera)",
                      r == AP_FORCED ? " (forced)" : "");
            ActRecLog(YES,
                      "simcam: DISABLED - preflight failed (see log); the app sees the stock simulator (no camera)");
            break;
        case AP_MISSING:
            ActRecLog(YES, "simcam: activation failed: %s not found; this launch has no camera", gActMissing);
            break;
        // recorded only if this CAS won: after a timeout the waiter already recorded this launch's one strike
        case AP_THROW:
            if (won) {
                (void)ActStrikeOnce();
                dispatch_async_f(gActRecQ, NULL, ActRecordExceptionF);
                ActRecLog(YES, "simcam: activation threw %s; this launch has no camera", gActExc);
            } else
                ActRecLog(NO, "activation: prepare threw %s after the timeout; not recorded (the timeout is this "
                              "launch's strike)",
                          gActExc);
            break;
    }
    if (!won)
        ActRecLog(NO, "activation: prepare failed after %llu ms, after a caller had already given up; staying stock",
                  ms);
}
static void ActRun(void *ctx) {
    gActThread = 1;
    atomic_store(&gActRunning, 1);
    int r;
    @autoreleasepool {
        @try {
            r = ActPrepare();
        } @catch (NSException *e) {
            snprintf(gActExc, sizeof gActExc, "%s", e.name.UTF8String ?: "?");
            r = AP_THROW;
            LOG(@"activation: prepare threw %@: %@", e.name, e.reason);
        }
    }
    if (r != AP_OK) {
        ActStockAfterPrepare(r);
        if (!gActInline) dispatch_group_leave(gActGroup);
        gActThread = 0;
        return;
    }
    int expected = ACT_RUNNING;
    if (!atomic_compare_exchange_strong(&gAct, &expected, ACT_COMMITTING)) {   // a waiter timed out: install nothing
        atomic_store(&gActRunning, 0);
        SCRestoreConditional();
        SCCloseEventsFd();
        ActRecLog(NO, "activation: finished after %llu ms; staying stock (a caller gave up at %ld ms)",
                  (ActNow() - gActT0) / NSEC_PER_MSEC, ActBudget());
        if (!gActInline) dispatch_group_leave(gActGroup);
        gActThread = 0;
        return;
    }
    @try {
        ActSwap();
    } @catch (NSException *e) {   // ARMED-degraded
        snprintf(gActSwapExc, sizeof gActSwapExc, "%s", e.name.UTF8String ?: "?");
    }
    gActTArmed = ActNow();
    atomic_store_explicit(&gAct, ACT_ARMED, memory_order_release);
    atomic_store(&gActRunning, 0);
    atomic_store(&gActPhase, PH_DONE);
    TRArm();   // the op ring records from here (the activate op)
    if (gActInline) {
        gActThread = 0;
        TRArmFile();
        ActStart(NULL);
        return;
    }
    dispatch_group_leave(gActGroup);
    TRArmFile();   // the trace's directory, after the waiters are released (file I/O; nobody waits for it)
    gActThread = 0;
    dispatch_async_f(gActQ, NULL, ActStart);
}

// ---- callers ----
// ActTimedOut: the waiter whose CAS won. gActRunning first, then the conditional restore (before it returns to its
// caller: this covers a prepare that never finishes), then the record, dispatched at once.
static void ActTimedOut(void) {
    atomic_store(&gActRunning, 0);
    SCRestoreConditional();
    int ph = atomic_load(&gActPhase);
    if (ActStrikeOnce()) dispatch_async_f(gActRecQ, (void *)(long)ph, ActRecordTimeoutF);
    ActRecLog(YES, "simcam: activation took over %ld ms (in %s); this launch has no camera", ActBudget(),
              kActPhaseName[ph]);
}
static int ActWait(void) {
    for (;;) {
        int s = ActState();
        if (s == ACT_ARMED || s == ACT_STOCK) return s;
        uint64_t deadline = atomic_load(&gActDeadline), sliceStart = ActNow();
        int64_t left = (int64_t)(deadline - sliceStart), lo = (int64_t)NSEC_PER_MSEC, hi = 100 * (int64_t)NSEC_PER_MSEC;
        int64_t slice = left < lo ? lo : left > hi ? hi : left;
        if (dispatch_group_wait(gActGroup, dispatch_time(DISPATCH_TIME_NOW, slice)) == 0)
            continue;   // done: re-read the state
        uint64_t gap = ActNow() - sliceStart;
        // stopped (lldb, SIGSTOP, sleep): exactly one waiter extends the deadline
        if (gap > (uint64_t)slice + 500 * NSEC_PER_MSEC) {
            if (atomic_compare_exchange_strong(&gActDeadline, &deadline, deadline + gap))
                ActRecLog(NO, "activation: process was stopped for %.1f s; deadline +%.1f s", gap / 1e9, gap / 1e9);
            continue;
        }
        if (s == ACT_COMMITTING) {   // never time out a SWAP
            if (ActNow() >= atomic_load(&gActDeadline) && !atomic_exchange(&gActOverrunNoted, 1))
                ActRecLog(YES, "simcam: activation's install (SWAP) overran the %ld ms budget; waiting for it "
                               "(COMMITTING overran)",
                          ActBudget());
            continue;
        }
        if (ActNow() >= atomic_load(&gActDeadline)) {
            // re-measure right before the CAS: a waiter stopped after its gap check (SIGSTOP, lldb) must not go STOCK
            // before the waiter that saw the stop extends the deadline; it treats the stop as its own and loops
            uint64_t gap2 = ActNow() - sliceStart;
            if (gap2 > (uint64_t)slice + 500 * NSEC_PER_MSEC) {
                if (atomic_compare_exchange_strong(&gActDeadline, &deadline, deadline + gap2))
                    ActRecLog(NO, "activation: process was stopped for %.1f s; deadline +%.1f s", gap2 / 1e9,
                              gap2 / 1e9);
                continue;
            }
            int expected = ACT_RUNNING;
            if (atomic_compare_exchange_strong_explicit(&gAct, &expected, ACT_STOCK, memory_order_release,
                                                        memory_order_relaxed)) {
                ActTimedOut();
                return ACT_STOCK;
            }
        }
    }
}
typedef struct { const char *why; void *ra; } ActCaller;
static void ActBegin(void *ctx) {   // the dispatch_once body: microseconds, never waits
    ActCaller *c = ctx;
    gActT0 = ActNow();
    gActTPrep[PH_START] = gActT0;
    atomic_store(&gSCTraced, SCIsTraced());   // one sysctl, no lock
    // only the default is x10
    if (!gActBudgetExplicit && (atomic_load(&gSCTraced) || gSCScale > 1)) gActBudgetMs *= 10;
    atomic_store(&gActDeadline, gActT0 + (uint64_t)gActBudgetMs * NSEC_PER_MSEC);
    snprintf(gActWhy, sizeof gActWhy, "%s", c->why);
    gActWhyMain = pthread_main_np();
    gActCallerRA = c->ra;
    const char *queueLabel = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
    snprintf(gActQueueLabel, sizeof gActQueueLabel, "%s", queueLabel ? queueLabel : "");
    OrientTrackStartForCamera();   // enqueued, so the tracker runs while prepare does
    if (gTestFigFaultAtMs > 0)
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, gTestFigFaultAtMs * (int64_t)NSEC_PER_MSEC),
                         dispatch_get_global_queue(0, 0), NULL, ActFigFaultF);
    atomic_store(&gAct, ACT_RUNNING);
    if (gOffActivationTimeout) {   // L56 negative control: the inline path, no bound
        gActInline = YES;
        ActRun(NULL);
        return;
    }
    dispatch_group_enter(gActGroup);
    dispatch_async_f(gActQ, NULL, ActRun);
}
// SIMCAM_TEST_TRAP_FAULT=1 (act.inc): once per process, in the first camera call's trap after it armed
__attribute__((noinline)) static void ActTrapFault(void) {
    static _Atomic int once;
    if (atomic_exchange(&once, 1)) return;
    const void *k = atomic_load(&gSCFeatKeyOn) ? pthread_getspecific(gSCFeatKey) : NULL;
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_TRAP_FAULT: fault in ActEnsure now (%s thread, feature key %s)\n",
            getpid(), pthread_main_np() ? "main" : "bg",
            k == (const void *)kSCMotion ? "motion" : k ? "other" : "none");
    TestFaultNullWrite(1);
}
// every trapped call: returns the state the caller proceeds in (the trap forwards to the saved original either way:
// ARMED reaches the SWAPped hooks, STOCK reaches Apple's unhooked methods)
static int ActEnsure(const char *why, void *ra) {
    int s = ActState();
    if (s == ACT_ARMED) return s;
    if (s == ACT_STOCK) {
        if (gSCMarkerAtLoad[0] && !atomic_exchange(&gActStockNoted, 1))
            dispatch_async_f(gActRecQ, NULL, ActMarkerNoteF);
        return s;
    }
    // re-entrant (activation's own call, or AVF's dispatch_sync onto its queue from it)
    if (gActThread || dispatch_get_specific(&kActQKey)) {
        if (!atomic_exchange(&gActReenterNoted, 1))
            ActRecLog(NO, "activation: re-entrant call %s forwarded to stock (no wait)", why);
        return s;
    }
    uint64_t t = ActNow();
    ActCaller c = {why, ra};
    dispatch_once_f(&gActOnce, &c, ActBegin);
    s = gActInline ? ActState() : ActWait();
    uint64_t waited = ActNow() - t;
    if (waited > 50 * NSEC_PER_MSEC)
        ActRecLog(NO, "TIMING first camera call %s on %s thread waited %.1f ms -> %s", why,
                  pthread_main_np() ? "main" : "bg", waited / 1e6, s == ACT_ARMED ? "armed" : "stock");
    if (gTestTrapFault && s == ACT_ARMED) ActTrapFault();
    // a RotationCoordinator built in the same run-loop turn as the first call reads the real orientation
    // (camera code: run with the feature key cleared, whatever the calling thread carries).
    // SIMCAM_TEST_ORIENT_FAULT=ensure sets the motion key first, as a call from inside a motion entry point would carry
    // it; SIMCAM_OFF_ENSURE_KEY_CLEAR=1 (ledger L63) skips the clear.
    if (s == ACT_ARMED && pthread_main_np() && !atomic_exchange(&gActMainUpd, 1)) {
        void *injected = gTestOrientFault == ORF_ENSURE ? SCFeatEnter(kSCMotion) : NULL;
        void *saved = gOffEnsureKeyClear ? NULL : SCFeatEnter(NULL);
        OrientTrackRunMain();
        if (!gOffEnsureKeyClear) SCFeatLeave(saved);
        if (gTestOrientFault == ORF_ENSURE) SCFeatLeave(injected);
    }
    return s;
}
static void ActInstallTrap(Class c, SEL s, BOOL meta) {
    Class target = meta ? object_getClass(c) : c;
    Method m = target ? class_getInstanceMethod(target, s) : NULL;
    if (!m) return;
    IMP orig = method_getImplementation(m);
    const char *name = sel_getName(s);
    // every trapped method takes <= 3 object/integer args and returns an object pointer
    IMP rep = imp_implementationWithBlock(^void *(void *self_, void *a, void *b, void *c2) {
        (void)ActEnsure(name, __builtin_return_address(0));
        return ((void *(*)(void *, SEL, void *, void *, void *))orig)(self_, s, a, b, c2);
    });
    // the class's own method only: if it inherits this selector (e.g. -[AVCaptureSession init] coming from NSObject),
    // add an override on the class instead of swizzling the superclass's method for every object in the process
    if (!class_addMethod(target, s, rep, method_getTypeEncoding(m))) method_setImplementation(m, rep);
}
// SIMCAM_ACTIVATE_TIMEOUT_MS=<1-60000>: an explicit budget (never scaled); anything else keeps the default, with a line
static void ActReadBudget(void) {
    const char *t = getenv(ENV_ACTIVATE_TIMEOUT_MS);
    if (t) {
        char *end = NULL;
        long v = strtol(t, &end, 10);
        if (*t && end && !*end && v >= 1 && v <= 60000) {
            gActBudgetMs = v;
            gActBudgetExplicit = YES;
        } else
            fprintf(stderr,
                    "simcam: SIMCAM_ACTIVATE_TIMEOUT_MS=%s is not an integer in 1-60000; using the default 1000 ms\n",
                    t);
    }
}
// the private serial queues (simcam.activate, marked for the re-entrancy check; simcam.record) and the waiters' group
static void ActCreateQueues(void) {
    gActQ = dispatch_queue_create(
        "simcam.activate", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    dispatch_queue_set_specific(gActQ, &kActQKey, &kActQKey, NULL);
    gActRecQ = dispatch_queue_create(
        "simcam.record", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    gActGroup = dispatch_group_create();
}
// CoreMotion is a public class, so its stand-in's hooks go in at load (design A-MF2): an app that asks for the
// accelerometer before touching the camera must get the same answer as one that asks after. SIMCAM_OFF_MOTION=1 keeps
// CoreMotion stock. Decided once, here, from markers only: this build's camera latch, motion latch or real preflight
// failure (sentinel on, no clear) makes CoreMotion stock for the process, with one-shot forwarding traps that say so
// once.
static void MotionInstallAtLoad(void) {
    SCFeatKeyInit();   // the feature key that attributes stand-in crashes to `motion` (core/safety.inc)
    if (!getenv(ENV_OFF_MOTION)) {
        if (SCMotionStockAtLoad()) MotionInstallStockTraps();
        else MotionInstallHooks();
    }
}
// SIMCAM_LAZY=0: the phases inline at load, no timeout (test mode)
static void ActRunAtLoad(void) {
    gActInline = YES;
    gActT0 = ActNow();
    gActTPrep[PH_START] = gActT0;
    snprintf(gActWhy, sizeof gActWhy, "SIMCAM_LAZY=0 (load)");
    gActWhyMain = pthread_main_np();
    atomic_store(&gSCTraced, SCIsTraced());
    if (SCMarkerValidAtLoad()) {
        atomic_store(&gAct, ACT_STOCK);
        ActMarkerNoteF(NULL);
        return;
    }
    OrientTrackStartForCamera();
    atomic_store(&gAct, ACT_RUNNING);
    ActRun(NULL);
}
// the traps on the public capture entry points: the first call into one starts activation
static void ActInstallTraps(void) {
    Class device = objc_getClass("AVCaptureDevice");
    Class discovery = objc_getClass("AVCaptureDeviceDiscoverySession");
    Class session = objc_getClass("AVCaptureSession");
    SEL deviceSels[] = {@selector(defaultDeviceWithMediaType:),
                        @selector(defaultDeviceWithDeviceType:mediaType:position:),
                        @selector(devices),
                        @selector(devicesWithMediaType:),
                        @selector(deviceWithUniqueID:),
                        @selector(systemPreferredCamera)};
    for (unsigned i = 0; i < sizeof deviceSels / sizeof *deviceSels; i++) ActInstallTrap(device, deviceSels[i], YES);
    ActInstallTrap(discovery, @selector(discoverySessionWithDeviceTypes:mediaType:position:), YES);
    ActInstallTrap(session, @selector(init), NO);
}
__attribute__((constructor)) static void ActInstallAtLoad(void) {
    const char *e = getenv(ENV_SIMCAM);
    if (e && e[0] == '0') return;
    if (!ActIsPrimaryEngine()) return;   // a second engine in this process stays inert (core/init.inc)
    {
        const char *lazyEnv = getenv(ENV_LAZY);
        gActLazy = (lazyEnv && lazyEnv[0] == '0') ? 0 : 1;
    }
    // computed once, at load: budget, sanitizer scale, flags, bundle id, build, both state paths, marker stat()s
    ActReadBudget();
    gActFakeBreak = getenv(ENV_PF_BREAK) != NULL;
    ActParseTestEnv();
    SCLoadInit();
    // SIMCAM_SENTINEL=clear: the clear line, markers unlinked (an explicit action: I/O at load is fine)
    SCClearAtLoad();
    ActCreateQueues();
    MotionInstallAtLoad();
    if (!gActLazy) {
        ActRunAtLoad();
        return;
    }
    // a valid latched-<build> marker (sentinel on, no clear): STOCK from the start, no prepare (one line at the first
    // call)
    if (SCMarkerValidAtLoad()) atomic_store(&gAct, ACT_STOCK);
    ActInstallTraps();
    // One main-queue block that records the UI orientation once (no timer, no notifications), so a coordinator built
    // off main while main is busy reads the right preview angle at once. Not in a marker-STOCK launch.
    if (atomic_load(&gAct) != ACT_STOCK) OrientUISnapAtLoad();
}
