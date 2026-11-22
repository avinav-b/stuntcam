// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): CoreMotion accelerometer stand-in.
//
// Provides: MotionInstallHooks, MotionInstallStockTraps, OrientTrackRunMain, OrientTrackStartForCamera,
//           OrientUISnapAtLoad (core/lazy.inc)
// Owns:     gOrientTracker, the UI orientation (main thread only; read by client/rotation.inc), and gMotion
//           (core/lazy.inc reads installedAt and stockTrapCount), defined in client/motion.h
// Uses:     core/act.inc, core/safety.inc, providers/scene.inc, client/rotation.inc, core/lazy.inc
//
// ============================================================================
// CoreMotion: CMMotionManager reports an accelerometer and device motion, and delivers gravity that matches the
// current UI orientation (portrait by default).
// Hooked: the 16 CMMotionManager selectors of kMotionHooks (below).
// Data objects: subclasses of CMAccelerometerData (-acceleration), CMDeviceMotion (-gravity, -userAcceleration,
// -attitude, -magneticField) and CMAttitude (the attitude stand-in, below).
// ============================================================================
#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>
#include "motion.h"
#include "../core/sentinel.h"
#include "../providers/scene.h"
#include "../core/activation.h"
#include "rotation.h"

@interface SimcamAccelerometerData : CMAccelerometerData
@property CMAcceleration simAcceleration;
@property NSTimeInterval simTimestamp;
@end

@implementation SimcamAccelerometerData

- (CMAcceleration)acceleration { return _simAcceleration; }

- (NSTimeInterval)timestamp { return _simTimestamp; }
@end

// ---- the attitude stand-in ----
// The device is held still, upright in the current orientation, yaw 0, in CMAttitudeReferenceFrameXArbitraryZVertical.
// Apple's conventions, measured on real CMAttitude objects (fixtures/motion-probe -attitude 1 checks each sample):
//   - q (x, y, z, w) rotates device coordinates into the reference frame: v_ref = q v_dev q*;
//   - rotationMatrix is q's matrix transposed (reference to device), so gravity = -(m13, m23, m33);
//   - q = qz(yaw) * qx(pitch) * qy(roll): pitch = asin(m23), roll = atan2(-m13, m33), yaw = atan2(-m21, m22);
//   - [a multiplyByInverseOfAttitude:b] sets a = b^-1 * a in place, and a CMDeviceMotion returns the same CMAttitude
//     on every read, so the multiply sticks; -copy gives a new object.
// CMAttitude has no public initializer, so SimcamAttitude subclasses it and overrides every method that reads its
// private storage. At upright portrait (gimbal lock, pitch +pi/2) it reports roll 0 and yaw 0; Apple's -pi, -pi is
// the same rotation.
static CMQuaternion MotionQuatMul(CMQuaternion lhs, CMQuaternion rhs) {   // the Hamilton product lhs * rhs
    return (CMQuaternion){lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
                          lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
                          lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
                          lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z};
}

// reference to device: the transpose of the quaternion's rotation matrix
static CMRotationMatrix MotionQuatMatrix(CMQuaternion quaternion) {
    double x = quaternion.x;
    double y = quaternion.y;
    double z = quaternion.z;
    double w = quaternion.w;
    return (CMRotationMatrix){1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                              2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                              2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}

@interface SimcamAttitude : CMAttitude {
  @public
    CMQuaternion _quaternion;
    double _roll;
    double _pitch;
    double _yaw;
}
@end

// FNV-1a, 64-bit: the hash of the quaternion's bits
#define kFNV1aOffsetBasis 14695981039346656037ull
#define kFNV1aPrime 1099511628211ull

@implementation SimcamAttitude

+ (BOOL)supportsSecureCoding { return YES; }

- (void)simcam_recomputeAngles {   // Euler angles from the matrix, as CoreMotion computes them
    CMRotationMatrix matrix = MotionQuatMatrix(_quaternion);
    _pitch = asin(fmax(-1, fmin(1, matrix.m23)));
    _roll = atan2(-matrix.m13, matrix.m33);
    _yaw = atan2(-matrix.m21, matrix.m22);
}

- (CMQuaternion)quaternion { return _quaternion; }

- (CMRotationMatrix)rotationMatrix { return MotionQuatMatrix(_quaternion); }

- (double)roll { return _roll; }

- (double)pitch { return _pitch; }

- (double)yaw { return _yaw; }

- (void)setQuaternion:(CMQuaternion)quaternion {   // CMAttitude's private setter, in case anything calls it
    _quaternion = quaternion;
    [self simcam_recomputeAngles];
}

// nil leaves the attitude unchanged (Apple's segfaults on nil; this one would otherwise zero _quaternion)
- (void)multiplyByInverseOfAttitude:(CMAttitude *)other {
    if (!other) {
        return;
    }
    CMQuaternion otherQuaternion = other.quaternion;
    _quaternion = MotionQuatMul((CMQuaternion){-otherQuaternion.x, -otherQuaternion.y, -otherQuaternion.z,
                                               otherQuaternion.w},
                                _quaternion);
    [self simcam_recomputeAngles];
}

- (id)copyWithZone:(NSZone *)zone {
    SimcamAttitude *copy = [SimcamAttitude new];
    copy->_quaternion = _quaternion;
    copy->_roll = _roll;
    copy->_pitch = _pitch;
    copy->_yaw = _yaw;
    return copy;
}

- (BOOL)isEqual:(id)other {
    if (other == self) {
        return YES;
    }
    if (![other isKindOfClass:[CMAttitude class]]) {
        return NO;
    }
    CMQuaternion otherQuaternion = ((CMAttitude *)other).quaternion;
    return otherQuaternion.x == _quaternion.x && otherQuaternion.y == _quaternion.y &&
           otherQuaternion.z == _quaternion.z && otherQuaternion.w == _quaternion.w;
}

// The quaternion's bits. Adding 0.0 turns -0 into 0: they are equal under isEqual:, so they must hash the same.
- (NSUInteger)hash {
    double components[4] = {_quaternion.x + 0.0, _quaternion.y + 0.0, _quaternion.z + 0.0, _quaternion.w + 0.0};
    uint64_t hash = kFNV1aOffsetBasis;
    uint64_t bits;
    for (int i = 0; i < 4; i++) {
        memcpy(&bits, &components[i], sizeof bits);
        hash = (hash ^ bits) * kFNV1aPrime;
    }
    return (NSUInteger)hash;
}

- (NSString *)description {
    return [NSString stringWithFormat:@"CMAttitude Pitch: %f, Roll: %f, Yaw: %f", _pitch * 180 / M_PI,
                                      _roll * 180 / M_PI, _yaw * 180 / M_PI];
}

// Archived exactly as Apple archives a CMAttitude (four doubles under kCMAttitudeCodingKeyQX/QY/QZ/QW), so an app that
// saves a reference attitude can decode it without the engine. While the stand-in is active, every decode in the
// process gives a SimcamAttitude (MotionInstallHooks).
- (Class)classForCoder { return [CMAttitude class]; }

- (Class)classForKeyedArchiver { return [CMAttitude class]; }

- (void)encodeWithCoder:(NSCoder *)coder {
    [coder encodeDouble:_quaternion.x forKey:@"kCMAttitudeCodingKeyQX"];
    [coder encodeDouble:_quaternion.y forKey:@"kCMAttitudeCodingKeyQY"];
    [coder encodeDouble:_quaternion.z forKey:@"kCMAttitudeCodingKeyQZ"];
    [coder encodeDouble:_quaternion.w forKey:@"kCMAttitudeCodingKeyQW"];
}

// CoreMotion's keys: an archive of a real CMAttitude or of this class
- (instancetype)initWithCoder:(NSCoder *)coder {
    if (!(self = [super init])) {
        return nil;
    }
    _quaternion = (CMQuaternion){[coder decodeDoubleForKey:@"kCMAttitudeCodingKeyQX"],
                                 [coder decodeDoubleForKey:@"kCMAttitudeCodingKeyQY"],
                                 [coder decodeDoubleForKey:@"kCMAttitudeCodingKeyQZ"],
                                 [coder decodeDoubleForKey:@"kCMAttitudeCodingKeyQW"]};
    [self simcam_recomputeAngles];
    return self;
}
@end

// a gravity shorter than this has no direction: the attitude is the identity
#define kMotionMinGravityNorm 1e-9

// Held still, upright in the orientation that gives this gravity, yaw 0: q = qx(pitch) * qy(roll).
static SimcamAttitude *MotionAttitudeFor(CMAcceleration gravity) {
    SimcamAttitude *attitude = [SimcamAttitude new];
    double norm = sqrt(gravity.x * gravity.x + gravity.y * gravity.y + gravity.z * gravity.z);

    if (norm < kMotionMinGravityNorm) {
        attitude->_quaternion = (CMQuaternion){0, 0, 0, 1};
        return attitude;
    }

    double unitX = gravity.x / norm + 0.0;   // + 0.0: no -0 into atan2
    double unitY = gravity.y / norm;
    double unitZ = gravity.z / norm;
    double pitch = asin(fmax(-1, fmin(1, -unitY)));
    double roll = atan2(unitX, -unitZ + 0.0);
    double sinHalfPitch = sin(pitch / 2);
    double cosHalfPitch = cos(pitch / 2);
    double sinHalfRoll = sin(roll / 2);
    double cosHalfRoll = cos(roll / 2);
    attitude->_quaternion = (CMQuaternion){sinHalfPitch * cosHalfRoll, cosHalfPitch * sinHalfRoll,
                                           sinHalfPitch * sinHalfRoll, cosHalfPitch * cosHalfRoll};
    attitude->_pitch = pitch;   // stored, not recomputed: upright portrait is at gimbal lock (above)
    attitude->_roll = roll;
    attitude->_yaw = 0;
    return attitude;
}

@interface SimcamDeviceMotion : CMDeviceMotion
@property CMAcceleration simGravity;
@property NSTimeInterval simTimestamp;
@property SimcamAttitude *simAttitude;
@end

@implementation SimcamDeviceMotion

- (CMAcceleration)gravity { return _simGravity; }

- (CMAcceleration)userAcceleration { return (CMAcceleration){0, 0, 0}; }

- (CMRotationRate)rotationRate { return (CMRotationRate){0, 0, 0}; }

- (NSTimeInterval)timestamp { return _simTimestamp; }

- (double)heading { return -1; }

- (CMAttitude *)attitude { return _simAttitude; }   // one object per sample, as CoreMotion does

- (CMCalibratedMagneticField)magneticField {
    return (CMCalibratedMagneticField){{0, 0, 0}, CMMagneticFieldCalibrationAccuracyUncalibrated};
}

- (CMDeviceMotionSensorLocation)sensorLocation { return CMDeviceMotionSensorLocationDefault; }

- (id)copyWithZone:(NSZone *)zone {
    SimcamDeviceMotion *copy = [SimcamDeviceMotion new];
    copy.simGravity = _simGravity;
    copy.simTimestamp = _simTimestamp;
    copy.simAttitude = [_simAttitude copy];
    return copy;
}
@end

// gOrientTracker.uiOrientation is written and read on the main thread only. Readers off the main thread read
// gOrientation.ui (OrientUIAny, providers/scene.inc), which every writer stores too.

static CMAcceleration MotionGravity(void) {
    const char *override = getenv(ENV_GRAVITY);   // "x,y,z" override
    if (override) {
        CMAcceleration gravity;
        sscanf(override, "%lf,%lf,%lf", &gravity.x, &gravity.y, &gravity.z);
        return gravity;
    }
    // Gravity follows the (virtual) device, not the UI. This runs on timer threads. A device orientation has the value
    // of the interface orientation it gives, so the cases name interface orientations.
    switch (OrientVirtualOn() ? OrientDevice() : OrientUIAny()) {
        case UIInterfaceOrientationPortraitUpsideDown: return (CMAcceleration){0, 1, 0};
        case UIInterfaceOrientationLandscapeRight: return (CMAcceleration){-1, 0, 0};   // device landscapeLeft
        case UIInterfaceOrientationLandscapeLeft: return (CMAcceleration){1, 0, 0};     // device landscapeRight
        default: return (CMAcceleration){0, -1, 0};                                     // portrait
    }
}

// the associated-object keys of a manager's accelerometer timer and device-motion timer
static const void *kAccelerometerTimerKey = &kAccelerometerTimerKey;
static const void *kDeviceMotionTimerKey = &kDeviceMotionTimerKey;
static void OrientTrackStart(const char *startedBy);

// The test faults' crash: a write through a null pointer the compiler can't see (atoi("0")). Always inlined, so the
// fault stays in the function that calls it (that function is noinline, in our image).
static inline __attribute__((always_inline)) void TestFaultNullWriteInline(void) {
    volatile int *nullPointer = (volatile int *)(uintptr_t)atoi("0");
    *nullPointer = 1;
}

// SIMCAM_TEST_MOTION_FAULT=1 (activation.sh motion-crash): a fault in our image inside a motion timer tick, 1 s after
// the first tick (gMotion.firstTickNs)

__attribute__((noinline)) static void MotionTestFaultCheck(void) {
    if (!gTestMotionFault) {
        return;
    }
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t unset = 0;
    if (atomic_compare_exchange_strong(&gMotion.firstTickNs, &unset, now)) {
        return;
    }
    if (now - atomic_load(&gMotion.firstTickNs) < NSEC_PER_SEC) {
        return;
    }
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_MOTION_FAULT: fault in a motion tick now\n", getpid());
    TestFaultNullWriteInline();
}

// the shortest timer interval: an app that asks for more than 100 Hz gets 100 Hz
#define kMotionMinIntervalS 0.01

// Every tick runs as a motion entry point (the feature key, core/safety.inc), whatever the tick block does.
static dispatch_source_t MotionTimer(id manager, double interval, void (^tick)(void)) {
    dispatch_source_t timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, (uint64_t)(MAX(interval, kMotionMinIntervalS) * NSEC_PER_SEC),
                              NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
        SENTINEL_MOTION_SCOPE;
        MotionTestFaultCheck();
        tick();
    });
    dispatch_resume(timer);
    return timer;
}

// The operation block that calls the app's handler: a motion entry point, with the key cleared around the app's
// handler, so a fault in app code never counts (an app's own crash must never latch stuntcam).
#define MOTION_CALL_APP(call)                    \
    do {                                         \
        SENTINEL_MOTION_SCOPE;                   \
        void *_app = SentinelFeatureEnter(NULL); \
        call;                                    \
        SentinelFeatureLeave(_app);              \
    } while (0)
// A message inside a motion scope that app code can answer (a CMMotionManager subclass's interval getter, an
// NSOperationQueue subclass's addOperationWithBlock:, the copy of an app's block): the key is cleared around it, as
// around the app's handler, so a crash in our image reached from that app code is never `motion`. Guarded by
// activation.sh app-send-crash (an interval getter that makes the first camera call; SIMCAM_TEST_TRAP_FAULT).
// SIMCAM_OFF_APP_SEND_KEY_CLEAR=1 keeps the key in all three (MOTION_APP_SEND, MOTION_APP_DO, MotionEnqueue).
#define MOTION_APP_SEND(expr)                                              \
    ({                                                                     \
        void *_appS = SentinelFeatureClearUnless(gOffAppSendKeyClear);     \
        __auto_type _r = (expr);                                           \
        SentinelFeatureRestoreUnless(gOffAppSendKeyClear, _appS);          \
        _r;                                                                \
    })
#define MOTION_APP_DO(stmt)                                                \
    do {                                                                   \
        void *_appS = SentinelFeatureClearUnless(gOffAppSendKeyClear);     \
        stmt;                                                              \
        SentinelFeatureRestoreUnless(gOffAppSendKeyClear, _appS);          \
    } while (0)

static SimcamAccelerometerData *MotionMakeAccelerometerData(void) {
    SimcamAccelerometerData *data = [SimcamAccelerometerData new];
    data.simAcceleration = MotionGravity();
    data.simTimestamp = [NSProcessInfo processInfo].systemUptime;
    return data;
}

static SimcamDeviceMotion *MotionMakeDeviceMotion(void) {
    SimcamDeviceMotion *motion = [SimcamDeviceMotion new];
    motion.simGravity = MotionGravity();
    motion.simAttitude = MotionAttitudeFor(motion.simGravity);
    motion.simTimestamp = [NSProcessInfo processInfo].systemUptime;
    return motion;
}

static void MotionStopTimer(id manager, const void *key) {
    dispatch_source_t timer = objc_getAssociatedObject(manager, key);
    if (timer) {
        dispatch_source_cancel(timer);
    }
    objc_setAssociatedObject(manager, key, nil, OBJC_ASSOCIATION_RETAIN);
}

// the associated-object key of the reference frame of the last device-motion start (an NSNumber)
static const void *kReferenceFrameKey = &kReferenceFrameKey;

static void MotionSetFrame(id manager, CMAttitudeReferenceFrame frame) {
    objc_setAssociatedObject(manager, kReferenceFrameKey, @(frame), OBJC_ASSOCIATION_RETAIN);
}

static void MotionEnqueue(NSOperationQueue *queue, void (^operation)(void)) {
    MOTION_APP_DO([queue addOperationWithBlock:operation]);
}

// Availability queries never start orientation tracking; starts and data calls do.
// Every hooked selector is a motion entry point (SENTINEL_MOTION_SCOPE: the feature key says `motion` on this thread
// until it returns).
static BOOL hook_motionAvailable(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    return YES;
}

static BOOL hook_isAccelerometerActive(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    return objc_getAssociatedObject(self, kAccelerometerTimerKey) != nil;
}

static BOOL hook_isDeviceMotionActive(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    return objc_getAssociatedObject(self, kDeviceMotionTimerKey) != nil;
}

static id hook_accelerometerData(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    OrientTrackStart("accelerometerData");
    return hook_isAccelerometerActive(self, _cmd) ? MotionMakeAccelerometerData() : nil;
}

static id hook_deviceMotion(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    OrientTrackStart("deviceMotion");
    return hook_isDeviceMotionActive(self, _cmd) ? MotionMakeDeviceMotion() : nil;
}

static void hook_startAccelerometerUpdates(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    OrientTrackStart("startAccelerometerUpdates");
    MotionStopTimer(self, kAccelerometerTimerKey);
    objc_setAssociatedObject(self, kAccelerometerTimerKey, MotionTimer(self, 1, ^{}), OBJC_ASSOCIATION_RETAIN);
}

static void hook_startAccelerometerUpdatesToQueue(id self, SEL _cmd, NSOperationQueue *queue,
                                                  CMAccelerometerHandler handler) {
    SENTINEL_MOTION_SCOPE;
    OrientTrackStart("startAccelerometerUpdatesToQueue:withHandler:");
    MotionStopTimer(self, kAccelerometerTimerKey);
    double interval = MOTION_APP_SEND([(CMMotionManager *)self accelerometerUpdateInterval]);
    LOG(@"CMMotionManager startAccelerometerUpdatesToQueue (interval %.3f) gravity portrait-ish", interval);
    MOTION_APP_DO(handler = [handler copy]);
    objc_setAssociatedObject(self, kAccelerometerTimerKey, MotionTimer(self, interval, ^{
                                 SimcamAccelerometerData *data = MotionMakeAccelerometerData();
                                 MotionEnqueue(queue, ^{
                                     MOTION_CALL_APP(handler(data, nil));
                                 });
                             }),
                             OBJC_ASSOCIATION_RETAIN);
}

static void hook_stopAccelerometerUpdates(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    MotionStopTimer(self, kAccelerometerTimerKey);
}

static void MotionStartDeviceMotionPull(id manager, CMAttitudeReferenceFrame frame, const char *startedBy) {
    OrientTrackStart(startedBy);
    MotionStopTimer(manager, kDeviceMotionTimerKey);
    MotionSetFrame(manager, frame);
    objc_setAssociatedObject(manager, kDeviceMotionTimerKey, MotionTimer(manager, 1, ^{}), OBJC_ASSOCIATION_RETAIN);
}

static void hook_startDeviceMotionUpdates(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    MotionStartDeviceMotionPull(self, CMAttitudeReferenceFrameXArbitraryZVertical, "startDeviceMotionUpdates");
}

static void hook_startDeviceMotionUpdatesUsingReferenceFrame(id self, SEL _cmd, CMAttitudeReferenceFrame frame) {
    SENTINEL_MOTION_SCOPE;
    LOG(@"CMMotionManager startDeviceMotionUpdatesUsingReferenceFrame:%lu (pull)", (unsigned long)frame);
    MotionStartDeviceMotionPull(self, frame, "startDeviceMotionUpdatesUsingReferenceFrame:");
}

static void MotionStartDeviceMotionPush(id manager, CMAttitudeReferenceFrame frame, NSOperationQueue *queue,
                                        CMDeviceMotionHandler handler, const char *startedBy) {
    OrientTrackStart(startedBy);
    MotionStopTimer(manager, kDeviceMotionTimerKey);
    MotionSetFrame(manager, frame);
    double interval = MOTION_APP_SEND([(CMMotionManager *)manager deviceMotionUpdateInterval]);
    LOG(@"CMMotionManager %s (interval %.3f, reference frame %lu)", startedBy, interval, (unsigned long)frame);
    MOTION_APP_DO(handler = [handler copy]);
    objc_setAssociatedObject(manager, kDeviceMotionTimerKey, MotionTimer(manager, interval, ^{
                                 SimcamDeviceMotion *motion = MotionMakeDeviceMotion();
                                 MotionEnqueue(queue, ^{
                                     MOTION_CALL_APP(handler(motion, nil));
                                 });
                             }),
                             OBJC_ASSOCIATION_RETAIN);
}

static void hook_startDeviceMotionUpdatesToQueue(id self, SEL _cmd, NSOperationQueue *queue,
                                                 CMDeviceMotionHandler handler) {
    SENTINEL_MOTION_SCOPE;
    MotionStartDeviceMotionPush(self, CMAttitudeReferenceFrameXArbitraryZVertical, queue, handler,
                                "startDeviceMotionUpdatesToQueue:withHandler:");
}

// Any requested frame gets the same samples: a still device at yaw 0 (only xArbitraryZVertical is advertised, below).
static void hook_startDeviceMotionUpdatesUsingReferenceFrameToQueue(id self, SEL _cmd, CMAttitudeReferenceFrame frame,
                                                                    NSOperationQueue *queue,
                                                                    CMDeviceMotionHandler handler) {
    SENTINEL_MOTION_SCOPE;
    MotionStartDeviceMotionPush(self, frame, queue, handler,
                                "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:");
}

// +availableAttitudeReferenceFrames (a class method): the stand-in has no magnetometer and no drift correction
static NSUInteger hook_availableAttitudeReferenceFrames(id managerClass, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    return CMAttitudeReferenceFrameXArbitraryZVertical;
}

// -attitudeReferenceFrame: the last start's frame, xArbitraryZVertical before any start
static NSUInteger hook_attitudeReferenceFrame(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    NSNumber *frame = objc_getAssociatedObject(self, kReferenceFrameKey);
    return frame ? frame.unsignedIntegerValue : CMAttitudeReferenceFrameXArbitraryZVertical;
}

static void hook_stopDeviceMotionUpdates(id self, SEL _cmd) {
    SENTINEL_MOTION_SCOPE;
    MotionStopTimer(self, kDeviceMotionTimerKey);
}

// Two independent parts:
//   MotionInstallHooks   the stand-in's 16 selector hooks, at load, never removed. A camera latch, a motion latch or a
//                        preflight failure of this build makes CoreMotion stock from the next launch instead
//                        (SentinelMotionStockAtLoad), with traps that say so once (MotionInstallStockTraps).
//                        SIMCAM_OFF_MOTION=1 keeps CoreMotion stock, without the traps.
//   OrientTrack          the main-queue orientation tracker. The camera needs it too (rotation angles, the scene's
//                        orientation), so it starts at the first motion call or trapped camera call, whichever comes
//                        first, even with SIMCAM_OFF_MOTION=1.
// The 16 CMMotionManager selectors and the stand-in's replacements; a leading '+' marks a class method. The last three
// belong to the attitude stand-in.
typedef struct {
    const char *selector;
    IMP hook;
} MotionHook;

static const MotionHook kMotionHooks[] = {
    {.selector = "isAccelerometerAvailable", .hook = (IMP)hook_motionAvailable},
    {.selector = "isDeviceMotionAvailable", .hook = (IMP)hook_motionAvailable},
    {.selector = "isAccelerometerActive", .hook = (IMP)hook_isAccelerometerActive},
    {.selector = "isDeviceMotionActive", .hook = (IMP)hook_isDeviceMotionActive},
    {.selector = "accelerometerData", .hook = (IMP)hook_accelerometerData},
    {.selector = "deviceMotion", .hook = (IMP)hook_deviceMotion},
    {.selector = "startAccelerometerUpdates", .hook = (IMP)hook_startAccelerometerUpdates},
    {.selector = "startAccelerometerUpdatesToQueue:withHandler:", .hook = (IMP)hook_startAccelerometerUpdatesToQueue},
    {.selector = "stopAccelerometerUpdates", .hook = (IMP)hook_stopAccelerometerUpdates},
    {.selector = "startDeviceMotionUpdates", .hook = (IMP)hook_startDeviceMotionUpdates},
    {.selector = "startDeviceMotionUpdatesToQueue:withHandler:", .hook = (IMP)hook_startDeviceMotionUpdatesToQueue},
    {.selector = "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:",
     .hook = (IMP)hook_startDeviceMotionUpdatesUsingReferenceFrameToQueue},
    {.selector = "stopDeviceMotionUpdates", .hook = (IMP)hook_stopDeviceMotionUpdates},
    {.selector = "startDeviceMotionUpdatesUsingReferenceFrame:",
     .hook = (IMP)hook_startDeviceMotionUpdatesUsingReferenceFrame},
    {.selector = "attitudeReferenceFrame", .hook = (IMP)hook_attitudeReferenceFrame},
    {.selector = "+availableAttitudeReferenceFrames", .hook = (IMP)hook_availableAttitudeReferenceFrames},
};

#define kMotionHookCount (sizeof kMotionHooks / sizeof *kMotionHooks)

static Method MotionMethod(Class managerClass, const char *name, SEL *outSel) {   // "+sel" looks on the metaclass
    BOOL isClassMethod = name[0] == '+';
    *outSel = sel_registerName(isClassMethod ? name + 1 : name);
    return class_getInstanceMethod(isClassMethod ? object_getClass(managerClass) : managerClass, *outSel);
}

static Class MotionClass(void) {
    Class managerClass = objc_getClass("CMMotionManager");
    if (!managerClass) {
        dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW);
        managerClass = objc_getClass("CMMotionManager");
    }
    if (!managerClass) {
        LOG(@"MISMATCH: no CMMotionManager");
    }
    return managerClass;
}

static void MotionInstallHooks(void) {
    static int done;
    if (done) {
        return;
    }
    done = 1;
    Class managerClass = MotionClass();
    if (!managerClass) {
        return;
    }
    int hooked = 0;

    for (size_t i = 0; i < kMotionHookCount; i++) {
        SEL sel;
        Method method = MotionMethod(managerClass, kMotionHooks[i].selector, &sel);
        if (method) {
            method_setImplementation(method, kMotionHooks[i].hook);
            hooked++;
        } else {
            LOG(@"motion: missing %s", kMotionHooks[i].selector);
        }
    }

    gMotion.hookedCount = hooked;
    gMotion.installedAt = CFAbsoluteTimeGetCurrent();

    // Every attitude decoded while the stand-in is active is a SimcamAttitude. The stand-in archives in Apple's format,
    // so it would otherwise decode to a real CMAttitude, whose methods read the argument's private storage: a multiply
    // by a stand-in segfaults in CoreMotion, and isEqual: answers NO for the same quaternion.
    if (hooked) {
        [NSKeyedUnarchiver setClass:[SimcamAttitude class] forClassName:@"CMAttitude"];
    }
    LOG(@"motion: hooked %d CMMotionManager selectors; archived CMAttitudes decode as the stand-in", hooked);
}

// ---- CoreMotion stock from a marker (stock is never silent: the first call says why) ----
// The same 16 selectors get forwarding traps instead of the stand-in: the first call in the process queues the stock
// line (ActivationMotionStockNote), and every call forwards to the original. Each takes at most 3 integer or pointer
// arguments and returns an integer, a BOOL, an object or nothing, so the camera traps' void * trampoline
// (ActivationInstallTrap) fits them too. Not installed under
// SIMCAM_OFF_MOTION=1 (the user asked for stock).

static void MotionInstallStockTraps(void) {
    static int done;
    if (done) {
        return;
    }
    done = 1;
    Class managerClass = MotionClass();
    if (!managerClass) {
        return;
    }

    for (size_t i = 0; i < kMotionHookCount; i++) {
        SEL sel;
        Method method = MotionMethod(managerClass, kMotionHooks[i].selector, &sel);
        if (!method) {
            LOG(@"motion: missing %s", kMotionHooks[i].selector);
            continue;
        }
        IMP orig = method_getImplementation(method);
        IMP trap = imp_implementationWithBlock(^void *(void *receiver, void *arg1, void *arg2, void *arg3) {
            if (!atomic_load_explicit(&gMotion.stockNoted, memory_order_relaxed) &&
                !atomic_exchange(&gMotion.stockNoted, 1)) {
                ActivationMotionStockNote();
            }
            return ((void *(*)(void *, SEL, void *, void *, void *))orig)(receiver, sel, arg1, arg2, arg3);
        });
        method_setImplementation(method, trap);
        gMotion.stockTrapCount++;
    }

    gMotion.installedAt = CFAbsoluteTimeGetCurrent();
    LOG(@"motion: CoreMotion stock this launch (%s); %d one-shot forwarding traps", gSentinel.atLoad.motionStockMarker,
        gMotion.stockTrapCount);
}

// ---- the orientation tracker (main queue only) ----
// The tracker is camera code (the rotation coordinators and the scene orientation depend on it), not a motion entry
// point: OrientUpdate fires KVO on the coordinators, and their observers call our camera getters
// (hook_coordinatorPreviewAngle, hook_coordinatorCaptureAngle) on this thread. So OrientUpdate runs with the feature
// key CLEARED (saved and restored, as around the app's handler): a fault in it counts as `provider` (or an in-flight
// op's feature) and closes the camera, never as `motion`. Counted as `motion`, it only made CoreMotion stock, and the
// camera kept crashing every launch. SIMCAM_OFF_TRACKER_KEY_CLEAR=1 brings back the motion key here.

// SIMCAM_TEST_ORIENT_FAULT=1: once ARMED with a coordinator, OrientUpdate fires the coordinators' KVO, then faults in
// our image
__attribute__((noinline)) static void OrientTestFaultCheck(void) {
    if (gTestOrientFault != kOrientFaultUpd || !ActivationIsArmed()) {
        return;
    }
    NSHashTable *coords = CoordTable();
    NSUInteger coordCount = 0;
    if (coords) {
        @synchronized (coords) { coordCount = coords.count; }
    }
    if (!coordCount) {
        return;
    }
    CoordOrientationChanged();
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_ORIENT_FAULT: fault in the orientation tracker's upd now\n", getpid());
    TestFaultNullWriteInline();
}

// The early fault waits for ARMED (the full crash handler) in polls: up to 1000 x 5 ms = 5 s.
#define kOrientArmedWaitPolls 1000
#define kOrientArmedPollUs 5000

// SIMCAM_TEST_ORIENT_FAULT=start|runmain|ensure: a fault outside OrientUpdate, once per process, when the switch names
// mode or otherMode
__attribute__((noinline)) static void OrientTestEarlyFault(int mode, int otherMode, const char *where) {
    static _Atomic int once;
    BOOL selected = gTestOrientFault == mode || gTestOrientFault == otherMode;
    if (!selected || atomic_exchange(&once, 1)) {
        return;
    }
    for (int i = 0; i < kOrientArmedWaitPolls && !ActivationIsArmed(); i++) {
        usleep(kOrientArmedPollUs);
    }
    if (!ActivationIsArmed()) {
        fprintf(stderr, "[simcam %d] SIMCAM_TEST_ORIENT_FAULT: not armed after 5 s; no fault in %s\n", getpid(), where);
        return;
    }
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_ORIENT_FAULT: fault in %s now (%s thread)\n", getpid(), where,
            pthread_main_np() ? "main" : "bg");
    TestFaultNullWriteInline();
}

// Orientation values 1-4 are portrait, upside down and the two landscapes, in both UIDeviceOrientation and
// UIInterfaceOrientation; 0 is unknown, and 5 and 6 (a device face up or down) have no interface orientation.
#define kOrientValidMin 1
#define kOrientValidMax 4

static void OrientUpdate(void) {
    // camera code: no feature key while it runs (SIMCAM_OFF_TRACKER_KEY_CLEAR=1 keeps `motion` instead)
    SENTINEL_FEATURE_SCOPE(gOffTrackerKeyClear ? kMotionFeatureKey : NULL);

    // the first window scene's interface orientation
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:[UIWindowScene class]]) {
            long orientation = ((UIWindowScene *)scene).effectiveGeometry.interfaceOrientation;
            if (orientation != gOrientTracker.uiOrientation) {
                gOrientTracker.uiOrientation = orientation;
                atomic_store(&gOrientation.ui, orientation);
                CoordOrientationChanged();
            }
            break;
        }
    }

    // The device orientation (the simulator's rotation, or UIDevice set by our control file); it falls back to the UI
    // orientation.
    long deviceOrientation = UIDevice.currentDevice.orientation;
    if (deviceOrientation < kOrientValidMin || deviceOrientation > kOrientValidMax) {
        deviceOrientation = gOrientTracker.uiOrientation;
    }

    if (deviceOrientation != atomic_load(&gOrientation.deviceSeen)) {
        atomic_store(&gOrientation.deviceSeen, deviceOrientation);
        LOG(@"device orientation -> %ld (ui %ld, forced %ld)", deviceOrientation, gOrientTracker.uiOrientation,
            atomic_load(&gOrientation.deviceForced));
        CoordOrientationChanged();
    }

    OrientTestFaultCheck();
}

#define kOrientTrackIntervalS 0.5   // the tracker's polling timer
static void OrientUIUnwatch(void);

static void OrientTrackRunMain(void) {   // main: start tracking now if it isn't running yet, else refresh once
    if (gOrientTracker.running) {
        OrientUpdate();
        return;
    }
    gOrientTracker.running = YES;
    OrientUIUnwatch();   // the snapshot's observers (below): the tracker owns the value now
    LOG(@"motion: tracker starts; ui orientation at load: %s, now %ld",
        gOrientTracker.uiSnapshot.note[0] ? gOrientTracker.uiSnapshot.note : "not yet read",
        gOrientTracker.uiOrientation);
    [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
    OrientTestEarlyFault(kOrientFaultRunMain, kOrientFaultEnsure, "OrientTrackRunMain");
    OrientUpdate();
    [NSTimer scheduledTimerWithTimeInterval:kOrientTrackIntervalS
                                    repeats:YES
                                      block:^(NSTimer *timer) {
                                          OrientUpdate();
                                      }];
}

static void OrientTrackRunMainF(void *ctx) {
    @autoreleasepool {
        if (!gOrientTracker.running) {
            OrientTrackRunMain();
        }
    }
}

// Started at most once, by whichever comes first; never blocks (of three threads making their first motion call at
// once, one wins).
static void OrientTrackStart(const char *startedBy) {
    if (atomic_load(&gOrientTracker.started) || atomic_exchange(&gOrientTracker.started, 1)) {
        return;
    }
    // Called from inside the stand-in's selectors (motion key set), but the tracker is camera code, so the key is
    // cleared. SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1 skips this clear.
    void *saved = SentinelFeatureClearUnless(gOffTrackStartKeyClear);
    LOG(@"motion: tracking started by %s (%s thread)", startedBy, pthread_main_np() ? "main" : "bg");
    OrientTestEarlyFault(kOrientFaultStart, kOrientFaultStart, "OrientTrackStart");
    // a first motion call on main gets the right gravity in its first sample
    if (pthread_main_np()) {
        OrientTrackRunMain();
    } else {
        dispatch_async_f(dispatch_get_main_queue(), NULL, OrientTrackRunMainF);
    }
    SentinelFeatureRestoreUnless(gOffTrackStartKeyClear, saved);
}

// ---- the UI orientation snapshot at load ----
// One main-queue block enqueued at load records the interface orientation where the coordinator getters read it
// (gOrientTracker.uiOrientation and gOrientation.ui), so a RotationCoordinator built on a camera queue while main is
// busy reads the right preview angle at once. The full tracker still starts at the first motion or camera call.
//   - With no window scene connected yet, the orientation is read at the first UISceneWillConnectNotification.
//   - Then that scene's effectiveGeometry (KVO-compliant) is watched for every change until the tracker starts, which
//     removes the observer: a landscape-only app turns 250-450 ms after its scene activates, after both reads above,
//     and an app that allows all orientations can turn again before its first camera call.
// Main queue only; never blocks. SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1 turns it off (the tracker alone).
@interface SimcamSceneGeometryObserver : NSObject
@property (weak) UIWindowScene *scene;
@end

static BOOL OrientUIRecord(UIWindowScene *scene, const char *how) {   // main: NO if the scene has no orientation yet
    long orientation = scene.effectiveGeometry.interfaceOrientation;
    if (orientation < kOrientValidMin || orientation > kOrientValidMax) {
        return NO;
    }
    size_t used = strlen(gOrientTracker.uiSnapshot.note);
    // joined as "<first>, then <next>": for example "1 (load), then 3 (geometry change)" or "no scene at load, then 1
    // (scene connection)"
    snprintf(gOrientTracker.uiSnapshot.note + used, sizeof gOrientTracker.uiSnapshot.note - used, "%s%ld (%s)",
             used ? ", then " : "", orientation, how);
    if (gOrientTracker.running) {
        return YES;   // the tracker owns the value once it runs
    }
    if (orientation !=
        gOrientTracker.uiOrientation) {   // CoordOrientationChanged does nothing unless ARMED with coordinators
        gOrientTracker.uiOrientation = orientation;
        atomic_store(&gOrientation.ui, orientation);
        CoordOrientationChanged();
    }
    return YES;
}

static void OrientUIUnwatch(void) {   // main
    if (gOrientTracker.uiSnapshot.sceneObserver) {
        [NSNotificationCenter.defaultCenter removeObserver:gOrientTracker.uiSnapshot.sceneObserver];
        gOrientTracker.uiSnapshot.sceneObserver = nil;
    }
    SimcamSceneGeometryObserver *observer = gOrientTracker.uiSnapshot.geometryObserver;
    gOrientTracker.uiSnapshot.geometryObserver = nil;
    if (observer) {
        UIWindowScene *scene = observer.scene;
        if (scene) {
            [scene removeObserver:observer
                       forKeyPath:@"effectiveGeometry"
                          context:&gOrientTracker.uiSnapshot.geometryObserver];
        }
    }
}

@implementation SimcamSceneGeometryObserver

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    if (context != &gOrientTracker.uiSnapshot.geometryObserver) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    if (!pthread_main_np() || gOrientTracker.uiSnapshot.geometryObserver != self) {
        return;
    }
    // a resize, not a turn: keep waiting
    if (((UIWindowScene *)object).effectiveGeometry.interfaceOrientation == gOrientTracker.uiOrientation) {
        return;
    }
    // every turn until the tracker starts (its start removes this observer)
    OrientUIRecord(object, "geometry change");
}
@end

static void OrientUIWatch(UIWindowScene *scene) {   // main: this scene's orientation changes, until the tracker starts
    if (gOrientTracker.uiSnapshot.geometryObserver || gOrientTracker.running) {
        return;
    }
    SimcamSceneGeometryObserver *observer = [SimcamSceneGeometryObserver new];
    observer.scene = scene;
    gOrientTracker.uiSnapshot.geometryObserver = observer;
    [scene addObserver:observer
            forKeyPath:@"effectiveGeometry"
               options:0
               context:&gOrientTracker.uiSnapshot.geometryObserver];
}

static void OrientUISnapF(void *ctx) {
    @autoreleasepool {
        if (gOrientTracker.running) {
            snprintf(gOrientTracker.uiSnapshot.note, sizeof gOrientTracker.uiSnapshot.note,
                     "skipped (tracker already running)");
            return;
        }
        UIApplication *app = UIApplication.sharedApplication;   // nil in a process that isn't a UIKit app (yet)
        for (UIScene *scene in app.connectedScenes) {
            if ([scene isKindOfClass:[UIWindowScene class]]) {
                if (!OrientUIRecord((UIWindowScene *)scene, "load")) {
                    snprintf(gOrientTracker.uiSnapshot.note, sizeof gOrientTracker.uiSnapshot.note, "none at load");
                }
                OrientUIWatch((UIWindowScene *)scene);
                return;
            }
        }
        snprintf(gOrientTracker.uiSnapshot.note, sizeof gOrientTracker.uiSnapshot.note, "no scene at load");
        gOrientTracker.uiSnapshot.sceneObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:UISceneWillConnectNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *note) {
                        if (!gOrientTracker.uiSnapshot.sceneObserver ||
                            ![note.object isKindOfClass:[UIWindowScene class]]) {
                            return;
                        }
                        [NSNotificationCenter.defaultCenter
                            removeObserver:gOrientTracker.uiSnapshot.sceneObserver];   // once
                        gOrientTracker.uiSnapshot.sceneObserver = nil;
                        OrientUIRecord(note.object, "scene connection");
                        OrientUIWatch(note.object);
                    }];
    }
}

static void OrientUISnapAtLoad(void) {   // core/lazy.inc's constructor: enqueue only
    if (gOffUIOrientationAtLoad) {
        snprintf(gOrientTracker.uiSnapshot.note, sizeof gOrientTracker.uiSnapshot.note,
                 "off (SIMCAM_OFF_UI_ORIENTATION_AT_LOAD)");
        return;
    }
    dispatch_async_f(dispatch_get_main_queue(), NULL, OrientUISnapF);
}

// the first trapped camera call (core/lazy.inc's once body): enqueue only, so it runs while PREPARE does
static void OrientTrackStartForCamera(void) {
    if (atomic_load(&gOrientTracker.started) || atomic_exchange(&gOrientTracker.started, 1)) {
        return;
    }
    dispatch_async_f(dispatch_get_main_queue(), NULL, OrientTrackRunMainF);
}
