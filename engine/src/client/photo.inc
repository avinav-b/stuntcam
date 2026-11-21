// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): photo data fix and diagnostics.
// ---- the shutter (the trace's shutter op, core/trace.inc) and, with SIMCAM_DIAG, the diagnostics: log (and rethrow)
// exceptions from capturePhotoWithSettings:delegate: ----
static IMP gOrigCapPhoto;
static BOOL gDiagCapture;
static void hook_capturePhotoWithSettings(id self, SEL _cmd, id settings, id delegate) {
    TRRecord(TR_SHUTTER, TRSettingsID(settings), 0);
    if (!gDiagCapture) {
        ((void (*)(id, SEL, id, id))gOrigCapPhoto)(self, _cmd, settings, delegate);
        return;
    }
    LOG(@"DIAG capturePhoto settings=%@ codecs=%@ fileTypes=%@ maxDims=%@ format=%@", settings,
        SafeKVC(self, @"availablePhotoCodecTypes"), SafeKVC(self, @"availablePhotoFileTypes"),
        SafeKVC(self, @"maxPhotoDimensions"),
        SafeKVC(settings, @"format"));
    @try {
        ((void (*)(id, SEL, id, id))gOrigCapPhoto)(self, _cmd, settings, delegate);
    } @catch (NSException *e) {
        LOG(@"DIAG capturePhoto EXCEPTION %@: %@", e.name, e.reason);
        @throw;
    }
}
// Diagnostics: where AVF handles the session's DidStopRunning and reports the stop (17.5: on the notification thread,
// then on the stopRunning thread; the photo delegate callbacks run on the main queue)
static IMP gOrigDiagDidStop, gOrigDiagSetRunning;
static void hook_diagHandleDidStopRunning(id self, SEL _cmd, id payload) {
    LOG(@"DIAG AVCaptureSession DidStopRunning handled on %s (main thread %d)",
        dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), [NSThread isMainThread]);
    ((void (*)(id, SEL, id))gOrigDiagDidStop)(self, _cmd, payload);
}
static void hook_diagSetRunning(id self, SEL _cmd, BOOL on) {
    ((void (*)(id, SEL, BOOL))gOrigDiagSetRunning)(self, _cmd, on);
    LOG(@"DIAG AVCaptureSession _setRunning:%d returned on %s (main thread %d)", on,
        dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), [NSThread isMainThread]);
}
static void DiagResolveSessionHooks(void) {   // PREPARE 4 (SIMCAM_DIAG only; optional)
    if (!getenv(ENV_DIAG)) return;
    ActHookResolve(HG_CLIENT, "diag didStop", objc_getClass("AVCaptureSession"),
                   "_handleDidStopRunningNotificationWithPayload:", NO, NO, (IMP)hook_diagHandleDidStopRunning,
                   &gOrigDiagDidStop, NO);
    ActHookResolve(HG_CLIENT, "diag setRunning", objc_getClass("AVCaptureSession"), "_setRunning:", NO, NO,
                   (IMP)hook_diagSetRunning, &gOrigDiagSetRunning, NO);
}

// ============================================================================
// Client-side photo fix. The simulator build of AVFCapture compiles out the compressed path of
// -[AVCapturePhoto fileDataRepresentation] (returns nil when _isCompressedPhoto). The encoded bytes are in the
// photo surface (AVCapturePhotoInternal +0x20, the same ivar fileDataRepresentation null-checks), which our
// sink filled; return them. Also -fileDataRepresentationWithCustomizer: and -CGImageRepresentation.
// ============================================================================
static IMP gOrigFDR, gOrigFDRC, gOrigCGI;
static NSData *PhotoBytesFromSurface(id photo) {
    @try {
        void *internal = *(void **)((char *)(__bridge void *)photo + 8);
        if (!internal) return nil;
        CFTypeRef surfRef = *(CFTypeRef *)((char *)internal + 0x20);
        if (!surfRef || CFGetTypeID(surfRef) != IOSurfaceGetTypeID()) {
            LOG(@"photo fix: +0x20 is not an IOSurface (%p)", surfRef);
            return nil;
        }
        IOSurfaceRef surf = (IOSurfaceRef)surfRef;
        // our byte surfaces are length x 1
        size_t n = IOSurfaceGetWidth(surf) * IOSurfaceGetHeight(surf) * IOSurfaceGetBytesPerElement(surf);
        if (n > IOSurfaceGetAllocSize(surf)) n = IOSurfaceGetAllocSize(surf);
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        NSData *d = [NSData dataWithBytes:IOSurfaceGetBaseAddress(surf) length:n];
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        return d;
    } @catch (NSException *e) { return nil; }
}
static id hook_fileDataRepresentation(id self, SEL _cmd) {
    id d = ((id (*)(id, SEL))gOrigFDR)(self, _cmd);
    if (!d) {
        d = PhotoBytesFromSurface(self);
        LOG(@"photo fix: fileDataRepresentation -> %lu bytes from surface", (unsigned long)[d length]);
    }
    return d;
}
static id hook_fileDataRepresentationWithCustomizer(id self, SEL _cmd, id c) {
    id d = ((id (*)(id, SEL, id))gOrigFDRC)(self, _cmd, c);
    if (!d) d = PhotoBytesFromSurface(self);
    return d;
}
static CGImageRef hook_CGImageRepresentation(id self, SEL _cmd) {
    CGImageRef r = ((CGImageRef (*)(id, SEL))gOrigCGI)(self, _cmd);
    if (r) return r;
    NSData *d = PhotoBytesFromSurface(self);
    if (!d) return NULL;
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    if (!source) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    return (CGImageRef)CFAutorelease(img);
}
static BOOL PhotoResolveDataFix(void) {   // PREPARE 4; the preflight checks all three selectors
    if (getenv(ENV_OFF_PHOTO_DATA)) return YES;   // off switch: fileDataRepresentation stays the simulator's (nil)
    Class P = objc_getClass("AVCapturePhoto");
    return ActHookResolve(HG_CLIENT, "photo fix", P, "fileDataRepresentation", NO, NO, (IMP)hook_fileDataRepresentation,
                          &gOrigFDR, YES) &&
           ActHookResolve(HG_CLIENT, "photo fix", P, "fileDataRepresentationWithCustomizer:", NO, NO,
                          (IMP)hook_fileDataRepresentationWithCustomizer, &gOrigFDRC, YES) &&
           ActHookResolve(HG_CLIENT, "photo fix", P, "CGImageRepresentation", NO, NO, (IMP)hook_CGImageRepresentation,
                          &gOrigCGI, YES);
}

// PREPARE 4: the shutter hook, for the trace (unless SIMCAM_TRACE=0) or SIMCAM_DIAG; optional
static void PhotoResolveShutter(void) {
    const char *trace = getenv(ENV_TRACE);
    gDiagCapture = getenv(ENV_DIAG) != NULL;
    if ((trace && trace[0] == '0') && !gDiagCapture) return;
    int n0 = gActNHooks;
    ActHookResolve(HG_CLIENT, "trace shutter", objc_getClass("AVCapturePhotoOutput"),
                   "capturePhotoWithSettings:delegate:", NO, NO, (IMP)hook_capturePhotoWithSettings, &gOrigCapPhoto,
                   NO);
    if (gActNHooks == n0)
        LOG(@"MISMATCH: -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] not found; "
            @"the trace has no shutter op");
}
// ---- diagnostics (SIMCAM_DIAG): metadata output ----
static IMP gOrigSetMOT, gOrigAvailMOT, gOrigMRCInit;
static void hook_diagSetMetadataObjectTypes(id self, SEL c, id types) {
    LOG(@"DIAG setMetadataObjectTypes %@ (available %@)", types,
        ((id (*)(id, SEL))gOrigAvailMOT)(self, sel_registerName("availableMetadataObjectTypes")));
    ((void (*)(id, SEL, id))gOrigSetMOT)(self, c, types);
}
static id hook_diagAvailableMetadataObjectTypes(id self, SEL c) { return ((id (*)(id, SEL))gOrigAvailMOT)(self, c); }
static id hook_diagMRCInit(id self, SEL c, id dict, id input) {
    id r = ((id (*)(id, SEL, id, id))gOrigMRCInit)(self, c, dict, input);
    static int n;
    if (n++ < 3) LOG(@"DIAG MRC object -> %@ string=%@", r, [r valueForKey:@"stringValue"]);
    return r;
}
static void DiagResolveMetadataHooks(void) {   // PREPARE 4 (SIMCAM_DIAG only; optional)
    if (!getenv(ENV_DIAG)) return;
    Class M = objc_getClass("AVCaptureMetadataOutput");
    ActHookResolve(HG_CLIENT, "diag metadata", M, "availableMetadataObjectTypes", NO, NO,
                   (IMP)hook_diagAvailableMetadataObjectTypes, &gOrigAvailMOT, NO);
    ActHookResolve(HG_CLIENT, "diag metadata", M, "setMetadataObjectTypes:", NO, NO,
                   (IMP)hook_diagSetMetadataObjectTypes, &gOrigSetMOT, NO);
    ActHookResolve(HG_CLIENT, "diag metadata", objc_getClass("AVMetadataMachineReadableCodeObject"),
                   "initWithFigEmbeddedCaptureDeviceMachineReadableCodeDictionary:input:", NO, NO,
                   (IMP)hook_diagMRCInit, &gOrigMRCInit, NO);
}


// ============================================================================
// Features stuntcam can't deliver read as unsupported (design §6.5). Both readings below are derived by AVFCapture
// itself, not read from the camera profile, so the profile can't turn them off (flash could: cfg-m10.plist "Flash").
// - bracket: -[AVCapturePhotoOutput _updateMaxBracketedCapturePhotoCountForSourceDevice:] hard-codes 8 for any device
//   with an active format (0 only while Center Stage / background blur / Studio Light / background replacement is
//   active). The engine delivers one photo per request, so a 3-photo bracket gave 1 photo and never didFinishCapture
//   (hang). After Apple's update, the internal count is set to 0 (KVO-notified), so the getter reads 0 and capturePhoto
//   rejects a bracket ("exceeds -maxBracketedCapturePhotoCount") instead of hanging. SIMCAM_OFF_BRACKET_UNSUPPORTED=1
//   brings the 8 back (ledger L53).
// - multicam: -[AVCaptureDeviceDiscoverySession supportedMultiCamDeviceSets] builds its sets from the simulator's
//   gestalt answer (SupportsMultiCamCapture: YES) and the device list (every virtual device, and position/type
//   combinations), so stuntcam's cameras give 29 sets; but no format is multi-cam and startRunning fails -11873. The
//   hook returns [] (+allSupportedMultiCamDeviceSets goes through it). The class flag
//   AVCaptureMultiCamSession.isMultiCamSupported is the simulator's own (YES also unarmed) and stays.
//   SIMCAM_OFF_MULTICAM_UNSUPPORTED=1 brings the 29 sets back (ledger L54).
// ============================================================================
static IMP gOrigUpdBracket;
static Ivar gPhotoInternalIvar;
static void hook_updateMaxBracketedCount(id self, SEL _cmd, id device) {   // bracketed capture unsupported
    ((void (*)(id, SEL, id))gOrigUpdBracket)(self, _cmd, device);
    id internal = object_getIvar(self, gPhotoInternalIvar);
    Ivar countIvar = internal ? class_getInstanceVariable(object_getClass(internal), "maxBracketedCapturePhotoCount")
                              : NULL;
    const char *type = countIvar ? ivar_getTypeEncoding(countIvar) : NULL;
    if (!type || (type[0] != 'Q' && type[0] != 'q')) {
        static int n;
        if (!n++)
            LOG(@"MISMATCH: AVCapturePhotoOutputInternal.maxBracketedCapturePhotoCount not found (%s)", type ?: "-");
        return;
    }
    // aligned word: Apple's own write is one str
    NSUInteger *count = (NSUInteger *)((char *)(__bridge void *)internal + ivar_getOffset(countIvar));
    if (*count == 0) return;
    static int logged;
    if (logged++ < 3)
        LOG(@"features: maxBracketedCapturePhotoCount %lu -> 0 (bracketed capture unsupported)",
            (unsigned long)*count);
    [self willChangeValueForKey:@"maxBracketedCapturePhotoCount"];
    *count = 0;
    [self didChangeValueForKey:@"maxBracketedCapturePhotoCount"];
}
static id hook_supportedMultiCamDeviceSets(id self, SEL _cmd) { return @[]; }   // no multi-cam device sets
static void PhotoResolveUnsupported(void) {   // PREPARE 4: optional client hooks; a missing method only logs a MISMATCH
    if (!getenv(ENV_OFF_BRACKET_UNSUPPORTED)) {
        Class P = objc_getClass("AVCapturePhotoOutput");
        gPhotoInternalIvar = P ? class_getInstanceVariable(P, "_internal") : NULL;
        int n0 = gActNHooks;
        if (gPhotoInternalIvar)
            ActHookResolve(HG_CLIENT, "bracket unsupported", P, "_updateMaxBracketedCapturePhotoCountForSourceDevice:",
                           NO, NO, (IMP)hook_updateMaxBracketedCount, &gOrigUpdBracket, NO);
        if (gActNHooks > n0)
            LOG(@"features: bracketed capture reads unsupported (max count 0)");
        else
            LOG(@"MISMATCH: AVCapturePhotoOutput _internal / _updateMaxBracketedCapturePhotoCountForSourceDevice: "
                @"not found; bracket count stays Apple's");
    }
    if (!getenv(ENV_OFF_MULTICAM_UNSUPPORTED)) {
        int n0 = gActNHooks;
        ActHookResolve(HG_CLIENT, "multicam unsupported", objc_getClass("AVCaptureDeviceDiscoverySession"),
                       "supportedMultiCamDeviceSets", NO, NO, (IMP)hook_supportedMultiCamDeviceSets, NULL, NO);
        if (gActNHooks > n0) LOG(@"features: no multi-cam device sets");
        else LOG(@"MISMATCH: -[AVCaptureDeviceDiscoverySession supportedMultiCamDeviceSets] not found");
    }
}

// ============================================================================
// Photo done (ledger L66; the state and the why are in fig/session_state.inc): know when AVF has run a photo's
// didFinishCaptureForResolvedSettings: on its delegate queue, so sess_StopRunning can report the stop after it.
// - _handleDidFinishCaptureNotificationWithPayload:forRequest: hands the callback to the request's delegate storage,
//   which dispatch_asyncs it to its delegate queue (the main queue on 17.5, 26.3 and 27.0); a block queued there after
//   it runs once the callback has returned: the photo is done. AVF's own handling of the session's DidStopRunning (it
//   wakes the stopRunning thread) also runs on the main thread, but isn't queued in order with that dispatch_async:
//   when main is busy it can run first (17.5 with the main queue busy: 3 of 17 runs).
//   Test faults: SIMCAM_TEST_FINISH_DELAY_MS=<ms> hands the callback over <ms> late (from a private queue), so AVF
//   queues it after the stop: the deterministic form. SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS=<ms> first queues a <ms> sleep
//   on the delegate queue, the condition the 17.5 race needs (not deterministic).
// - _handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest: (a request's first callback)
//   marks that delegate queue, so a stopRunning from inside any photo callback is seen as one and not made to wait for
//   callbacks queued behind it.
// Both optional: a missing method logs a MISMATCH, and with no done-hook no photo is tracked (the stop is reported at
// once).
// ============================================================================
static IMP gOrigHandleDidFinishCapture, gOrigHandleWillBeginCapture;
static long gTestFinishDelayMs;
// A lookup that finds no queue, or something that isn't one, logs a MISMATCH once: the photo
// is then marked done at once (the stop doesn't wait for it), which only the canary's positive control
// (SIMCAM_TEST_FINISH_DELAY_MS=300 alone must PASS, mutation.sh) would otherwise show.
// Test faults: SIMCAM_TEST_NO_DELEGATE_QUEUE=1 makes the lookup find nothing (as if AVF renamed delegateStorage): the
// MISMATCH line; the canary can't see it (the photo is then marked done right after AVF queues its callback, and the
// order breaks only with main busy). SIMCAM_TEST_LOSE_PHOTO_DONE=1 never marks a photo done: the stop's wait runs to
// its bound, which the canary's stop timing check flags (mutation.sh).
static BOOL gTestNoDelegateQueue, gTestLosePhotoDone;
static dispatch_queue_t StopWaitDelegateQueue(id request) {
    id q = gTestNoDelegateQueue ? nil : SafeKVC(SafeKVC(request, @"delegateStorage"), @"delegateQueue");
    static Class qc;
    static Protocol *qp;
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        qc = objc_getClass("OS_dispatch_queue");
        qp = objc_getProtocol("OS_dispatch_queue");
    });
    if (!q || !((qc && [q isKindOfClass:qc]) || (qp && [q conformsToProtocol:qp]))) {
        static _Atomic int once;
        if (!atomic_exchange(&once, 1))
            LOG(@"MISMATCH: photo stop: a photo request's delegateStorage.delegateQueue is %@; "
                @"a stop doesn't wait for its callbacks",
                q ? NSStringFromClass([q class]) : @"nil");
        return nil;
    }
    dispatch_queue_t dq = (dispatch_queue_t)q;
    atomic_fetch_or(&gStopWaitMainDelegateQ, dq == dispatch_get_main_queue() ? 1 : 2);
    if (!dispatch_queue_get_specific(dq, &gStopWaitDelegateQKey)) {
        dispatch_queue_set_specific(dq, &gStopWaitDelegateQKey, &gStopWaitDelegateQKey, NULL);
        LOG(@"photo stop: photo delegate queue %s marked (callbacks reach the app there)",
            dispatch_queue_get_label(dq));
    }
    return dq;
}
static long long StopWaitSettingsID(id payload, id request) {
    id v = [payload isKindOfClass:[NSDictionary class]] ? ((NSDictionary *)payload)[@"SettingsID"] : nil;
    return v ? [v longLongValue] : [SafeKVC(SafeKVC(request, @"unresolvedSettings"), @"uniqueID") longLongValue];
}
static void hook_handleWillBeginCapture(id self, SEL _cmd, id payload, id request) {
    (void)StopWaitDelegateQueue(request);
    ((void (*)(id, SEL, id, id))gOrigHandleWillBeginCapture)(self, _cmd, payload, request);
}
static long gTestBusyDelegateQueueMs;
static void StopWaitDidFinishNow(id self, SEL _cmd, id payload, id request, long long sid, dispatch_queue_t dq) {
    ((void (*)(id, SEL, id, id))gOrigHandleDidFinishCapture)(self, _cmd, payload, request);
    if (gTestLosePhotoDone) return;   // test fault: the done-mark is lost
    if (dq) {
        dispatch_async(dq, ^{
            if (getenv(ENV_DIAG))
                LOG(@"DIAG photo stop: photo %lld didFinishCapture delivered (on %s)", sid,
                    dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL));
            TRRecord(TR_DID_FINISH, sid, 0);
            StopWaitPhotoDone(sid);
        });
    } else {
        TRRecord(TR_DID_FINISH, sid, 0);
        StopWaitPhotoDone(sid);
    }
}
static void hook_handleDidFinishCapture(id self, SEL _cmd, id payload, id request) {
    long long sid = StopWaitSettingsID(payload, request);
    dispatch_queue_t dq = StopWaitDelegateQueue(request);
    {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 2)
            LOG(@"photo stop: didFinishCapture of photo %lld handled on %s, delegate queue %s", sid,
                dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), dq ? dispatch_queue_get_label(dq) : "none");
    }
    if (dq && gTestBusyDelegateQueueMs > 0) {   // test fault SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS
        long ms = gTestBusyDelegateQueueMs;
        LOG(@"SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS: delegate queue busy %ld ms before didFinishCapture of photo %lld", ms,
            sid);
        if (dispatch_get_specific(&gStopWaitDelegateQKey)) usleep((useconds_t)ms * 1000);
        else dispatch_async(dq, ^{ usleep((useconds_t)ms * 1000); });
    }
    if (gTestFinishDelayMs > 0) {   // test fault SIMCAM_TEST_FINISH_DELAY_MS
        static dispatch_queue_t fq;
        static dispatch_once_t o;
        dispatch_once(&o, ^{ fq = dispatch_queue_create("simcam.testfault", DISPATCH_QUEUE_SERIAL); });
        LOG(@"SIMCAM_TEST_FINISH_DELAY_MS: didFinishCapture of photo %lld handed over %ld ms late", sid,
            gTestFinishDelayMs);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, gTestFinishDelayMs * NSEC_PER_MSEC), fq, ^{
            StopWaitDidFinishNow(self, _cmd, payload, request, sid, dq);
        });
        return;
    }
    StopWaitDidFinishNow(self, _cmd, payload, request, sid, dq);
}
static void StopWaitResolveHooks(void) {   // PREPARE 4: optional client hooks
    const char *d = getenv(ENV_TEST_FINISH_DELAY_MS);
    gTestFinishDelayMs = d ? atol(d) : 0;
    d = getenv(ENV_TEST_BUSY_DELEGATE_QUEUE_MS);
    gTestBusyDelegateQueueMs = d ? atol(d) : 0;
    gTestNoDelegateQueue = getenv(ENV_TEST_NO_DELEGATE_QUEUE) != NULL;
    gTestLosePhotoDone = getenv(ENV_TEST_LOSE_PHOTO_DONE) != NULL;
    if (gTestNoDelegateQueue || gTestLosePhotoDone)
        LOG(@"test fault:%s%s", gTestNoDelegateQueue ? " SIMCAM_TEST_NO_DELEGATE_QUEUE" : "",
            gTestLosePhotoDone ? " SIMCAM_TEST_LOSE_PHOTO_DONE" : "");
    Class P = objc_getClass("AVCapturePhotoOutput");
    int n0 = gActNHooks;
    ActHookResolve(HG_CLIENT, "photo done", P, "_handleDidFinishCaptureNotificationWithPayload:forRequest:", NO,
                   NO, (IMP)hook_handleDidFinishCapture, &gOrigHandleDidFinishCapture, NO);
    if (gActNHooks == n0) {
        LOG(@"MISMATCH: -[AVCapturePhotoOutput _handleDidFinishCaptureNotificationWithPayload:forRequest:] "
            @"not found; a stop doesn't wait for photo callbacks");
        return;
    }
    StopWaitInit();
    atomic_store(&gStopWaitHooked, 1);   // the state exists before any photo can be registered
    int n1 = gActNHooks;
    ActHookResolve(HG_CLIENT, "photo queue", P,
                   "_handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest:", NO, NO,
                   (IMP)hook_handleWillBeginCapture, &gOrigHandleWillBeginCapture, NO);
    if (gActNHooks == n1)
        LOG(@"MISMATCH: -[AVCapturePhotoOutput "
            @"_handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest:] "
            @"not found; delegate queues are marked at didFinishCapture only");
    LOG(@"photo stop: a stop waits for in-flight photos' didFinishCapture (≤ %.1f s)%s", kStopWaitBoundS,
        getenv(ENV_OFF_STOP_WAIT) ? " [off: SIMCAM_OFF_STOP_WAIT]" : "");
}
