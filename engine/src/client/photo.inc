// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): photo data fix and diagnostics.
//
// Provides: PhotoResolveDataFix, PhotoResolveShutter, PhotoResolveUnsupported, DiagResolveSessionHooks,
//           DiagResolveMetadataHooks, StopWaitResolveHooks (core/lazy.inc)
// Owns:     nothing shared.
// Uses:     core/act.inc, core/trace.inc, fig/session_state.inc (the photo stop wait)
#include "../core/activation.h"
#include "../core/trace.h"
#include "../fig/session.h"
#include "photo.h"

// ---- the shutter (the trace's shutter op, core/trace.inc) and, with SIMCAM_DIAG, the diagnostics: log (and rethrow)
// exceptions from capturePhotoWithSettings:delegate: ----
static IMP gOrigCapturePhoto;
static BOOL gDiagCapture;

static void hook_capturePhotoWithSettings(id self, SEL _cmd, id settings, id delegate) {
    TraceRecord(kTraceOpShutter, TraceSettingsID(settings), 0);
    if (!gDiagCapture) {
        ((void (*)(id, SEL, id, id))gOrigCapturePhoto)(self, _cmd, settings, delegate);
        return;
    }
    LOG(@"DIAG capturePhoto settings=%@ codecs=%@ fileTypes=%@ maxDims=%@ format=%@", settings,
        SafeKVC(self, @"availablePhotoCodecTypes"), SafeKVC(self, @"availablePhotoFileTypes"),
        SafeKVC(self, @"maxPhotoDimensions"),
        SafeKVC(settings, @"format"));
    @try {
        ((void (*)(id, SEL, id, id))gOrigCapturePhoto)(self, _cmd, settings, delegate);
    } @catch (NSException *exception) {
        LOG(@"DIAG capturePhoto EXCEPTION %@: %@", exception.name, exception.reason);
        @throw;
    }
}

// Diagnostics: where AVF handles the session's DidStopRunning and reports the stop. On 17.5 it runs on the
// notification thread, then on the stopRunning thread; the photo delegate callbacks run on the main queue.
static IMP gOrigDiagHandleDidStopRunning;
static IMP gOrigDiagSetRunning;

static void hook_diagHandleDidStopRunning(id self, SEL _cmd, id payload) {
    LOG(@"DIAG AVCaptureSession DidStopRunning handled on %s (main thread %d)",
        dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), [NSThread isMainThread]);
    ((void (*)(id, SEL, id))gOrigDiagHandleDidStopRunning)(self, _cmd, payload);
}

static void hook_diagSetRunning(id self, SEL _cmd, BOOL on) {
    ((void (*)(id, SEL, BOOL))gOrigDiagSetRunning)(self, _cmd, on);
    LOG(@"DIAG AVCaptureSession _setRunning:%d returned on %s (main thread %d)", on,
        dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), [NSThread isMainThread]);
}

static void DiagResolveSessionHooks(void) {   // resolve step (SIMCAM_DIAG only; optional)
    if (!getenv(ENV_DIAG)) {
        return;
    }
    ActivationHookResolve(kHookGroupClient, "diag didStop", objc_getClass("AVCaptureSession"),
                          "_handleDidStopRunningNotificationWithPayload:", NO, NO, (IMP)hook_diagHandleDidStopRunning,
                          &gOrigDiagHandleDidStopRunning, NO);
    ActivationHookResolve(kHookGroupClient, "diag setRunning", objc_getClass("AVCaptureSession"), "_setRunning:", NO,
                          NO, (IMP)hook_diagSetRunning, &gOrigDiagSetRunning, NO);
}

// ============================================================================
// Client-side photo fix. The simulator build of AVFCapture compiles out the compressed path of
// -[AVCapturePhoto fileDataRepresentation] (it returns nil when _isCompressedPhoto). The encoded bytes are in the
// photo surface (AVCapturePhotoInternal +0x20, the same ivar fileDataRepresentation null-checks), which our sink
// filled; the hooks return them. The same goes for -fileDataRepresentationWithCustomizer: and -CGImageRepresentation.
// ============================================================================
// AVCapturePhoto's first ivar, its AVCapturePhotoInternal pointer, right after the isa pointer
#define kPhotoInternalOffset 8
// AVCapturePhotoInternal's photo surface (the log line below spells this offset out as +0x20)
#define kPhotoInternalSurfaceOffset 0x20
static IMP gOrigFileDataRepresentation;
static IMP gOrigFileDataRepresentationWithCustomizer;
static IMP gOrigCGImageRepresentation;

static NSData *PhotoBytesFromSurface(id photo) {
    @try {
        void *internal = *(void **)((char *)(__bridge void *)photo + kPhotoInternalOffset);
        if (!internal) {
            return nil;
        }
        CFTypeRef surfaceRef = *(CFTypeRef *)((char *)internal + kPhotoInternalSurfaceOffset);
        if (!surfaceRef || CFGetTypeID(surfaceRef) != IOSurfaceGetTypeID()) {
            LOG(@"photo fix: +0x20 is not an IOSurface (%p)", surfaceRef);
            return nil;
        }
        IOSurfaceRef surface = (IOSurfaceRef)surfaceRef;
        // our byte surfaces are length x 1
        size_t length = IOSurfaceGetWidth(surface) * IOSurfaceGetHeight(surface) * IOSurfaceGetBytesPerElement(surface);
        if (length > IOSurfaceGetAllocSize(surface)) {
            length = IOSurfaceGetAllocSize(surface);
        }
        // a read-only lock of a surface nothing writes any more: it can only fail for an invalid surface
        (void)IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        NSData *bytes = [NSData dataWithBytes:IOSurfaceGetBaseAddress(surface) length:length];
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        return bytes;
    } @catch (NSException *exception) { return nil; }
}

static id hook_fileDataRepresentation(id self, SEL _cmd) {
    id data = ((id (*)(id, SEL))gOrigFileDataRepresentation)(self, _cmd);
    if (!data) {
        data = PhotoBytesFromSurface(self);
        LOG(@"photo fix: fileDataRepresentation -> %lu bytes from surface", (unsigned long)[data length]);
    }
    return data;
}

static id hook_fileDataRepresentationWithCustomizer(id self, SEL _cmd, id customizer) {
    id data = ((id (*)(id, SEL, id))gOrigFileDataRepresentationWithCustomizer)(self, _cmd, customizer);
    if (!data) {
        data = PhotoBytesFromSurface(self);
    }
    return data;
}

static CGImageRef hook_CGImageRepresentation(id self, SEL _cmd) {
    CGImageRef original = ((CGImageRef (*)(id, SEL))gOrigCGImageRepresentation)(self, _cmd);
    if (original) {
        return original;
    }
    NSData *data = PhotoBytesFromSurface(self);
    if (!data) {
        return NULL;
    }
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (!source) {
        return NULL;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    return (CGImageRef)CFAutorelease(image);
}

static BOOL PhotoResolveDataFix(void) {   // resolve step; the preflight checks all three selectors
    if (!PhotoDataFixOn()) {
        return YES;   // off switch: fileDataRepresentation stays the simulator's (nil)
    }
    Class photoClass = objc_getClass("AVCapturePhoto");
    return ActivationHookResolve(kHookGroupClient, "photo fix", photoClass, "fileDataRepresentation", NO, NO,
                                 (IMP)hook_fileDataRepresentation, &gOrigFileDataRepresentation, YES) &&
           ActivationHookResolve(kHookGroupClient, "photo fix", photoClass, "fileDataRepresentationWithCustomizer:", NO,
                                 NO, (IMP)hook_fileDataRepresentationWithCustomizer,
                                 &gOrigFileDataRepresentationWithCustomizer, YES) &&
           ActivationHookResolve(kHookGroupClient, "photo fix", photoClass, "CGImageRepresentation", NO, NO,
                                 (IMP)hook_CGImageRepresentation, &gOrigCGImageRepresentation, YES);
}

// resolve step: the shutter hook, for the trace (unless SIMCAM_TRACE=0) or SIMCAM_DIAG; optional
static void PhotoResolveShutter(void) {
    const char *trace = getenv(ENV_TRACE);
    gDiagCapture = getenv(ENV_DIAG) != NULL;
    BOOL traceOff = trace && trace[0] == '0';
    if (traceOff && !gDiagCapture) {
        return;
    }
    int hooksBefore = gActivation.hooks.count;
    ActivationHookResolve(kHookGroupClient, "trace shutter", objc_getClass("AVCapturePhotoOutput"),
                          "capturePhotoWithSettings:delegate:", NO, NO, (IMP)hook_capturePhotoWithSettings,
                          &gOrigCapturePhoto, NO);
    if (gActivation.hooks.count == hooksBefore) {
        LOG(@"MISMATCH: -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] not found; "
            @"the trace has no shutter op");
    }
}

// ---- diagnostics (SIMCAM_DIAG): metadata output ----
static IMP gOrigSetMetadataObjectTypes;
static IMP gOrigAvailableMetadataObjectTypes;
static IMP gOrigMachineReadableCodeInit;

static void hook_diagSetMetadataObjectTypes(id self, SEL _cmd, id types) {
    LOG(@"DIAG setMetadataObjectTypes %@ (available %@)", types,
        ((id (*)(id, SEL))gOrigAvailableMetadataObjectTypes)(self, sel_registerName("availableMetadataObjectTypes")));
    ((void (*)(id, SEL, id))gOrigSetMetadataObjectTypes)(self, _cmd, types);
}

static id hook_diagAvailableMetadataObjectTypes(id self, SEL _cmd) {
    return ((id (*)(id, SEL))gOrigAvailableMetadataObjectTypes)(self, _cmd);
}

static id hook_diagMachineReadableCodeInit(id self, SEL _cmd, id dict, id input) {
    id code = ((id (*)(id, SEL, id, id))gOrigMachineReadableCodeInit)(self, _cmd, dict, input);
    static int logged;
    if (logged++ < 3) {
        LOG(@"DIAG MRC object -> %@ string=%@", code, [code valueForKey:@"stringValue"]);
    }
    return code;
}

static void DiagResolveMetadataHooks(void) {   // resolve step (SIMCAM_DIAG only; optional)
    if (!getenv(ENV_DIAG)) {
        return;
    }
    Class metadataClass = objc_getClass("AVCaptureMetadataOutput");
    ActivationHookResolve(kHookGroupClient, "diag metadata", metadataClass, "availableMetadataObjectTypes", NO, NO,
                          (IMP)hook_diagAvailableMetadataObjectTypes, &gOrigAvailableMetadataObjectTypes, NO);
    ActivationHookResolve(kHookGroupClient, "diag metadata", metadataClass, "setMetadataObjectTypes:", NO, NO,
                          (IMP)hook_diagSetMetadataObjectTypes, &gOrigSetMetadataObjectTypes, NO);
    ActivationHookResolve(kHookGroupClient, "diag metadata", objc_getClass("AVMetadataMachineReadableCodeObject"),
                          "initWithFigEmbeddedCaptureDeviceMachineReadableCodeDictionary:input:", NO, NO,
                          (IMP)hook_diagMachineReadableCodeInit, &gOrigMachineReadableCodeInit, NO);
}

// ============================================================================
// Features stuntcam can't deliver read as unsupported, so an app that checks first never asks. AVF derives both
// itself rather than from the camera profile, so the profile can't turn them off.
// - bracket: AVF says 8 for any device with an active format, but the engine delivers one photo per request, so a
//   bracket never finished (a hang). After AVF's update the hook sets the internal count to 0 (KVO-notified), and
//   capturePhoto rejects a bracket instead. SIMCAM_OFF_BRACKET_UNSUPPORTED=1 brings the 8 back.
// - multicam: AVF builds 29 device sets from the simulator's answer and the device list, but no format is multi-cam,
//   so startRunning fails with -11873. The hook returns [] (+allSupportedMultiCamDeviceSets goes through it);
//   AVCaptureMultiCamSession.isMultiCamSupported is the simulator's own and stays. SIMCAM_OFF_MULTICAM_UNSUPPORTED=1
//   brings the 29 sets back.
// ============================================================================
static IMP gOrigUpdateMaxBracketedCount;
static Ivar gPhotoInternalIvar;

static void hook_updateMaxBracketedCount(id self, SEL _cmd, id device) {   // bracketed capture unsupported
    ((void (*)(id, SEL, id))gOrigUpdateMaxBracketedCount)(self, _cmd, device);
    id internal = object_getIvar(self, gPhotoInternalIvar);
    Ivar countIvar = internal ? class_getInstanceVariable(object_getClass(internal), "maxBracketedCapturePhotoCount")
                              : NULL;
    const char *type = countIvar ? ivar_getTypeEncoding(countIvar) : NULL;
    BOOL isWord = type && (type[0] == 'Q' || type[0] == 'q');   // an unsigned or signed 64-bit integer
    if (!isWord) {
        static int missingLogged;
        if (!missingLogged++) {
            LOG(@"MISMATCH: AVCapturePhotoOutputInternal.maxBracketedCapturePhotoCount not found (%s)", type ?: "-");
        }
        return;
    }

    // an aligned word: Apple's own write is a single store instruction
    NSUInteger *count = (NSUInteger *)((char *)(__bridge void *)internal + ivar_getOffset(countIvar));
    if (*count == 0) {
        return;
    }
    static int logged;
    if (logged++ < 3) {
        LOG(@"features: maxBracketedCapturePhotoCount %lu -> 0 (bracketed capture unsupported)",
            (unsigned long)*count);
    }
    [self willChangeValueForKey:@"maxBracketedCapturePhotoCount"];
    *count = 0;
    [self didChangeValueForKey:@"maxBracketedCapturePhotoCount"];
}

static id hook_supportedMultiCamDeviceSets(id self, SEL _cmd) { return @[]; }   // no multi-cam device sets

// resolve step: optional client hooks; a missing method only logs a MISMATCH
static void PhotoResolveUnsupported(void) {
    if (BracketUnsupportedOn()) {
        Class outputClass = objc_getClass("AVCapturePhotoOutput");
        gPhotoInternalIvar = outputClass ? class_getInstanceVariable(outputClass, "_internal") : NULL;
        int hooksBefore = gActivation.hooks.count;
        if (gPhotoInternalIvar) {
            ActivationHookResolve(kHookGroupClient, "bracket unsupported", outputClass,
                                  "_updateMaxBracketedCapturePhotoCountForSourceDevice:", NO, NO,
                                  (IMP)hook_updateMaxBracketedCount, &gOrigUpdateMaxBracketedCount, NO);
        }
        if (gActivation.hooks.count > hooksBefore) {
            LOG(@"features: bracketed capture reads unsupported (max count 0)");
        } else {
            LOG(@"MISMATCH: AVCapturePhotoOutput _internal / _updateMaxBracketedCapturePhotoCountForSourceDevice: "
                @"not found; bracket count stays Apple's");
        }
    }

    if (MultiCamUnsupportedOn()) {
        int hooksBefore = gActivation.hooks.count;
        ActivationHookResolve(kHookGroupClient, "multicam unsupported",
                              objc_getClass("AVCaptureDeviceDiscoverySession"), "supportedMultiCamDeviceSets", NO, NO,
                              (IMP)hook_supportedMultiCamDeviceSets, NULL, NO);
        if (gActivation.hooks.count > hooksBefore) {
            LOG(@"features: no multi-cam device sets");
        } else {
            LOG(@"MISMATCH: -[AVCaptureDeviceDiscoverySession supportedMultiCamDeviceSets] not found");
        }
    }
}

// ============================================================================
// Photo done (the stop wait in fig/session_state.inc): know when AVF has run a photo's didFinishCapture on its delegate
// queue, so sess_StopRunning can report the stop after it.
// - _handleDidFinishCaptureNotificationWithPayload:forRequest: dispatch_asyncs the callback to the delegate queue
//   (main). A block queued there after it runs once the callback has returned: the photo is then done.
//   Test faults: SIMCAM_TEST_FINISH_DELAY_MS=<ms> hands the callback over <ms> late, so AVF queues it after the stop
//   (deterministic). SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS=<ms> first queues a <ms> sleep on the delegate queue (the
//   condition of the race on 17.5; not deterministic).
// - _handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest: (a request's first callback)
//   marks that delegate queue, so a stopRunning from inside a photo callback doesn't wait for the callbacks queued
//   behind it.
// Both are optional: a missing method logs a MISMATCH, and with no done-hook no photo is tracked (the stop is reported
// at once).
// ============================================================================
static IMP gOrigHandleDidFinishCapture;
static IMP gOrigHandleWillBeginCapture;
static long gTestFinishDelayMs;
// A delegate-queue lookup that finds nil or something that isn't a dispatch queue logs a MISMATCH once, and the photo
// is marked done at once (the stop doesn't wait for it).
// Test faults: SIMCAM_TEST_NO_DELEGATE_QUEUE=1 makes the lookup find nothing (as if AVF renamed delegateStorage).
// SIMCAM_TEST_LOSE_PHOTO_DONE=1 never marks a photo done, so the stop's wait runs to its bound.
static BOOL gTestNoDelegateQueue;
static BOOL gTestLosePhotoDone;

static dispatch_queue_t StopWaitDelegateQueue(id request) {
    id queueObject = gTestNoDelegateQueue ? nil : SafeKVC(SafeKVC(request, @"delegateStorage"), @"delegateQueue");
    static Class queueClass;
    static Protocol *queueProtocol;
    static dispatch_once_t lookupOnce;
    dispatch_once(&lookupOnce, ^{
        queueClass = objc_getClass("OS_dispatch_queue");
        queueProtocol = objc_getProtocol("OS_dispatch_queue");
    });

    BOOL isQueue = NO;

    if (queueObject) {
        BOOL isQueueObject = queueClass && [queueObject isKindOfClass:queueClass];
        isQueue = isQueueObject || (queueProtocol && [queueObject conformsToProtocol:queueProtocol]);
    }

    if (!isQueue) {
        static _Atomic int mismatchLogged;
        if (!atomic_exchange(&mismatchLogged, 1)) {
            LOG(@"MISMATCH: photo stop: a photo request's delegateStorage.delegateQueue is %@; "
                @"a stop doesn't wait for its callbacks",
                queueObject ? NSStringFromClass([queueObject class]) : @"nil");
        }
        return nil;
    }

    dispatch_queue_t delegateQueue = (dispatch_queue_t)queueObject;
    int queueSeen = delegateQueue == dispatch_get_main_queue() ? kStopWaitSawMainDelegateQ : kStopWaitSawOtherDelegateQ;
    atomic_fetch_or(&gStopWait.delegateQueuesSeen, queueSeen);

    if (!dispatch_queue_get_specific(delegateQueue, &gStopWait.delegateQueueKey)) {
        dispatch_queue_set_specific(delegateQueue, &gStopWait.delegateQueueKey, &gStopWait.delegateQueueKey, NULL);
        LOG(@"photo stop: photo delegate queue %s marked (callbacks reach the app there)",
            dispatch_queue_get_label(delegateQueue));
    }

    return delegateQueue;
}

static long long StopWaitSettingsID(id payload, id request) {
    id fromPayload = [payload isKindOfClass:[NSDictionary class]] ? ((NSDictionary *)payload)[@"SettingsID"] : nil;
    return fromPayload ? [fromPayload longLongValue]
                       : [SafeKVC(SafeKVC(request, @"unresolvedSettings"), @"uniqueID") longLongValue];
}

static void hook_handleWillBeginCapture(id self, SEL _cmd, id payload, id request) {
    (void)StopWaitDelegateQueue(request);
    ((void (*)(id, SEL, id, id))gOrigHandleWillBeginCapture)(self, _cmd, payload, request);
}

static long gTestBusyDelegateQueueMs;

static void StopWaitDidFinishNow(id self, SEL _cmd, id payload, id request, long long settingsID,
                                 dispatch_queue_t delegateQueue) {
    ((void (*)(id, SEL, id, id))gOrigHandleDidFinishCapture)(self, _cmd, payload, request);
    if (gTestLosePhotoDone) {
        return;   // test fault: the done-mark is lost
    }
    if (delegateQueue) {
        dispatch_async(delegateQueue, ^{
            if (getenv(ENV_DIAG)) {
                LOG(@"DIAG photo stop: photo %lld didFinishCapture delivered (on %s)", settingsID,
                    dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL));
            }
            TraceRecord(kTraceOpDidFinish, settingsID, 0);
            StopWaitPhotoDone(settingsID);
        });
    } else {
        TraceRecord(kTraceOpDidFinish, settingsID, 0);
        StopWaitPhotoDone(settingsID);
    }
}

static void hook_handleDidFinishCapture(id self, SEL _cmd, id payload, id request) {
    long long settingsID = StopWaitSettingsID(payload, request);
    dispatch_queue_t delegateQueue = StopWaitDelegateQueue(request);
    {
        static _Atomic int logged;
        if (atomic_fetch_add(&logged, 1) < 2) {
            LOG(@"photo stop: didFinishCapture of photo %lld handled on %s, delegate queue %s", settingsID,
                dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL),
                delegateQueue ? dispatch_queue_get_label(delegateQueue) : "none");
        }
    }

    if (delegateQueue && gTestBusyDelegateQueueMs > 0) {   // test fault SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS
        long busyMs = gTestBusyDelegateQueueMs;
        LOG(@"SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS: delegate queue busy %ld ms before didFinishCapture of photo %lld",
            busyMs, settingsID);
        if (dispatch_get_specific(&gStopWait.delegateQueueKey)) {
            usleep((useconds_t)busyMs * 1000);
        } else {
            dispatch_async(delegateQueue, ^{
                usleep((useconds_t)busyMs * 1000);
            });
        }
    }

    if (gTestFinishDelayMs > 0) {   // test fault SIMCAM_TEST_FINISH_DELAY_MS
        static dispatch_queue_t faultQueue;
        static dispatch_once_t faultQueueOnce;
        dispatch_once(&faultQueueOnce, ^{
            faultQueue = dispatch_queue_create("simcam.testfault", DISPATCH_QUEUE_SERIAL);
        });
        LOG(@"SIMCAM_TEST_FINISH_DELAY_MS: didFinishCapture of photo %lld handed over %ld ms late", settingsID,
            gTestFinishDelayMs);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, gTestFinishDelayMs * NSEC_PER_MSEC), faultQueue, ^{
            StopWaitDidFinishNow(self, _cmd, payload, request, settingsID, delegateQueue);
        });
        return;
    }

    StopWaitDidFinishNow(self, _cmd, payload, request, settingsID, delegateQueue);
}

// the photo-done test faults, read once at the resolve step
static void StopWaitParseTestFaults(void) {
    gTestFinishDelayMs = TestFaultLong(ENV_TEST_FINISH_DELAY_MS);
    gTestBusyDelegateQueueMs = TestFaultLong(ENV_TEST_BUSY_DELEGATE_QUEUE_MS);
    gTestNoDelegateQueue = TestFaultOn(ENV_TEST_NO_DELEGATE_QUEUE);
    gTestLosePhotoDone = TestFaultOn(ENV_TEST_LOSE_PHOTO_DONE);
    if (gTestNoDelegateQueue || gTestLosePhotoDone) {
        LOG(@"test fault:%s%s", gTestNoDelegateQueue ? " SIMCAM_TEST_NO_DELEGATE_QUEUE" : "",
            gTestLosePhotoDone ? " SIMCAM_TEST_LOSE_PHOTO_DONE" : "");
    }
}

static void StopWaitResolveHooks(void) {   // resolve step: optional client hooks
    StopWaitParseTestFaults();
    Class outputClass = objc_getClass("AVCapturePhotoOutput");
    int hooksBefore = gActivation.hooks.count;
    ActivationHookResolve(kHookGroupClient, "photo done", outputClass,
                          "_handleDidFinishCaptureNotificationWithPayload:forRequest:", NO, NO,
                          (IMP)hook_handleDidFinishCapture, &gOrigHandleDidFinishCapture, NO);
    if (gActivation.hooks.count == hooksBefore) {
        LOG(@"MISMATCH: -[AVCapturePhotoOutput _handleDidFinishCaptureNotificationWithPayload:forRequest:] "
            @"not found; a stop doesn't wait for photo callbacks");
        return;
    }

    StopWaitInit();
    atomic_store(&gStopWait.hooked, 1);   // the state exists before any photo can be registered
    int hooksBeforeQueueHook = gActivation.hooks.count;
    ActivationHookResolve(kHookGroupClient, "photo queue", outputClass,
                          "_handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest:", NO, NO,
                          (IMP)hook_handleWillBeginCapture, &gOrigHandleWillBeginCapture, NO);
    if (gActivation.hooks.count == hooksBeforeQueueHook) {
        LOG(@"MISMATCH: -[AVCapturePhotoOutput "
            @"_handleWillBeginCaptureBeforeResolvingSettingsNotificationWithPayload:forRequest:] "
            @"not found; delegate queues are marked at didFinishCapture only");
    }
    LOG(@"photo stop: a stop waits for in-flight photos' didFinishCapture (≤ %.1f s)%s", kStopWaitBoundS,
        StopWaitOn() ? "" : " [off: SIMCAM_OFF_STOP_WAIT]");
}
