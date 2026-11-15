// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): rotation coordinator, preview rotation.
// ============================================================================
// AVCaptureDeviceRotationCoordinator — the simulator build is a stub (getters return ivars = 0, nothing observes
// orientation). Hook the two getters to derive the angle from the UI orientation + device position, and fire KVO when
// the orientation changes.
// ============================================================================
// The coordinator table is allocated in PREPARE 4 (CFBridgingRetain'd, kept as void *) and published in SWAP 8 with release
// semantics; readers acquire it. M5CoordOrientationChanged also returns unless ARMED, so a tracker started during prepare never
// reads it before it is published. kCoordPosKey is the associated-object key.
static void *_Atomic gCoordsRaw; static void *gCoordsNext; static char kCoordPosKey;
static NSHashTable *M5Coords(void) { return (__bridge NSHashTable *)atomic_load_explicit(&gCoordsRaw, memory_order_acquire); }
static double M5AngleFor(long iface, long position) {
    BOOL front = position == 2;
    switch (iface) {
        case 3: return front ? 180 : 0;    // UIInterfaceOrientationLandscapeRight
        case 4: return front ? 0 : 180;    // UIInterfaceOrientationLandscapeLeft
        case 2: return 270;                // portraitUpsideDown
        default: return 90;                // portrait
    }
}
static IMP gOrigCoordInit;
static id m5_coordInit(id self, SEL c, id device, id layer) {
    id r = ((id (*)(id, SEL, id, id))gOrigCoordInit)(self, c, device, layer);
    NSHashTable *coords = M5Coords();
    if (r && coords) { @synchronized (coords) { [coords addObject:r]; } objc_setAssociatedObject(r, &kCoordPosKey, @([[device valueForKey:@"position"] longValue]), OBJC_ASSOCIATION_RETAIN); }
    LOG(@"RotationCoordinator init device=%@ -> angle %.0f", [device valueForKey:@"localizedName"], M5AngleFor(M5UIOrientAny(), [[device valueForKey:@"position"] longValue]));
    return r;
}
// the getters run on any thread (a camera queue while main is busy): they read the atomic gUIOrientA, never gUIOrient
static CGFloat m5_coordAngle(id self, SEL c) { return M5AngleFor(M5UIOrientAny(), [objc_getAssociatedObject(self, &kCoordPosKey) longValue]); }
// capture angle follows the device orientation (gravity), preview angle the preview layer's UI orientation (as AVF documents)
static CGFloat m12_coordCapAngle(id self, SEL c) { return M5AngleFor(M12OrientOn() ? M12DevOrient() : M5UIOrientAny(), [objc_getAssociatedObject(self, &kCoordPosKey) longValue]); }
static void M5CoordOrientationChanged(void) {
    if (!ActIsArmed()) return;
    NSHashTable *coords = M5Coords(); if (!coords) return;
    NSArray *all; @synchronized (coords) { all = coords.allObjects; }
    for (id co in all) {
        [co willChangeValueForKey:@"videoRotationAngleForHorizonLevelPreview"]; [co willChangeValueForKey:@"videoRotationAngleForHorizonLevelCapture"];
        [co didChangeValueForKey:@"videoRotationAngleForHorizonLevelPreview"]; [co didChangeValueForKey:@"videoRotationAngleForHorizonLevelCapture"];
    }
    if (all.count) LOG(@"RotationCoordinator: orientation %ld -> KVO on %lu coordinators", gUIOrient, (unsigned long)all.count);
}
static BOOL m5coord(void) {   // PREPARE 4: allocate the table (runs NSHashTable's +initialize here), resolve the 3 hooks
    if (getenv("M5_NO_COORD")) return YES;
    if (!gCoordsNext) gCoordsNext = (void *)CFBridgingRetain([NSHashTable weakObjectsHashTable]);
    Class C = objc_getClass("AVCaptureDeviceRotationCoordinator");
    return ActHookResolve(HG_CLIENT, "coordinator", C, "initWithDevice:previewLayer:", NO, NO, (IMP)m5_coordInit, &gOrigCoordInit, YES) &&
           ActHookResolve(HG_CLIENT, "coordinator", C, "videoRotationAngleForHorizonLevelPreview", NO, NO, (IMP)m5_coordAngle, NULL, YES) &&
           ActHookResolve(HG_CLIENT, "coordinator", C, "videoRotationAngleForHorizonLevelCapture", NO, NO, (IMP)m12_coordCapAngle, NULL, YES);
}
static void M5CoordPublish(void) { if (gCoordsNext) atomic_store_explicit(&gCoordsRaw, gCoordsNext, memory_order_release); }   // SWAP 8

// ---- preview rotation: AVF's preview layer combines RotationDegrees with its connection's videoRotationAngle.
// Empirically upright+full-frame when RotationDegrees = connection.videoRotationAngle + 90 (our buffers are landscape
// sensor frames). The fig layer cannot see the connection, so adjust in the layer's own update method.
static IMP gOrigDidUpd; static char gDidUpdType;
// Measured on 26.3: the sim preview layer turns the buffer by (angle - R) clockwise, but by (180 - angle - R) when its connection is mirrored,
// and never mirrors. Device: turn by angle, mirror if videoMirrored. So R = 0 (unmirrored) / 180 - 2*angle (mirrored), and the pump flips the
// buffer itself (see M12PreviewFlip). A fixed "angle + 270" equals R = 0 only for angle 90 (portrait).
static float M12PreviewR(id layer, float conn) {
    BOOL mir = [[[layer valueForKey:@"connection"] valueForKey:@"videoMirrored"] boolValue];
    if (!getenv("M12_NO_PREVIEWMIRROR")) { NSString *key = [NSString stringWithFormat:@"<%@: %p", NSStringFromClass([layer class]), layer];
        @synchronized (gSrcSpecLock) { if (!gPrevNotes) gPrevNotes = [NSMutableDictionary dictionary]; gPrevNotes[key] = @[@(conn), @(mir)]; } }
    else mir = NO;
    return mir ? fmodf(540 - 2 * conn, 360) : 0;
}
static void m5_didUpdF(id self, SEL c, unsigned slot, id q, float rot, CGSize sz) {
    float conn = [[[self valueForKey:@"connection"] valueForKey:@"videoRotationAngle"] floatValue];
    ((void (*)(id, SEL, unsigned, id, float, CGSize))gOrigDidUpd)(self, c, slot, q, getenv("M5_PREVIEW_ROT") ? rot : M12OrientOn() ? M12PreviewR(self, conn) : conn + M7PreviewOfs(), sz);
}
static void m5_didUpdD(id self, SEL c, unsigned slot, id q, double rot, CGSize sz) {
    double conn = [[[self valueForKey:@"connection"] valueForKey:@"videoRotationAngle"] doubleValue];
    ((void (*)(id, SEL, unsigned, id, double, CGSize))gOrigDidUpd)(self, c, slot, q, getenv("M5_PREVIEW_ROT") ? rot : M12OrientOn() ? M12PreviewR(self, conn) : conn + M7PreviewOfs(), sz);
}
static BOOL m5prevrot(void) {   // PREPARE 4: the rotation argument's type picks the replacement ('f' or 'd')
    Method m = class_getInstanceMethod(objc_getClass("AVCaptureVideoPreviewLayer"), sel_registerName("didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:"));
    if (!m) { snprintf(gActMissing, sizeof gActMissing, "preview rotation: no didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:"); return NO; }
    char t[16] = {0}; method_getArgumentType(m, 4, t, sizeof t); gDidUpdType = t[0];
    if (gDidUpdType != 'f' && gDidUpdType != 'd') { snprintf(gActMissing, sizeof gActMissing, "preview rotation: rotation argument type %s", t); return NO; }
    return ActHookResolve(HG_CLIENT, "preview rotation", objc_getClass("AVCaptureVideoPreviewLayer"), "didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:", NO, NO,
                          gDidUpdType == 'f' ? (IMP)m5_didUpdF : (IMP)m5_didUpdD, &gOrigDidUpd, YES);
}

