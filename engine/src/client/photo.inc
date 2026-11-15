// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): photo data fix and diagnostics.
// ---- the shutter (the trace's shutter op, core/trace.inc) and, with M5_DIAG, the diagnostics: log (and rethrow)
// exceptions from capturePhotoWithSettings:delegate: ----
static IMP gOrigCapPhoto;
static BOOL gM5DiagCap;
static void m5_capturePhoto(id self, SEL _cmd, id settings, id delegate) {
    TRRecord(TR_SHUTTER, TRSettingsID(settings), 0);
    if (!gM5DiagCap) { ((void (*)(id, SEL, id, id))gOrigCapPhoto)(self, _cmd, settings, delegate); return; }
    LOG(@"DIAG capturePhoto settings=%@ codecs=%@ fileTypes=%@ maxDims=%@ format=%@", settings, M5KVC(self, @"availablePhotoCodecTypes"), M5KVC(self, @"availablePhotoFileTypes"), M5KVC(self, @"maxPhotoDimensions"), M5KVC(settings, @"format"));
    @try { ((void (*)(id, SEL, id, id))gOrigCapPhoto)(self, _cmd, settings, delegate); }
    @catch (NSException *e) { LOG(@"DIAG capturePhoto EXCEPTION %@: %@", e.name, e.reason); @throw; }
}
// Diagnostics: where AVF handles the session's DidStopRunning and reports the stop (17.5: on the notification thread, then on the
// stopRunning thread; the photo delegate callbacks run on the main queue)
static IMP gOrigDiagDidStop, gOrigDiagSetRunning;
static void m5_diagDidStop(id self, SEL _cmd, id payload) {
    LOG(@"DIAG AVCaptureSession DidStopRunning handled on %s (main thread %d)", dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), [NSThread isMainThread]);
    ((void (*)(id, SEL, id))gOrigDiagDidStop)(self, _cmd, payload);
}
static void m5_diagSetRunning(id self, SEL _cmd, BOOL on) {
    ((void (*)(id, SEL, BOOL))gOrigDiagSetRunning)(self, _cmd, on);
    LOG(@"DIAG AVCaptureSession _setRunning:%d returned on %s (main thread %d)", on, dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), [NSThread isMainThread]);
}
static void m5diag(void) {   // PREPARE 4 (M5_DIAG only; optional)
    if (!getenv("M5_DIAG")) return;
    ActHookResolve(HG_CLIENT, "diag didStop", objc_getClass("AVCaptureSession"), "_handleDidStopRunningNotificationWithPayload:", NO, NO, (IMP)m5_diagDidStop, &gOrigDiagDidStop, NO);
    ActHookResolve(HG_CLIENT, "diag setRunning", objc_getClass("AVCaptureSession"), "_setRunning:", NO, NO, (IMP)m5_diagSetRunning, &gOrigDiagSetRunning, NO);
}

// ============================================================================
// Client-side photo fix. The simulator build of AVFCapture compiles out the compressed path of
// -[AVCapturePhoto fileDataRepresentation] (returns nil when _isCompressedPhoto). The encoded bytes are in the
// photo surface (AVCapturePhotoInternal +0x20, the same ivar fileDataRepresentation null-checks), which our
// sink filled; return them. Also -fileDataRepresentationWithCustomizer: and -CGImageRepresentation.
// ============================================================================
static IMP gOrigFDR, gOrigFDRC, gOrigCGI;
static NSData *M5PhotoBytes(id photo) {
    @try {
        void *internal = *(void **)((char *)(__bridge void *)photo + 8);
        if (!internal) return nil;
        CFTypeRef surf = *(CFTypeRef *)((char *)internal + 0x20);
        if (!surf || CFGetTypeID(surf) != IOSurfaceGetTypeID()) { LOG(@"photo fix: +0x20 is not an IOSurface (%p)", surf); return nil; }
        IOSurfaceRef s = (IOSurfaceRef)surf;
        size_t n = IOSurfaceGetWidth(s) * IOSurfaceGetHeight(s) * IOSurfaceGetBytesPerElement(s); // our byte surfaces are length x 1
        if (n > IOSurfaceGetAllocSize(s)) n = IOSurfaceGetAllocSize(s);
        IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
        NSData *d = [NSData dataWithBytes:IOSurfaceGetBaseAddress(s) length:n];
        IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
        return d;
    } @catch (NSException *e) { return nil; }
}
static id m5_fdr(id self, SEL _cmd) { id d = ((id (*)(id, SEL))gOrigFDR)(self, _cmd); if (!d) { d = M5PhotoBytes(self); LOG(@"photo fix: fileDataRepresentation -> %lu bytes from surface", (unsigned long)[d length]); } return d; }
static id m5_fdrc(id self, SEL _cmd, id c) { id d = ((id (*)(id, SEL, id))gOrigFDRC)(self, _cmd, c); if (!d) d = M5PhotoBytes(self); return d; }
static CGImageRef m5_cgi(id self, SEL _cmd) {
    CGImageRef r = ((CGImageRef (*)(id, SEL))gOrigCGI)(self, _cmd);
    if (r) return r;
    NSData *d = M5PhotoBytes(self); if (!d) return NULL;
    CGImageSourceRef is = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL); if (!is) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(is, 0, NULL); CFRelease(is);
    return (CGImageRef)CFAutorelease(img);
}
static BOOL m5photofix(void) {   // PREPARE 4; the preflight checks all three selectors
    if (getenv("M5_NO_PHOTOFIX")) return YES;
    Class P = objc_getClass("AVCapturePhoto");
    return ActHookResolve(HG_CLIENT, "photo fix", P, "fileDataRepresentation", NO, NO, (IMP)m5_fdr, &gOrigFDR, YES) &&
           ActHookResolve(HG_CLIENT, "photo fix", P, "fileDataRepresentationWithCustomizer:", NO, NO, (IMP)m5_fdrc, &gOrigFDRC, YES) &&
           ActHookResolve(HG_CLIENT, "photo fix", P, "CGImageRepresentation", NO, NO, (IMP)m5_cgi, &gOrigCGI, YES);
}

// PREPARE 4: the shutter hook, for the trace (unless SIMCAM_TRACE=0) or M5_DIAG; optional
static void m48shutter(void) {
    const char *t = getenv("SIMCAM_TRACE"); gM5DiagCap = getenv("M5_DIAG") != NULL;
    if ((t && t[0] == '0') && !gM5DiagCap) return;
    int n0 = gActNHooks;
    ActHookResolve(HG_CLIENT, "w4 e48 trace shutter", objc_getClass("AVCapturePhotoOutput"), "capturePhotoWithSettings:delegate:", NO, NO, (IMP)m5_capturePhoto, &gOrigCapPhoto, NO);
    if (gActNHooks == n0) LOG(@"MISMATCH: -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] not found; the trace has no shutter op");
}
// ---- diagnostics (M5_DIAG): metadata output ----
static IMP gOrigSetMOT, gOrigAvailMOT, gOrigMRCInit;
static double M7PreviewOfs(void) { return getenv("M7_PREVIEW_OFS") ? atof(getenv("M7_PREVIEW_OFS")) : getenv("M7_SRC_LEGACY") ? 90 : 270; }
static void m5_setMOT(id self, SEL c, id types) { LOG(@"DIAG setMetadataObjectTypes %@ (available %@)", types, ((id (*)(id, SEL))gOrigAvailMOT)(self, sel_registerName("availableMetadataObjectTypes"))); ((void (*)(id, SEL, id))gOrigSetMOT)(self, c, types); }
static id m5_availMOT(id self, SEL c) { return ((id (*)(id, SEL))gOrigAvailMOT)(self, c); }
static id m5_mrcInit(id self, SEL c, id dict, id input) { id r = ((id (*)(id, SEL, id, id))gOrigMRCInit)(self, c, dict, input); static int n; if (n++ < 3) LOG(@"DIAG MRC object -> %@ string=%@", r, [r valueForKey:@"stringValue"]); return r; }
static void m5diag2(void) {   // PREPARE 4 (M5_DIAG only; optional)
    if (!getenv("M5_DIAG")) return;
    Class M = objc_getClass("AVCaptureMetadataOutput");
    ActHookResolve(HG_CLIENT, "diag metadata", M, "availableMetadataObjectTypes", NO, NO, (IMP)m5_availMOT, &gOrigAvailMOT, NO);
    ActHookResolve(HG_CLIENT, "diag metadata", M, "setMetadataObjectTypes:", NO, NO, (IMP)m5_setMOT, &gOrigSetMOT, NO);
    ActHookResolve(HG_CLIENT, "diag metadata", objc_getClass("AVMetadataMachineReadableCodeObject"), "initWithFigEmbeddedCaptureDeviceMachineReadableCodeDictionary:input:", NO, NO, (IMP)m5_mrcInit, &gOrigMRCInit, NO);
}


// ============================================================================
// Features stuntcam can't deliver read as unsupported (design §6.5). Both readings below are derived by AVFCapture
// itself, not read from the camera profile, so the profile can't turn them off (flash could: cfg-m10.plist "Flash").
// - bracket: -[AVCapturePhotoOutput _updateMaxBracketedCapturePhotoCountForSourceDevice:] hard-codes 8 for any device
//   with an active format (0 only while Center Stage / background blur / Studio Light / background replacement is active).
//   The engine delivers one photo per request, so a 3-photo bracket gave 1 photo and never didFinishCapture (hang). After
//   Apple's update, the internal count is set to 0 (KVO-notified), so the getter reads 0 and capturePhoto rejects a bracket
//   ("exceeds -maxBracketedCapturePhotoCount") instead of hanging. M15_OLD_BRACKET=1 brings the 8 back.
// - multicam: -[AVCaptureDeviceDiscoverySession supportedMultiCamDeviceSets] builds its sets from the simulator's
//   gestalt answer (SupportsMultiCamCapture: YES) and the device list (every virtual device, and position/type
//   combinations), so stuntcam's cameras gave 29 sets; but no format is multi-cam and startRunning fails -11873. It now
//   returns [] (+allSupportedMultiCamDeviceSets goes through it). The class flag AVCaptureMultiCamSession.isMultiCamSupported
//   is the simulator's own (YES also unarmed) and stays. M15_OLD_MULTICAM=1 brings the 29 sets back.
// ============================================================================
static IMP gOrigUpdBracket;
static Ivar gPhotoInternalIvar;
static void m15_updBracket(id self, SEL _cmd, id device) {   // bracketed capture unsupported
    ((void (*)(id, SEL, id))gOrigUpdBracket)(self, _cmd, device);
    id internal = object_getIvar(self, gPhotoInternalIvar);
    Ivar iv = internal ? class_getInstanceVariable(object_getClass(internal), "maxBracketedCapturePhotoCount") : NULL;
    const char *t = iv ? ivar_getTypeEncoding(iv) : NULL;
    if (!t || (t[0] != 'Q' && t[0] != 'q')) { static int n; if (!n++) LOG(@"MISMATCH: AVCapturePhotoOutputInternal.maxBracketedCapturePhotoCount not found (%s)", t ?: "-"); return; }
    NSUInteger *p = (NSUInteger *)((char *)(__bridge void *)internal + ivar_getOffset(iv));   // aligned word: Apple's own write is one str
    if (*p == 0) return;
    static int logged; if (logged++ < 3) LOG(@"w4 e22: maxBracketedCapturePhotoCount %lu -> 0 (bracketed capture unsupported)", (unsigned long)*p);
    [self willChangeValueForKey:@"maxBracketedCapturePhotoCount"];
    *p = 0;
    [self didChangeValueForKey:@"maxBracketedCapturePhotoCount"];
}
static id m15_multiCamSets(id self, SEL _cmd) { return @[]; }   // no multi-cam device sets
static void m15features(void) {   // PREPARE 4: optional client hooks; a missing method only logs a MISMATCH
    if (!getenv("M15_OLD_BRACKET")) {
        Class P = objc_getClass("AVCapturePhotoOutput");
        gPhotoInternalIvar = P ? class_getInstanceVariable(P, "_internal") : NULL;
        int n0 = gActNHooks;
        if (gPhotoInternalIvar) ActHookResolve(HG_CLIENT, "w4 e22 bracket", P, "_updateMaxBracketedCapturePhotoCountForSourceDevice:", NO, NO, (IMP)m15_updBracket, &gOrigUpdBracket, NO);
        if (gActNHooks > n0) LOG(@"w4 e22: bracketed capture reads unsupported (max count 0)");
        else LOG(@"MISMATCH: AVCapturePhotoOutput _internal / _updateMaxBracketedCapturePhotoCountForSourceDevice: not found; bracket count stays Apple's");
    }
    if (!getenv("M15_OLD_MULTICAM")) {
        int n0 = gActNHooks;
        ActHookResolve(HG_CLIENT, "w4 e22 multicam", objc_getClass("AVCaptureDeviceDiscoverySession"), "supportedMultiCamDeviceSets", NO, NO, (IMP)m15_multiCamSets, NULL, NO);
        if (gActNHooks > n0) LOG(@"w4 e22: no multi-cam device sets");
        else LOG(@"MISMATCH: -[AVCaptureDeviceDiscoverySession supportedMultiCamDeviceSets] not found");
    }
}

// ============================================================================
// Photo done (ledger L66; the state and the why are in fig/session_state.inc): know when AVF has run a photo's
// didFinishCaptureForResolvedSettings: on its delegate queue, so sess_StopRunning can report the stop after it.
// - _handleDidFinishCaptureNotificationWithPayload:forRequest: hands the callback to the request's delegate storage, which
//   dispatch_asyncs it to its delegate queue (the main queue on 17.5, 26.3 and 27.0); a block queued there after it runs
//   once the callback has returned: the photo is done. AVF's own handling of the session's DidStopRunning (it wakes the
//   stopRunning thread) also runs on the main thread, but isn't queued in order with that dispatch_async: when main is busy
//   it can run first (17.5 with the main queue busy: 3 of 17 runs).
//   Test faults: M40_FINISH_DELAY_MS=<ms> hands the callback over <ms> late (from a private queue), so AVF queues it after
//   the stop: the deterministic form. M40_BUSY_DELEGATEQ_MS=<ms> first queues a <ms> sleep on the delegate queue, the
//   condition the 17.5 race needs (not deterministic).
// - _handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest: (a request's first callback) marks
//   that delegate queue, so a stopRunning from inside any photo callback is seen as one and not made to wait for callbacks
//   queued behind it.
// Both optional: a missing method logs a MISMATCH, and with no done-hook no photo is tracked (the stop is reported at once).
// ============================================================================
static IMP gOrigM40DidFinish, gOrigM40WillBegin;
static long gM40DelayMs;
// A lookup that finds no queue, or something that isn't one, logs a MISMATCH once: the photo
// is then marked done at once (the stop doesn't wait for it), which only the canary's positive control
// (M40_FINISH_DELAY_MS=300 alone must PASS, mutation.sh) would otherwise show.
// Test faults: M40_NO_DELEGATEQ=1 makes the lookup find nothing (as if AVF renamed delegateStorage): the MISMATCH line; the
// canary can't see it (the photo is then marked done right after AVF queues its callback, and the order breaks only with
// main busy). M40_LOSE_DONE=1 never marks a photo done: the stop's wait runs to its bound, which the canary's stop timing
// check flags (mutation.sh).
static BOOL gM40NoDelegateQ, gM40LoseDone;
static dispatch_queue_t M40DelegateQueue(id request) {
    id q = gM40NoDelegateQ ? nil : M5KVC(M5KVC(request, @"delegateStorage"), @"delegateQueue");
    static Class qc; static Protocol *qp; static dispatch_once_t o;
    dispatch_once(&o, ^{ qc = objc_getClass("OS_dispatch_queue"); qp = objc_getProtocol("OS_dispatch_queue"); });
    if (!q || !((qc && [q isKindOfClass:qc]) || (qp && [q conformsToProtocol:qp]))) {
        static _Atomic int once;
        if (!atomic_exchange(&once, 1)) LOG(@"MISMATCH: w4 e40: a photo request's delegateStorage.delegateQueue is %@; a stop doesn't wait for its callbacks", q ? NSStringFromClass([q class]) : @"nil");
        return nil;
    }
    dispatch_queue_t dq = (dispatch_queue_t)q;
    atomic_fetch_or(&gM40MainDelegateQ, dq == dispatch_get_main_queue() ? 1 : 2);
    if (!dispatch_queue_get_specific(dq, &gM40DelegateQKey)) {
        dispatch_queue_set_specific(dq, &gM40DelegateQKey, &gM40DelegateQKey, NULL);
        LOG(@"w4 e40: photo delegate queue %s marked (callbacks reach the app there)", dispatch_queue_get_label(dq));
    }
    return dq;
}
static long long M40SettingsID(id payload, id request) {
    id v = [payload isKindOfClass:[NSDictionary class]] ? ((NSDictionary *)payload)[@"SettingsID"] : nil;
    return v ? [v longLongValue] : [M5KVC(M5KVC(request, @"unresolvedSettings"), @"uniqueID") longLongValue];
}
static void m40_willBegin(id self, SEL _cmd, id payload, id request) {
    (void)M40DelegateQueue(request);
    ((void (*)(id, SEL, id, id))gOrigM40WillBegin)(self, _cmd, payload, request);
}
static long gM40BusyMs;
static void M40DidFinishNow(id self, SEL _cmd, id payload, id request, long long sid, dispatch_queue_t dq) {
    ((void (*)(id, SEL, id, id))gOrigM40DidFinish)(self, _cmd, payload, request);
    if (gM40LoseDone) return;   // test fault: the done-mark is lost
    if (dq) dispatch_async(dq, ^{ if (getenv("M5_DIAG")) LOG(@"DIAG w4 e40: photo %lld didFinishCapture delivered (on %s)", sid, dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL)); TRRecord(TR_DID_FINISH, sid, 0); M40PhotoDone(sid); });
    else { TRRecord(TR_DID_FINISH, sid, 0); M40PhotoDone(sid); }
}
static void m40_didFinish(id self, SEL _cmd, id payload, id request) {
    long long sid = M40SettingsID(payload, request);
    dispatch_queue_t dq = M40DelegateQueue(request);
    { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 2) LOG(@"w4 e40: didFinishCapture of photo %lld handled on %s, delegate queue %s", sid, dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), dq ? dispatch_queue_get_label(dq) : "none"); }
    if (dq && gM40BusyMs > 0) {
        long ms = gM40BusyMs; LOG(@"M40 test fault: delegate queue busy %ld ms before didFinishCapture of photo %lld", ms, sid);
        if (dispatch_get_specific(&gM40DelegateQKey)) usleep((useconds_t)ms * 1000);
        else dispatch_async(dq, ^{ usleep((useconds_t)ms * 1000); });
    }
    if (gM40DelayMs > 0) {
        static dispatch_queue_t fq; static dispatch_once_t o; dispatch_once(&o, ^{ fq = dispatch_queue_create("simcam.m40fault", DISPATCH_QUEUE_SERIAL); });
        LOG(@"M40 test fault: didFinishCapture of photo %lld handed over %ld ms late", sid, gM40DelayMs);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, gM40DelayMs * NSEC_PER_MSEC), fq, ^{ M40DidFinishNow(self, _cmd, payload, request, sid, dq); });
        return;
    }
    M40DidFinishNow(self, _cmd, payload, request, sid, dq);
}
static void m40photostop(void) {   // PREPARE 4: optional client hooks
    const char *d = getenv("M40_FINISH_DELAY_MS"); gM40DelayMs = d ? atol(d) : 0;
    d = getenv("M40_BUSY_DELEGATEQ_MS"); gM40BusyMs = d ? atol(d) : 0;
    gM40NoDelegateQ = getenv("M40_NO_DELEGATEQ") != NULL; gM40LoseDone = getenv("M40_LOSE_DONE") != NULL;
    if (gM40NoDelegateQ || gM40LoseDone) LOG(@"M40 test fault:%s%s", gM40NoDelegateQ ? " M40_NO_DELEGATEQ" : "", gM40LoseDone ? " M40_LOSE_DONE" : "");
    Class P = objc_getClass("AVCapturePhotoOutput");
    int n0 = gActNHooks;
    ActHookResolve(HG_CLIENT, "w4 e40 photo done", P, "_handleDidFinishCaptureNotificationWithPayload:forRequest:", NO, NO, (IMP)m40_didFinish, &gOrigM40DidFinish, NO);
    if (gActNHooks == n0) { LOG(@"MISMATCH: -[AVCapturePhotoOutput _handleDidFinishCaptureNotificationWithPayload:forRequest:] not found; a stop doesn't wait for photo callbacks"); return; }
    M40Init(); atomic_store(&gM40Hooked, 1);   // the state exists before any photo can be registered
    int n1 = gActNHooks;
    ActHookResolve(HG_CLIENT, "w4 e40 photo queue", P, "_handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest:", NO, NO, (IMP)m40_willBegin, &gOrigM40WillBegin, NO);
    if (gActNHooks == n1) LOG(@"MISMATCH: -[AVCapturePhotoOutput _handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest:] not found; delegate queues are marked at didFinishCapture only");
    LOG(@"w4 e40: a stop waits for in-flight photos' didFinishCapture (≤ %.1f s)%s", kM40WaitS, getenv("M40_OLD_STOP") ? " [off: M40_OLD_STOP]" : "");
}
