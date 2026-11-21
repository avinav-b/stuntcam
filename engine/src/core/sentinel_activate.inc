// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/events_file.inc): the sentinel's
// activation steps: PREPARE 1 (read the past, decide latches) and START 1-3 (the sentinel goes live).
// ---- PREPARE 1 (read the past, decide latches): runs on simcam.activate ----
enum { SCP_OK = 0, SCP_OFF, SCP_LATCHED };
#define SC_FEATS_CAP 200   // the latched features' names (the latched-<build> marker's content)
#define SC_NUMS_CAP 100    // their strike counts, for the latch line
// step 1: the state dir, else the container-tmp fallback (17.5): the index of the first one events.log opens in, or -1
static int SCPickEventsDir(void) {
    int idx = -1;
    for (int i = 0; i < 2 && idx < 0; i++) {
        if (!gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/events.log", gSCDir[i]);
        int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) {
            close(fd);
            idx = i;
        }
    }
    return idx;
}
// step 3: the past (every line after the last clear, split into fields), and this activation's number, published for
// the record queue
static NSMutableArray *SCReadPast(void) {
    NSArray *lines = [[NSString stringWithContentsOfFile:@(gSCPath) encoding:NSUTF8StringEncoding error:nil]
                         componentsSeparatedByString:@"\n"];
    NSMutableArray *past = [NSMutableArray array];
    for (NSString *l in lines) {
        NSArray *fields = [l componentsSeparatedByString:@" "];
        if (fields.count < 5) continue;
        if ([fields[2] isEqual:@"clear"]) [past removeAllObjects];
        else [past addObject:fields];
    }
    // activation number = highest on record + 1 (not a count of lines, so trimming the file keeps the latch window
    // right); if this launch's timeout line is already written, its number is reused
    int maxAct = 0;
    for (NSString *l in lines) {
        NSArray *fields = [l componentsSeparatedByString:@" "];
        if (fields.count >= 5) maxAct = MAX(maxAct, [fields[1] intValue]);
    }
    int recorded = atomic_load(&gActRecordedAct);
    gSCAct = recorded ? recorded : maxAct + 1;
    atomic_store(&gSCActPub, gSCAct);
    atomic_store_explicit(&gSCPathPub, gSCPath, memory_order_release);
    return past;
}
// step 3: a previous traced activation that ended inside an op (no crash/exit line) is reported, never counted
static void SCNoteLastDebugRun(NSArray *past) {
    NSInteger lastAct = -1;
    for (NSInteger i = past.count - 1; i >= 0; i--) {
        if ([past[i][2] isEqual:@"activate"]) {
            lastAct = i;
            break;
        }
    }
    if (lastAct >= 0) {
        NSArray *activateLine = past[lastAct];
        BOOL traced = activateLine.count > 5 && [activateLine[5] isEqual:@"traced=1"], ended = NO;
        NSMutableDictionary *open = [NSMutableDictionary dictionary];   // slot= -> feature of ops not ended
        for (NSInteger i = lastAct + 1; i < (NSInteger)past.count; i++) {
            NSArray *fields = past[i];
            NSString *kind = fields[2];
            if ([kind isEqual:@"exit"] || [kind hasPrefix:@"crash"]) ended = YES;
            else if ([kind isEqual:@"op-begin"] && fields.count > 5) open[fields[5]] = fields[3];
            else if ([kind isEqual:@"op-end"] && fields.count > 5) [open removeObjectForKey:fields[5]];
        }
        if (traced && !ended && open.count) {
            NSString *feat = open.allValues.firstObject;
            LOG(@"sentinel: last debug run ended inside %@ op", feat);
            fprintf(stderr,
                    "simcam: last debug run ended inside %s op; if simcam crashed: SIMCAM_SENTINEL_LATCH=%s on the "
                    "next launch\n",
                    feat.UTF8String, feat.UTF8String);
        }
    }
}
// step 5: this build's motion strikes before this launch, for the full handler's latched-motion marker
static void SCPrepareMotionMarker(const int counts[SC_NFEAT], const long newest[SC_NFEAT]) {
    gSCMotionPrior = counts[SC_FMOTION];
    gSCMotionPriorEpoch = newest[SC_FMOTION];
    snprintf(gSCMotionMarkerUse, sizeof gSCMotionMarkerUse, "%s", gSCMotionMarker[gSCIdx]);
    snprintf(gSCMotionMarkerTmp, sizeof gSCMotionMarkerTmp, "%s/.latched-motion-%s.tmp-%d", gSCDir[gSCIdx], gSCBuild,
             getpid());
}
// step 5: the camera-closing features that latch (>= SC_STRIKES_TO_LATCH strikes): their names into feats, their counts
// into nums, the latest until into *untilMax. Returns whether any latched.
static BOOL SCCameraLatches(const int counts[SC_NFEAT], const long until[SC_NFEAT], char feats[SC_FEATS_CAP],
                            char nums[SC_NUMS_CAP], long *untilMax) {
    BOOL latched = NO;
    for (int i = 0; i < SC_NFEAT; i++) {
        if (counts[i] >= SC_STRIKES_TO_LATCH && SCClosesCamera(i)) {
            snprintf(feats + strlen(feats), SC_FEATS_CAP - strlen(feats), "%s%s", latched ? " " : "", gSCFeatures[i]);
            snprintf(nums + strlen(nums), SC_NUMS_CAP - strlen(nums), "%s%d", latched ? ", " : "", counts[i]);
            *untilMax = MAX(*untilMax, until[i]);
            latched = YES;
            LOG(@"sentinel: %s latched (%d crashes/hangs in the last 20 activations / 24 h); no camera", gSCFeatures[i],
                counts[i]);
        }
    }
    return latched;
}
static int SCPrepare(void) {
    if (gSCSentinel == 0) return SCP_OFF;
    atomic_store(&gActPhase, PH_SENTINEL);
    // 1. directory: the state dir, else the container-tmp fallback (17.5): the first one events.log opens in
    int idx = SCPickEventsDir();
    if (idx < 0) {
        LOG(@"sentinel: cannot open events.log in %s or %s; ops/sentinel off", gSCDir[0], gSCDir[1]);
        return SCP_OFF;
    }
    snprintf(gSCPath, sizeof gSCPath, "%s/events.log", gSCDir[idx]);
    gSCIdx = idx;
    // 2. trim (only past 4000 lines), then open
    SCTrim();
    int fd = open(gSCPath, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG(@"sentinel: cannot open %s; ops/sentinel off", gSCPath);
        return SCP_OFF;
    }
    gSCFd = fd;
    if (gSCSentinel == 2 && !gSCClearedAtLoad) SCEventRaw("clear", "-", "SIMCAM_SENTINEL=clear", -1);
    // 3. the past: everything after the last clear
    NSMutableArray *past = SCReadPast();
    SCNoteLastDebugRun(past);
    // 4. image ranges, then the minimal crash handler, if no waiter has given up meanwhile (checked again after)
    SCCacheRanges();
    if (ActState() == ACT_RUNNING) {
        SCInstallMinimal();
        if (ActState() != ACT_RUNNING) SCRestoreConditional();
    }
    ActTestSlowAt(PH_SENTINEL);   // @sentinel: after the parse, before the latch decision
    // 5. manual latch lines, then the latches: >= 2 crash/hang events of one feature, this build, last 20 activations
    // and 24 h
    // manual latch (design: 'stuntcam sentinel latch <feature>')
    const char *manualLatch = getenv(ENV_SENTINEL_LATCH);
    if (manualLatch && *manualLatch) {
        SCEventRaw("crash", manualLatch, "manual", -1);
        SCEventRaw("crash", manualLatch, "manual", -1);
    }
    long now = time(NULL);
    int counts[SC_NFEAT];
    long until[SC_NFEAT], newest[SC_NFEAT];
    SCCountStrikes(past, @(gSCBuild), now, gSCAct, counts, until, newest);
    SCPrepareMotionMarker(counts, newest);
    if (manualLatch && *manualLatch) {
        int fi = SCFeatureIndex(manualLatch);
        counts[fi] += SC_STRIKES_TO_LATCH;
        until[fi] = MAX(until[fi], now + SC_WINDOW_SECONDS);
    }
    BOOL mLatched = counts[SC_FMOTION] >= SC_STRIKES_TO_LATCH;
    if (mLatched)
        LOG(@"sentinel: motion latched (%d crashes in the last 20 activations / 24 h): CoreMotion stock from the next "
            @"launch; the camera is unaffected",
            counts[SC_FMOTION]);
    char feats[SC_FEATS_CAP] = "", nums[SC_NUMS_CAP] = "";
    long untilMax = 0;
    BOOL latched = SCCameraLatches(counts, until, feats, nums, &untilMax);
    // 6. markers, in both locations
    SCMarkersUpdate(past, now, latched, feats, untilMax, mLatched, until[SC_FMOTION]);
    // 7. a camera-closing latch -> STOCK (the caller restores the handler); the preflight never runs on a latched
    // launch
    if (latched) {
        snprintf(gSCLatchMsg, sizeof gSCLatchMsg,
                 "simcam: %s latched after %s crashes/hangs in the last 20 activations or 24 h; no camera. "
                 "Relaunch with SIMCAM_SENTINEL=clear to retry (events: %s)",
                 feats, nums, gSCPath);
        SCEventRaw("activate", "-", "latched=1 traced=", atomic_load(&gSCTraced));
        return SCP_LATCHED;
    }
    return SCP_OK;
}

// ---- START 1-3 (after ARMED; nobody waits for it): the sentinel goes live ----
// SWAP's @catch copies the exception name here; START 1 records it (the launch's one strike)
static char gActSwapExc[128];
static void SCStart(void) {
    if (gSCFd < 0) return;   // sentinel off, or events.log could not be opened
    SCEventRaw("activate", "-", "traced=", atomic_load(&gSCTraced));
    if (gActSwapExc[0] && ActStrikeOnce()) {
        char detail[160];
        snprintf(detail, sizeof detail, "exception=%s", gActSwapExc);
        SCEventRaw("crash", "activate", detail, -1);
    }
    atomic_store(&gSCOn, 1);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, SCWatchdog, NULL);
    atexit_b(^{
        SCEventRaw("exit", "-", "atexit", -1);
    });
    LOG(@"sentinel: activation %d build %s traced=%d scale=x%.0f events=%s (self %#lx-%#lx, %d related images)", gSCAct,
        gSCBuild, atomic_load(&gSCTraced), gSCScale, gSCPath, (unsigned long)gSCSelf.lo, (unsigned long)gSCSelf.hi,
        atomic_load(&gSCNRelated));
    // Conformance hook (conformance tests): SIMCAM_WD_TEST=
    //   hang        two photo ops that never end (1 s timeout) -> 2 hangs -> in-process latch; next activation latched
    //   pause       a photo op with a 3 s timeout whose work takes 2 s; stop the process from the host for >3 s
    //               meanwhile (kill -STOP <pid>; sleep 10; kill -CONT <pid>) -> no hang
    //   crash       a photo op, then a fault inside our image 1 s later -> "crash photo"; twice -> latched next
    //               activation
    //   crash-other abort() with no op inflight -> "crash-other", never counted
    const char *wdTest = getenv(ENV_WD_TEST);
    if (wdTest) {
        LOG(@"sentinel: test %s", wdTest);
        dispatch_queue_t q = dispatch_queue_create("simcam.wdtest", DISPATCH_QUEUE_SERIAL);
        if (!strcmp(wdTest, "hang")) {
            SCOpBegin("photo", 1.0);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), q, ^{
                SCOpBegin("photo", 1.0);
            });
        } else if (!strcmp(wdTest, "pause"))
            dispatch_async(q, ^{
                int op = SCOpBegin("photo", 3.0);
                LOG(@"sentinel: test pause op started, pid %d", getpid());
                sleep(2);
                SCOpEnd(op);
                LOG(@"sentinel: test pause op ended");
            });
        else if (!strcmp(wdTest, "crash"))
            dispatch_async(q, ^{
                SCOpBegin("photo", 30.0);
                sleep(1);
                TestFaultNullWrite(1);
            });
        else if (!strcmp(wdTest, "crash-other"))
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), q, ^{
                abort();
            });
    }
}
