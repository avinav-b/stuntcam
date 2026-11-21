// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): rotation coordinator, preview rotation.
// ============================================================================
// AVCaptureDeviceRotationCoordinator — the simulator build is a stub (getters return ivars = 0, nothing observes
// orientation). Hook the two getters to derive the angle from the UI orientation + device position, and fire KVO when
// the orientation changes.
// ============================================================================
// The coordinator table is allocated in PREPARE 4 (CFBridgingRetain'd, kept as void *) and published in SWAP 8 with
// release semantics; readers acquire it. CoordOrientationChanged also returns unless ARMED, so a tracker started
// during prepare never reads it before it is published. kCoordPosKey is the associated-object key.
static void *_Atomic gCoordsRaw;
static void *gCoordsNext;
static char kCoordPosKey;
static NSHashTable *CoordTable(void) {
    return (__bridge NSHashTable *)atomic_load_explicit(&gCoordsRaw, memory_order_acquire);
}
static double CoordAngleFor(long iface, long position) {
    BOOL front = position == 2;
    switch (iface) {
        case 3: return front ? 180 : 0;    // UIInterfaceOrientationLandscapeRight
        case 4: return front ? 0 : 180;    // UIInterfaceOrientationLandscapeLeft
        case 2: return 270;                // portraitUpsideDown
        default: return 90;                // portrait
    }
}
static IMP gOrigCoordInit;
static id hook_coordinatorInit(id self, SEL c, id device, id layer) {
    id r = ((id (*)(id, SEL, id, id))gOrigCoordInit)(self, c, device, layer);
    NSHashTable *coords = CoordTable();
    if (r && coords) {
        @synchronized (coords) { [coords addObject:r]; }
        objc_setAssociatedObject(r, &kCoordPosKey, @([[device valueForKey:@"position"] longValue]),
                                 OBJC_ASSOCIATION_RETAIN);
    }
    LOG(@"RotationCoordinator init device=%@ -> angle %.0f", [device valueForKey:@"localizedName"],
        CoordAngleFor(OrientUIAny(), [[device valueForKey:@"position"] longValue]));
    return r;
}
// the getters run on any thread (a camera queue while main is busy): they read the atomic gUIOrientA, never gUIOrient
static CGFloat hook_coordinatorPreviewAngle(id self, SEL c) {
    return CoordAngleFor(OrientUIAny(), [objc_getAssociatedObject(self, &kCoordPosKey) longValue]);
}
// capture angle follows the device orientation (gravity), preview angle the preview layer's UI orientation (as AVF
// documents)
static CGFloat hook_coordinatorCaptureAngle(id self, SEL c) {
    return CoordAngleFor(OrientVirtualOn() ? OrientDevice() : OrientUIAny(),
                      [objc_getAssociatedObject(self, &kCoordPosKey) longValue]);
}
static void CoordOrientationChanged(void) {
    if (!ActIsArmed()) return;
    NSHashTable *coords = CoordTable();
    if (!coords) return;
    NSArray *all;
    @synchronized (coords) { all = coords.allObjects; }
    for (id co in all) {
        [co willChangeValueForKey:@"videoRotationAngleForHorizonLevelPreview"];
        [co willChangeValueForKey:@"videoRotationAngleForHorizonLevelCapture"];
        [co didChangeValueForKey:@"videoRotationAngleForHorizonLevelPreview"];
        [co didChangeValueForKey:@"videoRotationAngleForHorizonLevelCapture"];
    }
    if (all.count)
        LOG(@"RotationCoordinator: orientation %ld -> KVO on %lu coordinators", gUIOrient, (unsigned long)all.count);
}
// PREPARE 4: allocate the table (runs NSHashTable's +initialize here), resolve the 3 hooks
static BOOL CoordResolveHooks(void) {
    if (getenv(ENV_OFF_ROTATION_COORDINATOR)) return YES;   // off switch: the coordinator stays the simulator's stub
    if (!gCoordsNext) gCoordsNext = (void *)CFBridgingRetain([NSHashTable weakObjectsHashTable]);
    Class C = objc_getClass("AVCaptureDeviceRotationCoordinator");
    return ActHookResolve(HG_CLIENT, "coordinator", C, "initWithDevice:previewLayer:", NO, NO,
                          (IMP)hook_coordinatorInit, &gOrigCoordInit, YES) &&
           ActHookResolve(HG_CLIENT, "coordinator", C, "videoRotationAngleForHorizonLevelPreview", NO, NO,
                          (IMP)hook_coordinatorPreviewAngle, NULL, YES) &&
           ActHookResolve(HG_CLIENT, "coordinator", C, "videoRotationAngleForHorizonLevelCapture", NO, NO,
                          (IMP)hook_coordinatorCaptureAngle, NULL, YES);
}
static void CoordPublish(void) {   // SWAP 8
    if (gCoordsNext) atomic_store_explicit(&gCoordsRaw, gCoordsNext, memory_order_release);
}

// ---- preview rotation: AVF's preview layer combines RotationDegrees with its connection's videoRotationAngle.
// The fig layer cannot see the connection, so adjust in the layer's own update method. With SIMCAM_OFF_SENSOR_LAYOUT,
// angle + 90 (PreviewOffsetWithoutVirtualOrientation) was measured upright for its landscape sensor frames.
static IMP gOrigDidUpd;
static char gDidUpdType;
// Measured on 26.3: the sim preview layer turns the buffer by (angle - R) clockwise, but by (180 - angle - R) when its
// connection is mirrored, and never mirrors. Device: turn by angle, mirror if videoMirrored. So R = 0 (unmirrored) /
// 180 - 2*angle (mirrored), and the pump flips the buffer itself (PreviewNoteFor, MirrorFlip). A fixed "angle + 270"
// equals R = 0 only for angle 90 (portrait).
static float PreviewRotationFor(id layer, float conn) {
    BOOL mirrored = [[[layer valueForKey:@"connection"] valueForKey:@"videoMirrored"] boolValue];
    if (!getenv(ENV_OFF_PREVIEW_MIRROR)) {   // off switch: the preview is never mirrored
        NSString *key = [NSString stringWithFormat:@"<%@: %p", NSStringFromClass([layer class]), layer];
        @synchronized (gSrcSpecLock) {   // gPrevNotes (providers/mirror.inc) is guarded by the source spec lock
            if (!gPrevNotes) gPrevNotes = [NSMutableDictionary dictionary];
            gPrevNotes[key] = @[@(conn), @(mirrored)];
        }
    } else {
        mirrored = NO;
    }
    return mirrored ? fmodf(540 - 2 * conn, 360) : 0;
}
// the preview angle offset without the virtual orientation: SIMCAM_PREVIEW_OFFSET overrides; SIMCAM_OFF_SENSOR_LAYOUT's
// sensor layout needs 90
static double PreviewOffsetWithoutVirtualOrientation(void) {
    return getenv(ENV_PREVIEW_OFFSET) ? atof(getenv(ENV_PREVIEW_OFFSET)) : getenv(ENV_OFF_SENSOR_LAYOUT) ? 90 : 270;
}
// SIMCAM_PREVIEW_ROTATION: the rotation AVF passes, unchanged
static void hook_previewDidUpdateFloat(id self, SEL c, unsigned slot, id q, float rot, CGSize sz) {
    float conn = [[[self valueForKey:@"connection"] valueForKey:@"videoRotationAngle"] floatValue];
    ((void (*)(id, SEL, unsigned, id, float, CGSize))gOrigDidUpd)(
        self, c, slot, q,
        getenv(ENV_PREVIEW_ROTATION) ? rot
        : OrientVirtualOn()          ? PreviewRotationFor(self, conn)
                                     : conn + PreviewOffsetWithoutVirtualOrientation(),
        sz);
}
static void hook_previewDidUpdateDouble(id self, SEL c, unsigned slot, id q, double rot, CGSize sz) {
    double conn = [[[self valueForKey:@"connection"] valueForKey:@"videoRotationAngle"] doubleValue];
    ((void (*)(id, SEL, unsigned, id, double, CGSize))gOrigDidUpd)(
        self, c, slot, q,
        getenv(ENV_PREVIEW_ROTATION) ? rot
        : OrientVirtualOn()          ? PreviewRotationFor(self, conn)
                                     : conn + PreviewOffsetWithoutVirtualOrientation(),
        sz);
}
// PREPARE 4: the rotation argument's type picks the replacement ('f' or 'd')
static BOOL PreviewResolveRotation(void) {
    Method m = class_getInstanceMethod(
        objc_getClass("AVCaptureVideoPreviewLayer"),
        sel_registerName("didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:"));
    if (!m) {
        snprintf(gActMissing, sizeof gActMissing,
                 "preview rotation: no didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:");
        return NO;
    }
    char type[16] = {0};
    method_getArgumentType(m, 4, type, sizeof type);
    gDidUpdType = type[0];
    if (gDidUpdType != 'f' && gDidUpdType != 'd') {
        snprintf(gActMissing, sizeof gActMissing, "preview rotation: rotation argument type %s", type);
        return NO;
    }
    return ActHookResolve(HG_CLIENT, "preview rotation", objc_getClass("AVCaptureVideoPreviewLayer"),
                          "didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:", NO, NO,
                          gDidUpdType == 'f' ? (IMP)hook_previewDidUpdateFloat : (IMP)hook_previewDidUpdateDouble,
                          &gOrigDidUpd, YES);
}

