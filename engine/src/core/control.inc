// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): host control file, per-launch env, interruptions (was m12_ctl.inc).
// ============================================================================
// m12: host control file + per-launch env, virtual device orientation, session interruptions.
//   SIMCAM_SOURCE=scene|image:<path>|video:<path>      per-launch source (default scene)
//   SIMCAM_ORIENT=portrait|upsideDown|landscapeLeft|landscapeRight|auto   per-launch virtual device orientation (UIDevice names)
//   SIMCAM_CONTROL=<host file>   polled every 250 ms; lines key=value:
//       source=...   orientation=...   event=<name>[@<seq>]  (interrupt[:reason] | end | runtime-error[:status] | server-died)
// Off switches: M12_NO_CONTROL (no polling), M12_NO_INTERRUPT (no lifecycle/audio interruptions; m10 behaviour), M12_NO_ORIENT.
// ============================================================================
#include <sys/stat.h>
static long M12ParseOrient(NSString *v) {
    v = v.lowercaseString;
    if ([v isEqualToString:@"portrait"] || [v isEqualToString:@"1"]) return 1;
    if ([v isEqualToString:@"upsidedown"] || [v isEqualToString:@"portraitupsidedown"] || [v isEqualToString:@"2"]) return 2;
    if ([v isEqualToString:@"landscapeleft"] || [v isEqualToString:@"3"]) return 3;    // UIDeviceOrientationLandscapeLeft: home button on the right
    if ([v isEqualToString:@"landscaperight"] || [v isEqualToString:@"4"]) return 4;
    return 0;  // auto
}
static long gOrientBeforeForce;   // main queue: the device orientation when forcing began; `auto` goes back to it
static void M12ApplyOrient(long o, const char *why) {
    // the rotation control itself works with M12_NO_ORIENT too (that switch only reverts the camera behaviour)
    long prev = atomic_exchange(&gDevOrientForced, o);
    LOG(@"m12 orientation -> %ld (%s)", o, why);
    dispatch_async(dispatch_get_main_queue(), ^{
        long ap = o;
        if (o && !prev) { long cur = UIDevice.currentDevice.orientation; gOrientBeforeForce = cur >= 1 && cur <= 4 ? cur : 1; }
        // auto after a forced orientation: rotate back to the device's own one (it only cleared the override, so a running
        // app stayed in the forced orientation until relaunched; w3 e19)
        if (!o && prev) {
            long cur = UIDevice.currentDevice.orientation;   // the Simulator rotated meanwhile (Hardware > Rotate): follow that
            ap = cur >= 1 && cur <= 4 && cur != prev ? cur : gOrientBeforeForce ? gOrientBeforeForce : 1;
            atomic_store(&gDevOrientSeen, ap);   // M12DevOrient falls back to it now; the motion timer would leave it stale for 0.5 s
            LOG(@"m12 orientation auto: back to %ld", ap);
        }
        if (ap) {
            @try { [UIDevice.currentDevice setValue:@(ap) forKey:@"orientation"]; } @catch (NSException *e) { LOG(@"m12 UIDevice orientation KVC threw %@", e.reason); }
            // the KVC set does not reach observers of the notification (Flutter's camera plugin); post it like backboardd would (M12_NO_ORIENTNOTE off)
            if (!getenv("M12_NO_ORIENTNOTE")) [NSNotificationCenter.defaultCenter postNotificationName:UIDeviceOrientationDidChangeNotification object:UIDevice.currentDevice];
            // rotate the UI like autorotation would, if the app allows that orientation (a portrait-locked app keeps its UI, like on a device)
            UIInterfaceOrientationMask mask = ap == 1 ? UIInterfaceOrientationMaskPortrait : ap == 2 ? UIInterfaceOrientationMaskPortraitUpsideDown :
                                              ap == 3 ? UIInterfaceOrientationMaskLandscapeRight : UIInterfaceOrientationMaskLandscapeLeft;
            for (UIScene *sc in UIApplication.sharedApplication.connectedScenes) {
                if (![sc isKindOfClass:[UIWindowScene class]]) continue;
                UIWindowScene *ws = (UIWindowScene *)sc;
                for (UIWindow *w in ws.windows) [w.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
                [ws requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask]
                                            errorHandler:^(NSError *e) { LOG(@"m12 UI rotation refused by the app: %@", e.localizedDescription); }];
            }
        }
        M5CoordOrientationChanged();
    });
}

// ---- interruptions ----
// AVF (-[AVCaptureSession _handleDidStopRunningNotificationWithPayload:], 26.3): DidStopRunning with ErrorStatus -16402 ->
// _setInterrupted:YES withReason:<ErrorReason> interruptor:<CameraStolenInterruptor>; DidStartRunning -> _setInterrupted:NO. Other statuses
// (except -16403..-16409 family / -15545) -> AVCaptureSessionRuntimeErrorNotification.
static const int kM12FigInterrupted = -16402;
static NSArray<M5Sess *> *M12AllSessions(void) { if (!gSessions) return @[]; @synchronized (gSessions) { return gSessions.allValues; } }
static void M12Interrupt(int reason, const char *why) {
    if (!gPumpQ) return;
    dispatch_async(gPumpQ, ^{
        for (M5Sess *S in M12AllSessions()) {
            if (!S.obj || !S.running || S.interrupted) continue;
            if (reason == 2 && !S.wantsMic) continue;   // audio interruptions only hit sessions with an audio input
            S.interrupted = YES; S.interruptReason = reason; S.running = NO;
            if (S.timer) { dispatch_source_cancel(S.timer); S.timer = nil; }
            M5UpdateMic(S);
            NSArray *recs; @synchronized (S.recs) { recs = S.recs.allKeys; }
            for (NSString *sid in recs) M12StopRec(S.obj, (__bridge CFStringRef)sid, -11818);   // AVErrorSessionWasInterrupted
            LOG(@"m12 interrupt session %p reason %d (%s), %lu recordings stopped", S.obj, reason, why, (unsigned long)recs.count);
            for (M5Sink *k in S.sinks) if ([k.kind isEqualToString:@"preview"]) M5SessPost(S.obj, @"DidStopPreviewing", @{@"SectionID": k.sinkID});
            if (!getenv("M13_NO_PHOTODRAIN")) dispatch_sync(M5PhotoQ(), ^{});   // in-flight photos complete before the interruption (see sess_StopRunning); photo blocks never wait on the pump queue. Holds the pump during the encode (TODO.md)
            M5SessPost(S.obj, @"DidStopRunning", @{@"ErrorStatus": @(kM12FigInterrupted), @"ErrorReason": @(reason)});
        }
    });
}
// Restarts only sessions still interrupted: the app's own stop or start during the interruption already ended it for that
// session (sess_StopRunning / sess_StartRunning). Both run on the pump queue, so either order with this one is consistent.
static void M12EndInterrupt(int reason, const char *why) {
    if (!gPumpQ) return;
    dispatch_async(gPumpQ, ^{
        for (M5Sess *S in M12AllSessions()) {
            if (!S.obj || !S.interrupted || (reason && S.interruptReason != reason)) continue;
            S.interrupted = NO; S.running = YES;
            long long cid = [M5KVC(S.config, @"configurationID") longLongValue];
            LOG(@"m12 interruption ended session %p (%s)", S.obj, why);
            for (M5Sink *k in S.sinks) if ([k.kind isEqualToString:@"preview"]) k.frames = 0;   // re-announce DidStartPreviewing
            M5SessPost(S.obj, @"DidStartRunning", @{@"ConfigurationID": @(cid)});
            M5UpdateMic(S); M5StartPump(S);
        }
    });
}
// w2: stop every active recording with status (sentinel movie latch, movie-error control event); the session keeps running.
// The session walk runs on the pump queue (as M12Interrupt's and M12RuntimeError's do): that is where finalize clears S.obj
// (M10ForgetSession), so a session can't be freed between reading S.obj and stopping its recording. (The app's own
// FileSinkStop calls M12StopRec on AVFoundation's thread with a session it holds.)
static void M12StopAllRecordings(int status) {
    if (!gPumpQ) return;
    dispatch_async(gPumpQ, ^{
        for (M5Sess *S in M12AllSessions()) { if (!S.obj) continue; NSArray *recs; @synchronized (S.recs) { recs = S.recs.allKeys; } for (NSString *sid in recs) M12StopRec(S.obj, (__bridge CFStringRef)sid, status); }
    });
}
static void M12RuntimeError(int status, BOOL serverDied) {
    if (!gPumpQ) return;
    dispatch_async(gPumpQ, ^{
        int st = status; BOOL decided = status != SC_LATCH_AUTO;
        for (M5Sess *S in M12AllSessions()) {
            if (!S.obj) continue;
            if (serverDied) { LOG(@"m12 inject ServerConnectionDied on session %p", S.obj); M5SessPost(S.obj, @"ServerConnectionDied", @{}); continue; }
            if (!S.running) continue;
            if (!decided) { st = SCSessionErrorAllowed() ? SC_LATCH_STATUS : 0; decided = YES; }   // only once a running session is found
            LOG(@"m12 %@ on session %p", st ? [NSString stringWithFormat:@"inject runtime error %d", st] : @"stop (session latched; no runtime error: one in the last 5 s)", S.obj);
            S.running = NO; if (S.timer) { dispatch_source_cancel(S.timer); S.timer = nil; } M5UpdateMic(S);
            NSArray *recs; @synchronized (S.recs) { recs = S.recs.allKeys; }
            for (NSString *sid in recs) M12StopRec(S.obj, (__bridge CFStringRef)sid, st);
            M5SessPost(S.obj, @"DidStopRunning", st ? @{@"ErrorStatus": @(st)} : nil);   // 0: a plain stop, no runtime error
        }
    });
}
static void M12Event(NSString *ev) {
    NSArray *p = [ev componentsSeparatedByString:@":"]; NSString *name = p[0]; int arg = p.count > 1 ? [p[1] intValue] : 0;
    if ([name isEqualToString:@"interrupt"]) M12Interrupt(arg ?: 3, "control file");   // default: videoDeviceInUseByAnotherClient
    else if ([name isEqualToString:@"end"]) M12EndInterrupt(0, "control file");
    else if ([name isEqualToString:@"runtime-error"]) M12RuntimeError(arg ?: -11819, NO);
    else if ([name isEqualToString:@"server-died"]) M12RuntimeError(0, YES);
    // w2 probes for mid-session error completion (design §5.4): the next photo / every active recording ends with this status
    else if ([name isEqualToString:@"photo-error"]) { atomic_store(&gPhotoErrNext, arg ?: -16800); LOG(@"m12 control: next photo fails with %d", arg ?: -16800); }
    else if ([name isEqualToString:@"movie-error"]) {
        int st = arg ?: -16800;
        M12StopAllRecordings(st);
        LOG(@"m12 control: stopping active recordings with %d", st);
    }
    else LOG(@"m12 control: unknown event %@", ev);
}

// ---- control file ----
static NSString *gCtlLastEvent; static struct timespec gCtlMtime; static BOOL gCtlFirstPoll = YES;
static void M12PollControl(const char *path) {
    struct stat st; if (stat(path, &st) != 0) { gCtlFirstPoll = NO; return; }
    if (st.st_mtimespec.tv_sec == gCtlMtime.tv_sec && st.st_mtimespec.tv_nsec == gCtlMtime.tv_nsec) return;
    gCtlMtime = st.st_mtimespec;
    NSString *txt = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil]; if (!txt) return;
    for (NSString *line0 in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *line = [line0 stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSRange eq = [line rangeOfString:@"="]; if (!line.length || [line hasPrefix:@"#"] || eq.location == NSNotFound) continue;
        NSString *k = [line substringToIndex:eq.location], *v = [line substringFromIndex:eq.location + 1];
        if ([k isEqualToString:@"source"]) M12SetSource(v, "control file");
        else if ([k isEqualToString:@"orientation"]) { long o = M12ParseOrient(v); if (o != atomic_load(&gDevOrientForced)) M12ApplyOrient(o, "control file"); }
        else if ([k isEqualToString:@"event"]) { if (![v isEqualToString:gCtlLastEvent]) { gCtlLastEvent = v; NSString *e = [v componentsSeparatedByString:@"@"][0];
            if (gCtlFirstPoll) LOG(@"m12 control: event %@ present at start, not replayed", v); else M12Event(e); } }
    }
    gCtlFirstPoll = NO;
}

static void m12_bg(CFNotificationCenterRef c, void *o, CFNotificationName n, const void *obj, CFDictionaryRef ui) {}
// ---- W4: m12init split into PREPARE 2 (control file read), PREPARE 4 (env/control-derived state), SWAP 1 (stores), SWAP 7
// (enqueues) and START 4 (control timer, observers). The first frame after ARMED already uses SIMCAM_SOURCE / SIMCAM_ORIENT and
// the control file's source= / orientation= (which override the env, as at today's first poll), even if START is late.
static struct { BOOL read; struct timespec mtime; void *source, *event; long orient; BOOL hasOrient; } gCtlRead;   // PREPARE 2
static void *gSrcSpecNext; static long gOrientNext; static const char *gOrientWhy; static int gPhotoErrInit; static BOOL gPhotoErrSet;
static void M12CtlRead(void) {   // PREPARE 2: one stat and parse, as M12PollControl does, applying nothing
    const char *ctl = getenv("SIMCAM_CONTROL");
    if (!ctl || getenv("M12_NO_CONTROL")) return;
    struct stat st; if (stat(ctl, &st) != 0) return;
    NSString *txt = [NSString stringWithContentsOfFile:@(ctl) encoding:NSUTF8StringEncoding error:nil];
    gCtlRead.read = YES; gCtlRead.mtime = st.st_mtimespec;
    for (NSString *line0 in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *line = [line0 stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSRange eq = [line rangeOfString:@"="]; if (!line.length || [line hasPrefix:@"#"] || eq.location == NSNotFound) continue;
        NSString *k = [line substringToIndex:eq.location], *v = [line substringFromIndex:eq.location + 1];
        if ([k isEqualToString:@"source"]) { if (gCtlRead.source) CFRelease(gCtlRead.source); gCtlRead.source = (void *)CFBridgingRetain([v copy]); }
        else if ([k isEqualToString:@"orientation"]) { gCtlRead.orient = M12ParseOrient(v); gCtlRead.hasOrient = YES; }
        else if ([k isEqualToString:@"event"]) {
            if (gCtlRead.event) CFRelease(gCtlRead.event);
            gCtlRead.event = (void *)CFBridgingRetain([v copy]);
            LOG(@"m12 control: event %@ present at start, not replayed", v);
        }
    }
    LOG(@"m12 control file %s read at activation (source=%@ orientation=%@)", ctl, gCtlRead.source ? (__bridge NSString *)gCtlRead.source : @"-", gCtlRead.hasOrient ? @(gCtlRead.orient) : @"-");
}
static void M12Resolve(void) {   // PREPARE 4: allocate, parse; objects SWAP stores are retained here and kept as void *
    gSrcSpecLock = [NSObject new];
    if (getenv("SIMCAM_TEST_PHOTO_ERR")) { gPhotoErrInit = atoi(getenv("SIMCAM_TEST_PHOTO_ERR")); gPhotoErrSet = YES; }   // w2 probe: first photo fails
    const char *src = getenv("SIMCAM_SOURCE");
    if (gCtlRead.source) { gSrcSpecNext = gCtlRead.source; gCtlRead.source = NULL; }
    else if (src) gSrcSpecNext = (void *)CFBridgingRetain(@(src));
    const char *ori = getenv("SIMCAM_ORIENT");
    if (gCtlRead.hasOrient) { gOrientNext = gCtlRead.orient; gOrientWhy = "control file"; }
    else if (ori) { gOrientNext = M12ParseOrient(@(ori)); gOrientWhy = "SIMCAM_ORIENT"; }
}
static void M12Swap(void) {   // SWAP 1: plain stores only (no lock, no log, no message): no session is adopted yet
    if (gSrcSpecNext) { *(void **)(void *)&gSrcSpecPending = gSrcSpecNext; gSrcSpecNext = NULL; atomic_fetch_add(&gSrcGen, 1); }   // what M12SetSource does
    if (gPhotoErrSet) atomic_store(&gPhotoErrNext, gPhotoErrInit);
    if (gOrientNext) atomic_store(&gDevOrientForced, gOrientNext);
    if (gCtlRead.read) { gCtlMtime = gCtlRead.mtime; if (gCtlRead.event) { *(void **)(void *)&gCtlLastEvent = gCtlRead.event; gCtlRead.event = NULL; } }
    gCtlFirstPoll = NO;   // START's first poll returns on the unchanged mtime; the next real event= is replayed
}
static void M12ApplyOrientF(void *ctx) { M12ApplyOrient(gOrientNext, gOrientWhy); }
static void M12QRWarmF(void *ctx) { @autoreleasepool { M12QRWarm(); } }
static void M12Enqueue(void) {   // SWAP 7: nothing waits on these
    if (gOrientNext) dispatch_async_f(dispatch_get_main_queue(), NULL, M12ApplyOrientF);
    if (M12Scene2On()) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL, M12QRWarmF);   // w2 e10b: build the scene QR off the first pump tick
}
static void M12Start(void) {   // START 4
    const char *ctl = getenv("SIMCAM_CONTROL");
    if (ctl && !getenv("M12_NO_CONTROL")) {
        char *path = strdup(ctl);
        dispatch_queue_t q = dispatch_queue_create("m12.control", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
        dispatch_source_set_timer(t, DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(t, ^{ @autoreleasepool { M12PollControl(path); } });
        dispatch_resume(t);
        static dispatch_source_t keep; keep = t; (void)keep;
        LOG(@"m12 control file %s (polled every 250 ms)", path);
    }
    if (!getenv("M12_NO_INTERRUPT")) {
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        // device: an app's capture session is interrupted (reason 1, VideoDeviceNotAvailableInBackground) when it leaves the foreground,
        // and resumes when it comes back
        [nc addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) { M12Interrupt(1, "app entered background"); }];
        [nc addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) { M12EndInterrupt(1, "app will enter foreground"); }];
        // audio session interruption (phone call, Siri, another app's non-mixable session): sessions with audio get reason 2
        [nc addObserverForName:@"AVAudioSessionInterruptionNotification" object:nil queue:nil usingBlock:^(NSNotification *n) {
            long type = [n.userInfo[@"AVAudioSessionInterruptionTypeKey"] longValue];
            LOG(@"m12 AVAudioSession interruption type %ld", type);
            if (type == 1) M12Interrupt(2, "audio session interruption began"); else M12EndInterrupt(2, "audio session interruption ended");
        }];
        LOG(@"m12 lifecycle interruptions armed");
    }
    if (gSrcSpecPending) LOG(@"m12 source -> %@ (at activation)", gSrcSpecPending);
    if (gOrientNext) LOG(@"m12 orientation %ld forced at activation (%s)", gOrientNext, gOrientWhy);
    (void)m12_bg;
}
