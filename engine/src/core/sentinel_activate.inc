// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/events_file.inc): the sentinel's
// activation steps: PREPARE's sentinel step, SentinelPrepare (read the past, decide latches), and SentinelStart, the
// first thing START does (the sentinel goes live).
//
// Provides: SentinelPrepare and its kSentinelPrepare* results, SentinelStart (core/lazy.inc)
// Owns:     nothing shared (SentinelStart records the exception SWAP threw, from core/activation.h).
// Uses:     core/act.inc, core/safety.inc, core/crash_handler.inc, core/markers.inc, core/events_file.inc
#include "activation.h"
#include "sentinel.h"

// ---- PREPARE's sentinel step (read the past, decide latches): runs on simcam.activate ----
// Step 1: the state dir, else the container-tmp fallback (for iOS 17.5). Returns the index of the first one events.log
// opens in, or -1.
static int SentinelPickEventsDir(void) {
    int dirIndex = -1;
    for (int i = 0; i < kStateDirCount && dirIndex < 0; i++) {
        if (!gSentinel.paths.eventsDirs[i][0] || SentinelMkdirP(gSentinel.paths.eventsDirs[i])) {
            continue;
        }
        char path[kSentinelPathBytes];
        snprintf(path, sizeof path, "%s/events.log", gSentinel.paths.eventsDirs[i]);
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) {
            close(fd);
            dirIndex = i;
        }
    }
    return dirIndex;
}

// Step 3: the past (every line after the last clear, split into fields), and this activation's number, published for
// the record queue.
static NSMutableArray *SentinelReadPast(void) {
    NSArray *lines =
        [[NSString stringWithContentsOfFile:@(gSentinel.events.path) encoding:NSUTF8StringEncoding error:nil]
            componentsSeparatedByString:@"\n"];
    NSMutableArray *past = [NSMutableArray array];

    for (NSString *line in lines) {
        NSArray *fields = [line componentsSeparatedByString:@" "];
        if (fields.count < kEventMinFields) {
            continue;
        }
        if ([fields[kEventFieldKind] isEqual:@"clear"]) {
            [past removeAllObjects];
        } else {
            [past addObject:fields];
        }
    }

    // The activation number is the highest on record plus one (not a count of lines, so trimming the file keeps the
    // latch window right). If this launch's timeout line is already written, its number is reused.
    int maxAct = 0;

    for (NSString *line in lines) {
        NSArray *fields = [line componentsSeparatedByString:@" "];
        if (fields.count >= kEventMinFields) {
            maxAct = MAX(maxAct, [fields[kEventFieldActivation] intValue]);
        }
    }

    int recorded = atomic_load(&gActivation.timeoutActivationNumber);
    gSentinel.events.activationNumber = recorded ? recorded : maxAct + 1;
    atomic_store(&gSentinel.events.activationNumberPublished, gSentinel.events.activationNumber);
    atomic_store_explicit(&gSentinel.events.pathPublished, gSentinel.events.path, memory_order_release);
    return past;
}

// Step 3: a previous traced activation that ended inside an op (with no crash or exit line) is reported, never
// counted.
static void SentinelNoteLastDebugRun(NSArray *past) {
    NSInteger lastAct = -1;

    for (NSInteger i = past.count - 1; i >= 0; i--) {
        if ([past[i][kEventFieldKind] isEqual:@"activate"]) {
            lastAct = i;
            break;
        }
    }

    if (lastAct < 0) {
        return;
    }
    NSArray *activateLine = past[lastAct];
    BOOL traced = activateLine.count > kEventFieldDetail && [activateLine[kEventFieldDetail] isEqual:@"traced=1"];
    BOOL ended = NO;
    NSMutableDictionary *openOps = [NSMutableDictionary dictionary];   // slot= -> feature of the ops not ended
    for (NSInteger i = lastAct + 1; i < (NSInteger)past.count; i++) {
        NSArray *fields = past[i];
        NSString *kind = fields[kEventFieldKind];
        if ([kind isEqual:@"exit"] || [kind hasPrefix:@"crash"]) {
            ended = YES;
        } else if ([kind isEqual:@"op-begin"] && fields.count > kEventFieldDetail) {
            openOps[fields[kEventFieldDetail]] = fields[kEventFieldFeature];
        } else if ([kind isEqual:@"op-end"] && fields.count > kEventFieldDetail) {
            [openOps removeObjectForKey:fields[kEventFieldDetail]];
        }
    }

    if (traced && !ended && openOps.count) {
        NSString *feature = openOps.allValues.firstObject;
        LOG(@"sentinel: last debug run ended inside %@ op", feature);
        fprintf(stderr,
                "simcam: last debug run ended inside %s op; if simcam crashed: SIMCAM_SENTINEL_LATCH=%s on the "
                "next launch\n",
                feature.UTF8String, feature.UTF8String);
    }
}

// Step 5: this build's motion strikes before this launch, for the full handler's latched-motion marker.
static void SentinelPrepareMotionMarker(const int counts[kFeatureCount], const long newest[kFeatureCount]) {
    gCrashHandler.motionMarker.priorStrikes = counts[kFeatureMotion];
    gCrashHandler.motionMarker.newestStrikeEpoch = newest[kFeatureMotion];
    snprintf(gCrashHandler.motionMarker.path, sizeof gCrashHandler.motionMarker.path, "%s",
             gSentinel.paths.motionMarker[gSentinel.events.dirIndex]);
    snprintf(gCrashHandler.motionMarker.tempPath, sizeof gCrashHandler.motionMarker.tempPath,
             "%s/.latched-motion-%s.tmp-%d", gSentinel.paths.eventsDirs[gSentinel.events.dirIndex], gSentinel.build,
             getpid());
}

// Steps 1 and 2: the events file. The directory is the state dir, else the container-tmp fallback (for iOS 17.5),
// whichever events.log opens in first. The trim (only past kSentinelTrimAboveLines lines) comes before the open. It
// sets gSentinel.events. NO: the sentinel is off for this launch.
static BOOL SentinelOpenEventsFile(void) {
    int dirIndex = SentinelPickEventsDir();

    if (dirIndex < 0) {
        LOG(@"sentinel: cannot open events.log in %s or %s; ops/sentinel off", gSentinel.paths.eventsDirs[0],
            gSentinel.paths.eventsDirs[1]);
        return NO;
    }

    snprintf(gSentinel.events.path, sizeof gSentinel.events.path, "%s/events.log",
             gSentinel.paths.eventsDirs[dirIndex]);
    gSentinel.events.dirIndex = dirIndex;
    SentinelTrim();
    int fd = open(gSentinel.events.path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0) {
        LOG(@"sentinel: cannot open %s; ops/sentinel off", gSentinel.events.path);
        return NO;
    }

    gSentinel.events.fd = fd;
    if (gSentinel.mode == kSentinelModeClear && !gSentinel.atLoad.cleared) {
        SentinelWriteEvent("clear", "-", "SIMCAM_SENTINEL=clear", -1);
    }
    return YES;
}

// Step 4: the image ranges, then the minimal crash handler, if no waiter has given up meanwhile (checked again after).
static void SentinelInstallMinimalIfRunning(void) {
    SentinelCacheRanges();

    if (ActivationState() == kActivationRunning) {
        SentinelInstallMinimal();
        if (ActivationState() != kActivationRunning) {
            SentinelRestoreConditional();
        }
    }
}

// What the latch decision reads: each feature's strikes with this build in the window, the latest until they give,
// and the newest strike's epoch.
typedef struct {
    long now;
    int counts[kFeatureCount];
    long until[kFeatureCount];
    long newest[kFeatureCount];
} SentinelStrikes;

// Step 5, first part: the manual latch lines, then the strikes. The manual latch (`stuntcam sentinel latch <feature>`
// sets SIMCAM_SENTINEL_LATCH) writes two crash lines and counts as a latch that lasts the whole window.
static void SentinelCountStrikesNow(NSArray *past, SentinelStrikes *strikes) {
    const char *manualLatch = getenv(ENV_SENTINEL_LATCH);

    if (manualLatch && *manualLatch) {
        SentinelWriteEvent("crash", manualLatch, "manual", -1);
        SentinelWriteEvent("crash", manualLatch, "manual", -1);
    }

    strikes->now = time(NULL);
    SentinelCountStrikes(past, @(gSentinel.build), strikes->now, gSentinel.events.activationNumber, strikes->counts,
                         strikes->until, strikes->newest);
    SentinelPrepareMotionMarker(strikes->counts, strikes->newest);

    if (manualLatch && *manualLatch) {
        int featureIndex = SentinelFeatureIndex(manualLatch);
        strikes->counts[featureIndex] += kSentinelStrikesToLatch;
        strikes->until[featureIndex] = MAX(strikes->until[featureIndex], strikes->now + kSentinelWindowSeconds);
    }
}

// Step 5, second part: `motion` latches CoreMotion (from the next launch), never the camera.
static BOOL SentinelMotionLatches(const SentinelStrikes *strikes) {
    BOOL motionLatched = strikes->counts[kFeatureMotion] >= kSentinelStrikesToLatch;

    if (motionLatched) {
        LOG(@"sentinel: motion latched (%d crashes in the last %d activations / %d h): CoreMotion stock from the "
            @"next launch; the camera is unaffected",
            strikes->counts[kFeatureMotion], kSentinelWindowActivations, kSentinelWindowHours);
    }

    return motionLatched;
}

// Step 5, last part: the camera-closing features that latch (kSentinelStrikesToLatch strikes or more), into latch.
static void SentinelCameraLatches(const SentinelStrikes *strikes, SentinelLatch *latch) {
    for (int i = 0; i < kFeatureCount; i++) {
        if (strikes->counts[i] < kSentinelStrikesToLatch || !SentinelClosesCamera(i)) {
            continue;
        }
        size_t featuresLen = strlen(latch->features);
        size_t countsLen = strlen(latch->counts);
        snprintf(latch->features + featuresLen, kSentinelLatchFeaturesCap - featuresLen, "%s%s",
                 latch->camera ? " " : "", kFeatureNames[i]);
        snprintf(latch->counts + countsLen, kSentinelLatchCountsCap - countsLen, "%s%d", latch->camera ? ", " : "",
                 strikes->counts[i]);
        latch->until = MAX(latch->until, strikes->until[i]);
        latch->camera = YES;
        LOG(@"sentinel: %s latched (%d crashes/hangs in the last %d activations / %d h); no camera",
            kFeatureNames[i], strikes->counts[i], kSentinelWindowActivations, kSentinelWindowHours);
    }
}

// Step 7: a camera-closing latch: the latch line for the record queue, and the latched activate line.
static void SentinelRecordLatch(const SentinelLatch *latch) {
    snprintf(gSentinel.latchMessage, sizeof gSentinel.latchMessage,
             "simcam: %s latched after %s crashes/hangs in the last %d activations or %d h; no camera. "
             "Relaunch with SIMCAM_SENTINEL=clear to retry (events: %s)",
             latch->features, latch->counts, kSentinelWindowActivations, kSentinelWindowHours, gSentinel.events.path);
    SentinelWriteEvent("activate", "-", "latched=1 traced=", atomic_load(&gSentinel.traced));
}

// PREPARE's sentinel step. A camera-closing latch is two or more crash or hang events of one feature, with this build,
// in the last 20 activations and 24 h. It makes the process STOCK (the caller restores the handler), and the
// preflight never runs on a latched launch.
static int SentinelPrepare(void) {
    if (gSentinel.mode == kSentinelModeOff) {
        return kSentinelPrepareOff;
    }
    atomic_store(&gActivation.phase, kActivationPhaseSentinel);

    // 1-2. The events file.
    if (!SentinelOpenEventsFile()) {
        return kSentinelPrepareOff;
    }

    // 3. The past: everything after the last clear.
    NSMutableArray *past = SentinelReadPast();
    SentinelNoteLastDebugRun(past);

    // 4. The minimal crash handler.
    SentinelInstallMinimalIfRunning();
    ActivationTestSlowAt(kActivationPhaseSentinel);   // @sentinel: after the parse, before the latch decision

    // 5. The strikes, then the latches.
    SentinelStrikes strikes;
    SentinelCountStrikesNow(past, &strikes);
    SentinelLatch latch = {0};
    latch.coreMotion = SentinelMotionLatches(&strikes);
    latch.motionUntil = strikes.until[kFeatureMotion];
    SentinelCameraLatches(&strikes, &latch);

    // 6. The markers, in both locations.
    SentinelMarkersUpdate(past, strikes.now, &latch);

    // 7. A camera-closing latch.
    if (!latch.camera) {
        return kSentinelPrepareOk;
    }
    SentinelRecordLatch(&latch);
    return kSentinelPrepareLatched;
}

// ---- START, first (after ARMED; nobody waits for it): the sentinel goes live ----
// The conformance tests' hook, SIMCAM_WD_TEST=
//   hang        two photo ops that never end (1 s timeout): two hangs, so an in-process latch, and the next activation
//               is latched
//   pause       a photo op with a 3 s timeout whose work takes 2 s; the host stops the process for more than 3 s
//               meanwhile (kill -STOP <pid>; sleep 10; kill -CONT <pid>): no hang
//   crash       a photo op, then a fault inside our image 1 s later: "crash photo"; twice, and the next activation is
//               latched
//   crash-other abort() with no op inflight: "crash-other", never counted
static void SentinelStartWatchdogTest(void) {
    const char *wdTest = getenv(ENV_WD_TEST);
    if (!wdTest) {
        return;
    }
    LOG(@"sentinel: test %s", wdTest);
    dispatch_queue_t queue = dispatch_queue_create("simcam.wdtest", DISPATCH_QUEUE_SERIAL);

    if (!strcmp(wdTest, "hang")) {
        SentinelOpBegin("photo", 1.0);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), queue, ^{
            SentinelOpBegin("photo", 1.0);
        });
    } else if (!strcmp(wdTest, "pause")) {
        dispatch_async(queue, ^{
            int op = SentinelOpBegin("photo", 3.0);
            LOG(@"sentinel: test pause op started, pid %d", getpid());
            sleep(2);
            SentinelOpEnd(op);
            LOG(@"sentinel: test pause op ended");
        });
    } else if (!strcmp(wdTest, "crash")) {
        dispatch_async(queue, ^{
            SentinelOpBegin("photo", 30.0);
            sleep(1);
            TestFaultNullWrite(1);
        });
    } else if (!strcmp(wdTest, "crash-other")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), queue, ^{
            abort();
        });
    }
}

static void SentinelStart(void) {
    if (gSentinel.events.fd < 0) {
        return;   // the sentinel is off, or events.log could not be opened
    }
    SentinelWriteEvent("activate", "-", "traced=", atomic_load(&gSentinel.traced));

    if (gActivation.swapExceptionName[0] && ActivationStrikeOnce()) {
        char detail[160];
        snprintf(detail, sizeof detail, "exception=%s", gActivation.swapExceptionName);
        SentinelWriteEvent("crash", "activate", detail, -1);
    }

    atomic_store(&gSentinel.live, 1);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // without the thread no op ever expires: hangs go unrecorded, and nothing is latched by mistake
    (void)pthread_create(&thread, &attr, SentinelWatchdog, NULL);
    atexit_b(^{
        SentinelWriteEvent("exit", "-", "atexit", -1);
    });

    LOG(@"sentinel: activation %d build %s traced=%d scale=x%.0f events=%s (self %#lx-%#lx, %d related images)",
        gSentinel.events.activationNumber, gSentinel.build, atomic_load(&gSentinel.traced), gSentinel.scale,
        gSentinel.events.path, (unsigned long)gCrashHandler.selfRange.lo, (unsigned long)gCrashHandler.selfRange.hi,
        atomic_load(&gCrashHandler.relatedCount));
    SentinelStartWatchdogTest();
}
