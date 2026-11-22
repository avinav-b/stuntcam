// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): Vision barcode revision pin.
//
// Provides: VisionInstall (providers/barcodes.inc); VisionResolveFix, VisionStart (core/lazy.inc)
// Owns:     gVisionState, gVisionInSwap: installed yet, and whether the hooks go in at SWAP (core/lazy.inc), defined
//           in client/vision.h
// Uses:     core/act.inc
#include "vision.h"
#include "../core/activation.h"

// ============================================================================
// Vision barcode detection in the simulator. VNDetectBarcodesRequest revisions 3 and 4 (the defaults on iOS 16+ and
// 17+) are ML-model based and fail in the simulator with "Could not create inference context". Revisions 1 and 2 are
// the classic detector. New requests are pinned to revision 1 (SIMCAM_VISION_REVISION=<n> picks another,
// SIMCAM_OFF_VISION_REVISION_PIN=1 turns the pin off). Not camera-specific.
// ============================================================================
#define kVisionPinnedRevision 1   // the default; the log lines below spell it out as "1"
static IMP gOrigBarcodeRequestInit;
static IMP gOrigBarcodeRequestInitWithHandler;
static __thread int gInRevisionPin;

static void VisionPinRevision(id request) {
    if (gInRevisionPin) {
        return;   // setRevision: creates probe requests through the same initializer
    }
    gInRevisionPin = 1;
    long wanted = getenv(ENV_VISION_REVISION) ? atol(getenv(ENV_VISION_REVISION)) : kVisionPinnedRevision;
    long current = ((long (*)(id, SEL))objc_msgSend)(request, sel_registerName("revision"));
    if (current > wanted) {
        BOOL ok = YES;
        @try {
            ((void (*)(id, SEL, long))objc_msgSend)(request, sel_registerName("setRevision:"), wanted);
        } @catch (NSException *exception) {
            ok = NO;
            LOG(@"vision setRevision exc %@", exception.reason);
        }
        static int logged;
        if (logged++ < 2) {
            LOG(@"VNDetectBarcodesRequest revision %ld -> %ld (%@)", current, wanted,
                ok ? @"ok" : @"setRevision: raised");
        }
    }
    gInRevisionPin = 0;
}

static id hook_barcodeRequestInit(id self, SEL _cmd) {
    id request = ((id (*)(id, SEL))gOrigBarcodeRequestInit)(self, _cmd);
    if (request) {
        VisionPinRevision(request);
    }
    return request;
}

static id hook_barcodeRequestInitWithHandler(id self, SEL _cmd, id handler) {
    id request = ((id (*)(id, SEL, id))gOrigBarcodeRequestInitWithHandler)(self, _cmd, handler);
    if (request) {
        VisionPinRevision(request);
    }
    return request;
}

// The hooks go in exactly once, by whichever comes first: SWAP (when Vision was already loaded at the resolve step),
// the dyld add-image callback (which VisionStart registers otherwise), or the pump queue (providers/barcodes.inc).
// gVisionState (client/vision.h) makes it once: a second install would record our own hook as the "original" and
// recurse.
static BOOL gVisionNeedsStart;
static void VisionInstallOnce(void);

static void VisionInstall(void) {
    if (!objc_getClass("VNDetectBarcodesRequest")) {
        return;
    }
    int notInstalled = 0;
    if (atomic_compare_exchange_strong(&gVisionState, &notInstalled, 1)) {
        VisionInstallOnce();
    }
}

static void VisionInstallOnce(void) {
    Class requestClass = objc_getClass("VNDetectBarcodesRequest");
    if (!requestClass) {
        return;
    }

    // Hook on VNDetectBarcodesRequest itself: if it only inherits an initializer from VNRequest, add an override on
    // this class instead of swizzling VNRequest's, which would pin the revision of every Vision request the app makes.
    SEL selInitWithHandler = sel_registerName("initWithCompletionHandler:");
    SEL selInit = sel_registerName("init");
    Method method = class_getInstanceMethod(requestClass, selInitWithHandler);

    if (method) {
        gOrigBarcodeRequestInitWithHandler = method_getImplementation(method);
        if (!class_addMethod(requestClass, selInitWithHandler, (IMP)hook_barcodeRequestInitWithHandler,
                             method_getTypeEncoding(method))) {
            method_setImplementation(method, (IMP)hook_barcodeRequestInitWithHandler);
        }
    }

    // -init on VNRequest calls initWithCompletionHandler:nil, but hook it too in case it does not
    method = class_getInstanceMethod(requestClass, selInit);
    BOOL initHooked = method && method_getImplementation(method) == (IMP)hook_barcodeRequestInit;

    if (method && !initHooked) {
        gOrigBarcodeRequestInit = method_getImplementation(method);
        if (!class_addMethod(requestClass, selInit, (IMP)hook_barcodeRequestInit, method_getTypeEncoding(method))) {
            method_setImplementation(method, (IMP)hook_barcodeRequestInit);
        }
    }

    LOG(@"vision fix installed (VNDetectBarcodesRequest -> rev %s)", getenv(ENV_VISION_REVISION) ?: "1");
}

static void VisionImageAdded(const struct mach_header *header, intptr_t slide) {
    if (!objc_getClass("VNDetectBarcodesRequest")) {
        return;
    }
    VisionInstall();
}

// resolve step: if Vision is already loaded, its hooks go in at SWAP; otherwise START registers the add-image callback
static void VisionResolveFix(void) {
    if (!VisionRevisionPinOn()) {
        return;   // no barcode revision pin
    }
    Class requestClass = objc_getClass("VNDetectBarcodesRequest");
    if (!requestClass) {
        gVisionNeedsStart = YES;
        return;
    }

    // Optional hooks: SWAP installs (and START reports) only what resolved. With none resolved gVisionState stays 0,
    // so the pump queue's VisionInstall can still try later.
    int hooksBefore = gActivation.hooks.count;
    ActivationHookResolve(kHookGroupClient, "vision fix", requestClass, "initWithCompletionHandler:", NO, YES,
                          (IMP)hook_barcodeRequestInitWithHandler, &gOrigBarcodeRequestInitWithHandler, NO);
    ActivationHookResolve(kHookGroupClient, "vision fix", requestClass, "init", NO, YES, (IMP)hook_barcodeRequestInit,
                          &gOrigBarcodeRequestInit, NO);
    gVisionInSwap = gActivation.hooks.count > hooksBefore;
    if (!gVisionInSwap) {
        LOG(@"vision fix: VNDetectBarcodesRequest has neither init method at prepare; not installed at SWAP");
    }
}

static void VisionStart(void) {   // START, after the control timer
    // Vision may be loaded later (for example by a plugin's dlopen)
    if (gVisionNeedsStart) {
        _dyld_register_func_for_add_image(VisionImageAdded);
    } else if (gVisionInSwap) {
        LOG(@"vision fix installed (VNDetectBarcodesRequest -> rev %s; at SWAP)", getenv(ENV_VISION_REVISION) ?: "1");
    }
}
