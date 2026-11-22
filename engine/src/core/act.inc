// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): bounded activation's shared state (the activation
// itself, and the phases PREPARE, SWAP and START, are in core/lazy.inc).
//
// Provides: ActivationState, ActivationIsArmed, the state machine (core/lazy.inc, core/sentinel_activate.inc, client/)
//           ActivationTestSlowAt, kActivationPhaseNames, the activation's steps (core/lazy.inc, the sentinel fragments)
//           ActivationHookResolve, the hook list (each fragment that resolves a hook; core/lazy.inc swaps them)
//           MonotonicNowNs, SlicedSleepMs, ActivationStrikeOnce, ActivationTestFailIs, TestFaultNullWrite (core/)
// Owns:     nothing: the activation's state (gActivation, gActivationThread, kActivationPhaseNames) and the test
//           switches (gTest*, gOff*) are defined in core/activation.h; ActivationParseTestEnv sets the switches at load
// Uses:     nothing.
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "activation.h"

static int ActivationState(void) { return atomic_load_explicit(&gActivation.state, memory_order_acquire); }

static BOOL ActivationIsArmed(void) { return ActivationState() == kActivationArmed; }

static uint64_t MonotonicNowNs(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

// Sets the test switches (core/activation.h) once, at load. The other CoreMotion off switches are read elsewhere:
// SIMCAM_OFF_MOTION_MARKERS (by SentinelMotionStockAtLoad) and SIMCAM_OFF_MOTION_KEY (by SentinelFeatureKeyInit) where
// they act, and SIMCAM_OFF_HANDLER_MOTION_MARKER by SentinelParseTestEnv at load.
static void ActivationParseTestEnv(void) {
    gTestPreflightBreak = getenv(ENV_PF_BREAK) != NULL;
    const char *value = getenv(ENV_TEST_SLOW_ACTIVATE_MS);

    if (value) {
        gTestSlowActivateMs = atol(value);
        const char *at = strchr(value, '@');
        if (at) {
            at++;
            for (int i = kActivationPhaseTrim; i <= kActivationPhaseResolve; i++) {
                if (!strcmp(at, kActivationPhaseNames[i])) {
                    gTestSlowActivateAt = i;
                }
            }
        }
    }

    if ((value = getenv(ENV_TEST_SLOW_SWAP_MS))) {
        gTestSlowSwapMs = atol(value);
    }
    if ((value = getenv(ENV_TEST_SLOW_START_MS))) {
        gTestSlowStartMs = atol(value);
    }
    if ((value = getenv(ENV_TEST_FIG_FAULT_AT_MS))) {
        gTestFigFaultAtMs = atol(value);
    }
    if ((value = getenv(ENV_TEST_ACTIVATE_FAIL))) {
        snprintf(gTestActivateFail, sizeof gTestActivateFail, "%s", value);
    }
    gTestPfFailReal = getenv(ENV_TEST_PF_FAIL_REAL) != NULL;
    gTestActivateNeedsMain = getenv(ENV_TEST_ACTIVATE_NEEDS_MAIN) != NULL;
    gTestReenter = getenv(ENV_TEST_REENTER) != NULL;
    gOffActivationTimeout = getenv(ENV_OFF_ACTIVATION_TIMEOUT) != NULL;

    // No UI orientation snapshot at load: a coordinator built off main while main is busy then reads portrait angles
    // until main frees.
    gOffUIOrientationAtLoad = getenv(ENV_OFF_UI_ORIENTATION_AT_LOAD) != NULL;
    gTestMotionFault = getenv(ENV_TEST_MOTION_FAULT) != NULL;

    if ((value = getenv(ENV_TEST_ORIENT_FAULT))) {
        if (!strcmp(value, "start")) {
            gTestOrientFault = kOrientFaultStart;
        } else if (!strcmp(value, "runmain")) {
            gTestOrientFault = kOrientFaultRunMain;
        } else if (!strcmp(value, "ensure")) {
            gTestOrientFault = kOrientFaultEnsure;
        } else {
            gTestOrientFault = kOrientFaultUpd;
        }
    }

    gOffTrackerKeyClear = getenv(ENV_OFF_TRACKER_KEY_CLEAR) != NULL;
    gOffTrackStartKeyClear = getenv(ENV_OFF_TRACKSTART_KEY_CLEAR) != NULL;
    gOffEnsureKeyClear = getenv(ENV_OFF_ENSURE_KEY_CLEAR) != NULL;
    gTestTrapFault = getenv(ENV_TEST_TRAP_FAULT) != NULL;
    gOffAppSendKeyClear = getenv(ENV_OFF_APP_SEND_KEY_CLEAR) != NULL;
}

static BOOL ActivationTestFailIs(const char *what) { return !strcmp(gTestActivateFail, what); }

// For test faults only: writes value through a null pointer, a SIGSEGV whose PC is in our image. The pointer comes
// from atoi("0") so the compiler can't prove it null and drop the write.
static void TestFaultNullWrite(int value) {
    volatile int *target = (volatile int *)(uintptr_t)atoi("0");
    *target = value;
}

// At most one counted activate strike per launch (gActivation.strikeRecorded): YES for the first recorder only.
static BOOL ActivationStrikeOnce(void) { return !atomic_exchange(&gActivation.strikeRecorded, 1); }

// A sliced sleep: kSlicedSleepSliceMs slices, counting only slices that took under kSlicedSleepStallMs, so a SIGSTOP or
// a debugger stop doesn't use it up. It counts the measured length of each slice (nanosleep overshoots by about 1-2 ms
// per slice).
#define kSlicedSleepSliceMs 10
#define kSlicedSleepStallMs 50

static void SlicedSleepMs(long ms) {
    uint64_t done = 0;
    uint64_t want = (uint64_t)ms * NSEC_PER_MSEC;
    while (done < want) {
        uint64_t left = want - done;
        uint64_t slice = left < kSlicedSleepSliceMs * NSEC_PER_MSEC ? left : kSlicedSleepSliceMs * NSEC_PER_MSEC;
        uint64_t sliceStart = MonotonicNowNs();
        struct timespec ts = {0, (long)slice};
        nanosleep(&ts, NULL);
        uint64_t slept = MonotonicNowNs() - sliceStart;
        if (slept < kSlicedSleepStallMs * NSEC_PER_MSEC) {
            done += slept;
        }
    }
}

static void ActivationTestSlowAt(int phase) {
    if (gTestSlowActivateMs > 0 && gTestSlowActivateAt == phase) {
        SlicedSleepMs(gTestSlowActivateMs);
    }
}

// ---- hooks: resolved in PREPARE's resolve step, installed in SWAP ----
// SWAP is memory-only: no messages, no I/O, no dyld lock. A hook records its Method (or, for add-style hooks, the
// class to add on), its replacement and where the hook's original IMP lives. SWAP installs group by group (client,
// session, audio, video, in that order) and chains to the IMP that method_setImplementation or class_replaceMethod
// returned, so a swizzle made by someone else between PREPARE and SWAP is kept.
// The hook groups, the hook record and the list are declared in core/activation.h.

// Resolves one hook. With addsOverride, the hook takes the class's own method if it has one, else it adds an override
// on the class (class_replaceMethod) that chains to the inherited IMP. Returns NO when a required method is missing,
// which sends the activation to STOCK.
static BOOL ActivationHookResolve(int group, const char *what, Class cls, const char *selName, BOOL meta,
                                  BOOL addsOverride, IMP replacement, IMP *origSlot, BOOL required) {
    Class target = cls && meta ? object_getClass(cls) : cls;
    SEL selector = sel_registerName(selName);
    Method method = target ? class_getInstanceMethod(target, selector) : NULL;

    if (!method || gActivation.hooks.count >= kActivationMaxHooks) {
        if (required && !gActivation.hooks.missing[0]) {
            snprintf(gActivation.hooks.missing, sizeof gActivation.hooks.missing, "%s: [%s %s]", what,
                     target ? class_getName(target) : "nil", selName);
        }
        return !required;
    }

    gActivation.hooks.list[gActivation.hooks.count++] = (ActivationHook){what,
                                        target,
                                        selector,
                                        method,
                                        replacement,
                                        origSlot,
                                        method_getImplementation(method),
                                        method_getTypeEncoding(method),
                                        addsOverride,
                                        group,
                                        kHookNotInstalled};
    return YES;
}

// SWAP: installs one group and records each hook's result in swapResult.
static void ActivationHookSwapGroup(int group) {
    for (int i = 0; i < gActivation.hooks.count; i++) {
        ActivationHook *hook = &gActivation.hooks.list[i];
        if (hook->group != group) {
            continue;
        }
        // set before the hook is live, so a call through it never reads NULL
        if (hook->origSlot) {
            *hook->origSlot = hook->preparedImp;
        }
        IMP replaced;
        if (hook->addsOverride) {
            replaced = class_replaceMethod(hook->cls, hook->sel, hook->replacement, hook->typeEncoding);
        } else {
            replaced = method_setImplementation(hook->method, hook->replacement);
        }
        // class_replaceMethod added an override: chain to the inherited IMP read in PREPARE
        if (!replaced) {
            replaced = hook->preparedImp;
        }
        if (hook->origSlot && replaced != hook->preparedImp) {
            *hook->origSlot = replaced;
        }
        hook->swapResult = replaced == hook->preparedImp ? kHookChainedToPrepared : kHookChainedToNewer;
    }
}
