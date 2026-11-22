// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): rotation coordinator, preview rotation.
//
// Provides: CoordTable, CoordOrientationChanged (client/motion.inc; CoordOrientationChanged also core/control.inc)
//           CoordAngleFor (providers/scene.inc)
//           CoordResolveHooks, CoordPublish, PreviewResolveRotation (core/lazy.inc)
// Owns:     nothing shared (other fragments reach the coordinator table through CoordTable).
// Uses:     core/act.inc, providers/scene.inc, providers/mirror.inc, client/motion.inc
#include "rotation.h"
#include "../core/activation.h"
#include "../providers/scene.h"
#include "motion.h"
#include "../providers/mirror.h"

// ============================================================================
// AVCaptureDeviceRotationCoordinator: the simulator build is a stub (the getters return ivars that stay 0, and
// nothing observes the orientation). The two getters are hooked to derive the angle from the UI orientation and the
// device position, and KVO fires when the orientation changes.
// ============================================================================
// The coordinator table is allocated in the resolve step (CFBridgingRetain'd, kept as void *) and published at the end
// of SWAP with release semantics; readers acquire it. CoordOrientationChanged also returns unless ARMED, so a tracker
// started during PREPARE never reads it before it is published. kCoordPositionKey is the associated-object key for a
// coordinator's camera position.
static void *_Atomic gCoordsRaw;
static void *gCoordsNext;
static char kCoordPositionKey;

static NSHashTable *CoordTable(void) {
    return (__bridge NSHashTable *)atomic_load_explicit(&gCoordsRaw, memory_order_acquire);
}

// The preview's horizon-level angle in degrees for an interface orientation and a camera position. A device
// orientation can be passed too: in portrait and landscape it has the value of the interface orientation it gives
// (UIDeviceOrientationLandscapeLeft == UIInterfaceOrientationLandscapeRight).
static double CoordAngleFor(long orientation, long position) {
    BOOL front = position == AVCaptureDevicePositionFront;
    switch (orientation) {
        case UIInterfaceOrientationLandscapeRight: return front ? 180 : 0;
        case UIInterfaceOrientationLandscapeLeft: return front ? 0 : 180;
        case UIInterfaceOrientationPortraitUpsideDown: return 270;
        default: return 90;   // portrait
    }
}

static IMP gOrigCoordInit;

static id hook_coordinatorInit(id self, SEL _cmd, id device, id layer) {
    id coordinator = ((id (*)(id, SEL, id, id))gOrigCoordInit)(self, _cmd, device, layer);
    NSHashTable *coords = CoordTable();
    if (coordinator && coords) {
        @synchronized (coords) { [coords addObject:coordinator]; }
        objc_setAssociatedObject(coordinator, &kCoordPositionKey, @([[device valueForKey:@"position"] longValue]),
                                 OBJC_ASSOCIATION_RETAIN);
    }
    LOG(@"RotationCoordinator init device=%@ -> angle %.0f", [device valueForKey:@"localizedName"],
        CoordAngleFor(OrientUIAny(), [[device valueForKey:@"position"] longValue]));
    return coordinator;
}

// The getters run on any thread, such as a camera queue while main is busy. So they read the atomic gOrientation.ui,
// never gOrientTracker.uiOrientation.
static CGFloat hook_coordinatorPreviewAngle(id self, SEL _cmd) {
    return CoordAngleFor(OrientUIAny(), [objc_getAssociatedObject(self, &kCoordPositionKey) longValue]);
}

// The capture angle follows the device orientation (gravity), the preview angle the preview layer's UI orientation
// (as AVF documents).
static CGFloat hook_coordinatorCaptureAngle(id self, SEL _cmd) {
    return CoordAngleFor(OrientVirtualOn() ? OrientDevice() : OrientUIAny(),
                         [objc_getAssociatedObject(self, &kCoordPositionKey) longValue]);
}

static void CoordOrientationChanged(void) {
    if (!ActivationIsArmed()) {
        return;
    }
    NSHashTable *coords = CoordTable();
    if (!coords) {
        return;
    }
    NSArray *all;
    @synchronized (coords) { all = coords.allObjects; }
    for (id coordinator in all) {
        [coordinator willChangeValueForKey:@"videoRotationAngleForHorizonLevelPreview"];
        [coordinator willChangeValueForKey:@"videoRotationAngleForHorizonLevelCapture"];
        [coordinator didChangeValueForKey:@"videoRotationAngleForHorizonLevelPreview"];
        [coordinator didChangeValueForKey:@"videoRotationAngleForHorizonLevelCapture"];
    }
    if (all.count) {
        LOG(@"RotationCoordinator: orientation %ld -> KVO on %lu coordinators", gOrientTracker.uiOrientation,
            (unsigned long)all.count);
    }
}

// resolve step: allocate the table (NSHashTable's +initialize runs here, not in SWAP) and resolve the 3 hooks
static BOOL CoordResolveHooks(void) {
    if (!RotationCoordinatorFixOn()) {
        return YES;   // off switch: the coordinator stays the simulator's stub
    }
    if (!gCoordsNext) {
        gCoordsNext = (void *)CFBridgingRetain([NSHashTable weakObjectsHashTable]);
    }
    Class coordClass = objc_getClass("AVCaptureDeviceRotationCoordinator");
    return ActivationHookResolve(kHookGroupClient, "coordinator", coordClass, "initWithDevice:previewLayer:", NO, NO,
                                 (IMP)hook_coordinatorInit, &gOrigCoordInit, YES) &&
           ActivationHookResolve(kHookGroupClient, "coordinator", coordClass,
                                 "videoRotationAngleForHorizonLevelPreview", NO, NO, (IMP)hook_coordinatorPreviewAngle,
                                 NULL, YES) &&
           ActivationHookResolve(kHookGroupClient, "coordinator", coordClass,
                                 "videoRotationAngleForHorizonLevelCapture", NO, NO, (IMP)hook_coordinatorCaptureAngle,
                                 NULL, YES);
}

static void CoordPublish(void) {   // SWAP, last
    if (gCoordsNext) {
        atomic_store_explicit(&gCoordsRaw, gCoordsNext, memory_order_release);
    }
}

// ---- preview rotation ----
// AVF's preview layer combines the RotationDegrees it is given with its connection's videoRotationAngle. The fig
// layer cannot see the connection, so the angle is adjusted in the layer's own update method. With
// SIMCAM_OFF_SENSOR_LAYOUT, angle + 90 (PreviewOffsetWithoutVirtualOrientation) was measured upright for that mode's
// landscape sensor frames.
static IMP gOrigPreviewDidUpdate;
static char gPreviewRotationArgType;
// the rotationDegrees argument's index in method_getArgumentType (0 is self, 1 is _cmd, then slot and imageQueue)
#define kPreviewRotationArgIndex 4

// Measured on 26.3: the simulator's preview layer turns the buffer by (angle - R) clockwise, but by (180 - angle - R)
// when its connection is mirrored, and never mirrors. A device turns by angle and mirrors if videoMirrored. So R is 0
// unmirrored and 180 - 2 * angle mirrored (540 - 2 * angle keeps it positive before the modulo), and the pump flips the
// buffer itself (PreviewNoteFor, MirrorFlip). A fixed "angle + 270" equals R = 0 only for angle 90 (portrait).
static float PreviewRotationFor(id layer, float connectionAngle) {
    BOOL mirrored = [[[layer valueForKey:@"connection"] valueForKey:@"videoMirrored"] boolValue];
    if (PreviewMirrorOn()) {   // off switch: the preview is never mirrored
        NSString *key = [NSString stringWithFormat:@"<%@: %p", NSStringFromClass([layer class]), layer];
        // gPreviewNotes (providers/mirror.inc) is guarded by the source spec lock
        @synchronized (gScene.request.lock) {
            if (!gPreviewNotes) {
                gPreviewNotes = [NSMutableDictionary dictionary];
            }
            gPreviewNotes[key] = @[@(connectionAngle), @(mirrored)];
        }
    } else {
        mirrored = NO;
    }
    return mirrored ? fmodf(540 - 2 * connectionAngle, 360) : 0;
}

// The preview angle offset in degrees without the virtual orientation: SIMCAM_PREVIEW_OFFSET overrides it; the sensor
// layout needs 270, and SIMCAM_OFF_SENSOR_LAYOUT's frames need 90.
static double PreviewOffsetWithoutVirtualOrientation(void) {
    if (getenv(ENV_PREVIEW_OFFSET)) {
        return atof(getenv(ENV_PREVIEW_OFFSET));
    }
    if (SensorLayoutOn()) {
        return 270;
    }
    return 90;
}

// The rotation the hooks pass on to AVF's own method. SIMCAM_PREVIEW_ROTATION: the rotation AVF passed, unchanged.
// Both hooks call this: a float argument and angle convert to double and back without loss.
static double PreviewRotationToPass(id layer, double avfRotation, double connectionAngle) {
    if (getenv(ENV_PREVIEW_ROTATION)) {
        return avfRotation;
    }
    if (OrientVirtualOn()) {
        return PreviewRotationFor(layer, connectionAngle);
    }
    return connectionAngle + PreviewOffsetWithoutVirtualOrientation();
}

static void hook_previewDidUpdateFloat(id self, SEL _cmd, unsigned slot, id imageQueue, float rotation, CGSize size) {
    float connectionAngle = [[[self valueForKey:@"connection"] valueForKey:@"videoRotationAngle"] floatValue];
    ((void (*)(id, SEL, unsigned, id, float, CGSize))gOrigPreviewDidUpdate)(
        self, _cmd, slot, imageQueue, PreviewRotationToPass(self, rotation, connectionAngle), size);
}

static void hook_previewDidUpdateDouble(id self, SEL _cmd, unsigned slot, id imageQueue, double rotation, CGSize size) {
    double connectionAngle = [[[self valueForKey:@"connection"] valueForKey:@"videoRotationAngle"] doubleValue];
    ((void (*)(id, SEL, unsigned, id, double, CGSize))gOrigPreviewDidUpdate)(
        self, _cmd, slot, imageQueue, PreviewRotationToPass(self, rotation, connectionAngle), size);
}

// resolve step: the rotation argument's type picks the replacement ('f' float or 'd' double)
static BOOL PreviewResolveRotation(void) {
    Method method = class_getInstanceMethod(
        objc_getClass("AVCaptureVideoPreviewLayer"),
        sel_registerName("didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:"));
    if (!method) {
        snprintf(gActivation.hooks.missing, sizeof gActivation.hooks.missing,
                 "preview rotation: no didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:");
        return NO;
    }

    char type[16] = {0};
    method_getArgumentType(method, kPreviewRotationArgIndex, type, sizeof type);
    gPreviewRotationArgType = type[0];

    if (gPreviewRotationArgType != 'f' && gPreviewRotationArgType != 'd') {
        snprintf(gActivation.hooks.missing, sizeof gActivation.hooks.missing,
                 "preview rotation: rotation argument type %s", type);
        return NO;
    }

    return ActivationHookResolve(
        kHookGroupClient, "preview rotation", objc_getClass("AVCaptureVideoPreviewLayer"),
        "didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:", NO, NO,
        gPreviewRotationArgType == 'f' ? (IMP)hook_previewDidUpdateFloat : (IMP)hook_previewDidUpdateDouble,
        &gOrigPreviewDidUpdate, YES);
}
