// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): Vision barcode revision pin.
// ============================================================================
// m7: Vision barcode detection in the simulator. VNDetectBarcodesRequest revisions 3/4 (the defaults on iOS 16+/17+) are
// ML-model based and fail in the sim with "Could not create inference context". Revisions 1/2 are the classic detector.
// Pin new requests to revision 1 (M7_VISION_REV=<n> overrides, M7_NO_VISIONFIX=1 disables). Not camera-specific.
// ============================================================================
static IMP gOrigVNBInit, gOrigVNBInitCH;
static __thread int gInPin;
static void M7PinRev(id req) {
    if (gInPin) return;   // setRevision: instantiates probe requests through the same initializer
    gInPin = 1;
    long want = getenv("M7_VISION_REV") ? atol(getenv("M7_VISION_REV")) : 1;
    long cur = ((long (*)(id, SEL))objc_msgSend)(req, sel_registerName("revision"));
    if (cur > want) {
        NSError *e = nil;
        BOOL ok = YES;
        @try { ((void (*)(id, SEL, long))objc_msgSend)(req, sel_registerName("setRevision:"), want); } @catch (NSException *x) { ok = NO; LOG(@"vision setRevision exc %@", x.reason); }
        static int n; if (n++ < 2) LOG(@"VNDetectBarcodesRequest revision %ld -> %ld (%@)", cur, want, ok ? @"ok" : e);
    }
    gInPin = 0;
}
static id m7_vnbInit(id self, SEL c) { id r = ((id (*)(id, SEL))gOrigVNBInit)(self, c); if (r) M7PinRev(r); return r; }
static id m7_vnbInitCH(id self, SEL c, id h) { id r = ((id (*)(id, SEL, id))gOrigVNBInitCH)(self, c, h); if (r) M7PinRev(r); return r; }
// W4: installed exactly once, by whichever comes first: SWAP (when Vision was already loaded at PREPARE 4), the dyld add-image
// callback (registered in START 5 otherwise), or the pump queue (scene.inc). gVisionState is the once: a second install
// would record our own hook as the "original" and recurse (review r1).
static _Atomic int gVisionState;   // 0 not installed, 1 installed (or being installed)
static BOOL gVisionInSwap, gVisionNeedsStart;
static void M7VisionInstallOnce(void);
static void M7VisionInstall(void) {
    // called from the dyld add-image callback and from the pump queue (scene.inc)
    if (!objc_getClass("VNDetectBarcodesRequest")) return;
    int z = 0; if (atomic_compare_exchange_strong(&gVisionState, &z, 1)) M7VisionInstallOnce();
}
static void M7VisionInstallOnce(void) {
    Class C = objc_getClass("VNDetectBarcodesRequest"); if (!C) return;
    // Hook on VNDetectBarcodesRequest itself: if it only inherits an initializer from VNRequest, add an override on this class
    // instead of swizzling VNRequest's, which would pin the revision of every Vision request the app makes (review r2).
    Method m;
    SEL sch = sel_registerName("initWithCompletionHandler:"), si = sel_registerName("init");
    if ((m = class_getInstanceMethod(C, sch))) {
        gOrigVNBInitCH = method_getImplementation(m);
        if (!class_addMethod(C, sch, (IMP)m7_vnbInitCH, method_getTypeEncoding(m))) method_setImplementation(m, (IMP)m7_vnbInitCH);
    }
    // -init on VNRequest calls initWithCompletionHandler:nil, but hook it too in case it does not
    if ((m = class_getInstanceMethod(C, si)) && method_getImplementation(m) != (IMP)m7_vnbInit) {
        gOrigVNBInit = method_getImplementation(m);
        if (!class_addMethod(C, si, (IMP)m7_vnbInit, method_getTypeEncoding(m))) method_setImplementation(m, (IMP)m7_vnbInit);
    }
    LOG(@"vision fix installed (VNDetectBarcodesRequest -> rev %s)", getenv("M7_VISION_REV") ?: "1");
}
static void M7ImageAdded(const struct mach_header *mh, intptr_t slide) { if (!objc_getClass("VNDetectBarcodesRequest")) return; M7VisionInstall(); }
static void m7visionfix(void) {   // PREPARE 4: Vision already loaded -> its hooks go in at SWAP; otherwise START registers the add-image hook
    if (getenv("M7_NO_VISIONFIX")) return;   // m10: was ungated
    Class C = objc_getClass("VNDetectBarcodesRequest");
    if (!C) { gVisionNeedsStart = YES; return; }
    // optional hooks: SWAP installs (and START reports) only what resolved; with none resolved gVisionState stays 0, so the
    // pump queue's M7VisionInstall can still try later (review item 5)
    int n0 = gActNHooks;
    ActHookResolve(HG_CLIENT, "vision fix", C, "initWithCompletionHandler:", NO, YES, (IMP)m7_vnbInitCH, &gOrigVNBInitCH, NO);
    ActHookResolve(HG_CLIENT, "vision fix", C, "init", NO, YES, (IMP)m7_vnbInit, &gOrigVNBInit, NO);
    gVisionInSwap = gActNHooks > n0;
    if (!gVisionInSwap) LOG(@"vision fix: VNDetectBarcodesRequest has neither init method at prepare; not installed at SWAP");
}
static void M7VisionStart(void) {   // START 5
    if (gVisionNeedsStart) _dyld_register_func_for_add_image(M7ImageAdded);   // Vision may be loaded later (e.g. dlopen by a plugin)
    else if (gVisionInSwap) LOG(@"vision fix installed (VNDetectBarcodesRequest -> rev %s; at SWAP)", getenv("M7_VISION_REV") ?: "1");
}
