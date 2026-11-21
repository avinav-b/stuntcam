// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): Vision barcode revision pin.
// ============================================================================
// Vision barcode detection in the simulator. VNDetectBarcodesRequest revisions 3/4 (the defaults on iOS 16+/17+) are
// ML-model based and fail in the sim with "Could not create inference context". Revisions 1/2 are the classic detector.
// Pin new requests to revision 1 (SIMCAM_VISION_REVISION=<n> overrides, SIMCAM_OFF_VISION_REVISION_PIN=1 disables). Not
// camera-specific.
// ============================================================================
static IMP gOrigVNBInit, gOrigVNBInitCH;
static __thread int gInPin;
static void VisionPinRevision(id req) {
    if (gInPin) return;   // setRevision: instantiates probe requests through the same initializer
    gInPin = 1;
    long want = getenv(ENV_VISION_REVISION) ? atol(getenv(ENV_VISION_REVISION)) : 1;
    long cur = ((long (*)(id, SEL))objc_msgSend)(req, sel_registerName("revision"));
    if (cur > want) {
        BOOL ok = YES;
        @try {
            ((void (*)(id, SEL, long))objc_msgSend)(req, sel_registerName("setRevision:"), want);
        } @catch (NSException *x) {
            ok = NO;
            LOG(@"vision setRevision exc %@", x.reason);
        }
        static int n;
        if (n++ < 2)
            LOG(@"VNDetectBarcodesRequest revision %ld -> %ld (%@)", cur, want, ok ? @"ok" : @"setRevision: raised");
    }
    gInPin = 0;
}
static id hook_barcodeRequestInit(id self, SEL c) {
    id r = ((id (*)(id, SEL))gOrigVNBInit)(self, c);
    if (r) VisionPinRevision(r);
    return r;
}
static id hook_barcodeRequestInitWithHandler(id self, SEL c, id h) {
    id r = ((id (*)(id, SEL, id))gOrigVNBInitCH)(self, c, h);
    if (r) VisionPinRevision(r);
    return r;
}
// Installed exactly once, by whichever comes first: SWAP (when Vision was already loaded at PREPARE 4), the dyld
// add-image callback (registered in START 5 otherwise), or the pump queue (providers/barcodes.inc). gVisionState is the
// once: a second install would record our own hook as the "original" and recurse.
static _Atomic int gVisionState;   // 0 not installed, 1 installed (or being installed)
static BOOL gVisionInSwap, gVisionNeedsStart;
static void VisionInstallOnce(void);
static void VisionInstall(void) {
    if (!objc_getClass("VNDetectBarcodesRequest")) return;
    int notInstalled = 0;
    if (atomic_compare_exchange_strong(&gVisionState, &notInstalled, 1)) VisionInstallOnce();
}
static void VisionInstallOnce(void) {
    Class C = objc_getClass("VNDetectBarcodesRequest");
    if (!C) return;
    // Hook on VNDetectBarcodesRequest itself: if it only inherits an initializer from VNRequest, add an override on
    // this class instead of swizzling VNRequest's, which would pin the revision of every Vision request the app makes.
    Method m;
    SEL selInitCH = sel_registerName("initWithCompletionHandler:"), selInit = sel_registerName("init");
    if ((m = class_getInstanceMethod(C, selInitCH))) {
        gOrigVNBInitCH = method_getImplementation(m);
        if (!class_addMethod(C, selInitCH, (IMP)hook_barcodeRequestInitWithHandler, method_getTypeEncoding(m)))
            method_setImplementation(m, (IMP)hook_barcodeRequestInitWithHandler);
    }
    // -init on VNRequest calls initWithCompletionHandler:nil, but hook it too in case it does not
    if ((m = class_getInstanceMethod(C, selInit)) && method_getImplementation(m) != (IMP)hook_barcodeRequestInit) {
        gOrigVNBInit = method_getImplementation(m);
        if (!class_addMethod(C, selInit, (IMP)hook_barcodeRequestInit, method_getTypeEncoding(m)))
            method_setImplementation(m, (IMP)hook_barcodeRequestInit);
    }
    LOG(@"vision fix installed (VNDetectBarcodesRequest -> rev %s)", getenv(ENV_VISION_REVISION) ?: "1");
}
static void VisionImageAdded(const struct mach_header *mh, intptr_t slide) {
    if (!objc_getClass("VNDetectBarcodesRequest")) return;
    VisionInstall();
}
// PREPARE 4: Vision already loaded -> its hooks go in at SWAP; otherwise START registers the add-image hook
static void VisionResolveFix(void) {
    if (getenv(ENV_OFF_VISION_REVISION_PIN)) return;   // no barcode revision pin
    Class C = objc_getClass("VNDetectBarcodesRequest");
    if (!C) {
        gVisionNeedsStart = YES;
        return;
    }
    // optional hooks: SWAP installs (and START reports) only what resolved; with none resolved gVisionState stays 0, so
    // the pump queue's VisionInstall can still try later
    int n0 = gActNHooks;
    ActHookResolve(HG_CLIENT, "vision fix", C, "initWithCompletionHandler:", NO, YES,
                   (IMP)hook_barcodeRequestInitWithHandler, &gOrigVNBInitCH, NO);
    ActHookResolve(HG_CLIENT, "vision fix", C, "init", NO, YES, (IMP)hook_barcodeRequestInit, &gOrigVNBInit, NO);
    gVisionInSwap = gActNHooks > n0;
    if (!gVisionInSwap)
        LOG(@"vision fix: VNDetectBarcodesRequest has neither init method at prepare; not installed at SWAP");
}
static void VisionStart(void) {   // START 5
    // Vision may be loaded later (e.g. dlopen by a plugin)
    if (gVisionNeedsStart) _dyld_register_func_for_add_image(VisionImageAdded);
    else if (gVisionInSwap)
        LOG(@"vision fix installed (VNDetectBarcodesRequest -> rev %s; at SWAP)", getenv(ENV_VISION_REVISION) ?: "1");
}
