// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): CoreMotion accelerometer stand-in.
// ============================================================================
// CoreMotion — CMMotionManager reports an accelerometer + device motion and
// delivers gravity matching the current UI orientation (portrait by default).
// Hooked: the 16 CMMotionManager selectors of kMotionSels (below).
// Data objects: subclasses of CMAccelerometerData (-acceleration), CMDeviceMotion (-gravity, -userAcceleration,
// -attitude, -magneticField) and CMAttitude (the attitude stand-in, below).
// ============================================================================
#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>
@interface SimcamAccelerometerData : CMAccelerometerData
@property CMAcceleration a;
@property NSTimeInterval ts;
@end
@implementation SimcamAccelerometerData
- (CMAcceleration)acceleration { return _a; }
- (NSTimeInterval)timestamp { return _ts; }
@end
// ---- the attitude stand-in ----
// The device is held still, upright in the current orientation, yaw 0, in CMAttitudeReferenceFrameXArbitraryZVertical.
// Apple's conventions, measured against real CMAttitude/CMDeviceMotion objects built through CoreMotion's own (private)
// initializers (macOS 26.6; fixtures/motion-probe -attitude 1 compares every sample with them in the simulator):
//   - quaternion q (x, y, z, w) rotates device coordinates into the reference frame (v_ref = q v_dev q*);
//   - rotationMatrix is the transpose of q's matrix (reference -> device), so gravity = R * (0, 0, -1) =
//     -(m13, m23, m33);
//   - Euler order: q = qz(yaw) * qx(pitch) * qy(roll); pitch = asin(m23), roll = atan2(-m13, m33),
//     yaw = atan2(-m21, m22);
//   - [a multiplyByInverseOfAttitude:b] sets a = b^-1 * a (it mutates a); -copy gives a new object;
//   - a CMDeviceMotion returns the same CMAttitude on every -attitude read (a multiply on it sticks); -copy gives a
//     new one.
//   - an archive of a CMAttitude holds the class name CMAttitude and four doubles
//     kCMAttitudeCodingKeyQX/QY/QZ/QW (below).
// CMAttitude has no public initializer, so SimcamAttitude subclasses it and overrides every method that reads
// CMAttitude's private storage (its _internal stays nil; CMAttitude's dealloc releases nil). Upright portrait is at
// gimbal lock
// (pitch +pi/2): the stand-in reports roll 0 and yaw 0 there; Apple's conversion of the same quaternion gives -pi, -pi
// (the same rotation).
static CMQuaternion MotionQuatMul(CMQuaternion a, CMQuaternion b) {   // Hamilton product a * b
    return (CMQuaternion){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
// reference -> device: the transpose of q's rotation matrix
static CMRotationMatrix MotionQuatMatrix(CMQuaternion q) {
    double x = q.x, y = q.y, z = q.z, w = q.w;
    return (CMRotationMatrix){1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                              2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                              2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}
@interface SimcamAttitude : CMAttitude {
  @public
    CMQuaternion _q;
    double _roll, _pitch, _yaw;
}
@end
@implementation SimcamAttitude
+ (BOOL)supportsSecureCoding { return YES; }
- (void)simcam_recomputeAngles {   // Euler angles from the matrix, as CoreMotion computes them
    CMRotationMatrix r = MotionQuatMatrix(_q);
    _pitch = asin(fmax(-1, fmin(1, r.m23)));
    _roll = atan2(-r.m13, r.m33);
    _yaw = atan2(-r.m21, r.m22);
}
- (CMQuaternion)quaternion { return _q; }
- (CMRotationMatrix)rotationMatrix { return MotionQuatMatrix(_q); }
- (double)roll { return _roll; }
- (double)pitch { return _pitch; }
- (double)yaw { return _yaw; }
- (void)setQuaternion:(CMQuaternion)q {   // CMAttitude's private setter, in case anything calls it
    _q = q;
    [self simcam_recomputeAngles];
}
- (void)multiplyByInverseOfAttitude:(CMAttitude *)b {   // nil: no change (Apple's segfaults; ours would zero _q)
    if (!b) return;
    CMQuaternion bq = b.quaternion;
    _q = MotionQuatMul((CMQuaternion){-bq.x, -bq.y, -bq.z, bq.w}, _q);
    [self simcam_recomputeAngles];
}
- (id)copyWithZone:(NSZone *)zone {
    SimcamAttitude *c = [SimcamAttitude new];
    c->_q = _q;
    c->_roll = _roll;
    c->_pitch = _pitch;
    c->_yaw = _yaw;
    return c;
}
- (BOOL)isEqual:(id)o {
    if (o == self) return YES;
    if (![o isKindOfClass:[CMAttitude class]]) return NO;
    CMQuaternion p = ((CMAttitude *)o).quaternion;
    return p.x == _q.x && p.y == _q.y && p.z == _q.z && p.w == _q.w;
}
- (NSUInteger)hash {   // the quaternion's bits (+ 0.0: -0 and 0 are equal under isEqual:, so they hash the same)
    double v[4] = {_q.x + 0.0, _q.y + 0.0, _q.z + 0.0, _q.w + 0.0};
    uint64_t h = 14695981039346656037ull, bits;   // FNV-1a
    for (int i = 0; i < 4; i++) {
        memcpy(&bits, &v[i], 8);
        h = (h ^ bits) * 1099511628211ull;
    }
    return (NSUInteger)h;
}
- (NSString *)description {
    return [NSString stringWithFormat:@"CMAttitude Pitch: %f, Roll: %f, Yaw: %f", _pitch * 180 / M_PI,
                                      _roll * 180 / M_PI, _yaw * 180 / M_PI];
}
// Coding: archived as a plain CMAttitude under CoreMotion's own keys, so an app that saves a reference
// attitude decodes it later without the engine (a real CMAttitude, from CMAttitude's own -initWithCoder:). Measured on
// macOS 26.6: CMAttitude encodes four doubles under kCMAttitudeCodingKeyQX/QY/QZ/QW, and this archive is byte-identical
// to Apple's for the same quaternion (the probe asserts it in the simulator: coding.apple). With the stand-in active,
// MotionInstallHooks maps the name CMAttitude to this class for every decode in the process, so a decoded attitude is
// a SimcamAttitude, never a real CMAttitude next to stand-ins (probe: coding.decode, m1.*).
- (Class)classForCoder { return [CMAttitude class]; }
- (Class)classForKeyedArchiver { return [CMAttitude class]; }
- (void)encodeWithCoder:(NSCoder *)c {
    [c encodeDouble:_q.x forKey:@"kCMAttitudeCodingKeyQX"];
    [c encodeDouble:_q.y forKey:@"kCMAttitudeCodingKeyQY"];
    [c encodeDouble:_q.z forKey:@"kCMAttitudeCodingKeyQZ"];
    [c encodeDouble:_q.w forKey:@"kCMAttitudeCodingKeyQW"];
}
- (instancetype)initWithCoder:(NSCoder *)c {   // CoreMotion's keys: an archive of a real CMAttitude or of this class
    if (!(self = [super init])) return nil;
    _q = (CMQuaternion){[c decodeDoubleForKey:@"kCMAttitudeCodingKeyQX"],
                        [c decodeDoubleForKey:@"kCMAttitudeCodingKeyQY"],
                        [c decodeDoubleForKey:@"kCMAttitudeCodingKeyQZ"],
                        [c decodeDoubleForKey:@"kCMAttitudeCodingKeyQW"]};
    [self simcam_recomputeAngles];
    return self;
}
@end
// held still, upright in the orientation that gives gravity g, yaw 0: q = qx(pitch) * qy(roll)
static SimcamAttitude *MotionAttitudeFor(CMAcceleration g) {
    SimcamAttitude *a = [SimcamAttitude new];
    double n = sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
    if (n < 1e-9) {
        a->_q = (CMQuaternion){0, 0, 0, 1};
        return a;
    }
    double gx = g.x / n + 0.0, gy = g.y / n, gz = g.z / n;   // + 0.0: no -0 into atan2
    double pitch = asin(fmax(-1, fmin(1, -gy))), roll = atan2(gx, -gz + 0.0);
    double sp = sin(pitch / 2), cp = cos(pitch / 2), sr = sin(roll / 2), cr = cos(roll / 2);
    a->_q = (CMQuaternion){sp * cr, cp * sr, sp * sr, cp * cr};
    a->_pitch = pitch;   // stored, not recomputed: upright portrait is at gimbal lock (above)
    a->_roll = roll;
    a->_yaw = 0;
    return a;
}
@interface SimcamDeviceMotion : CMDeviceMotion
@property CMAcceleration g;
@property NSTimeInterval ts;
@property SimcamAttitude *att;
@end
@implementation SimcamDeviceMotion
- (CMAcceleration)gravity { return _g; }
- (CMAcceleration)userAcceleration { return (CMAcceleration){0, 0, 0}; }
- (CMRotationRate)rotationRate { return (CMRotationRate){0, 0, 0}; }
- (NSTimeInterval)timestamp { return _ts; }
- (double)heading { return -1; }
- (CMAttitude *)attitude { return _att; }   // one object per sample, as CoreMotion does
- (CMCalibratedMagneticField)magneticField {
    return (CMCalibratedMagneticField){{0, 0, 0}, CMMagneticFieldCalibrationAccuracyUncalibrated};
}
- (CMDeviceMotionSensorLocation)sensorLocation { return CMDeviceMotionSensorLocationDefault; }
- (id)copyWithZone:(NSZone *)zone {
    SimcamDeviceMotion *c = [SimcamDeviceMotion new];
    c.g = _g;
    c.ts = _ts;
    c.att = [_att copy];
    return c;
}
@end

static volatile long gUIOrient = 1;   // UIInterfaceOrientation, written and read on the main thread only; off main read
                                      // gUIOrientA (OrientUIAny, providers/scene.inc), which every writer stores too
static void CoordOrientationChanged(void);
static CMAcceleration MotionGravity(void) {
    const char *override = getenv(ENV_GRAVITY);   // "x,y,z" override
    if (override) {
        CMAcceleration a;
        sscanf(override, "%lf,%lf,%lf", &a.x, &a.y, &a.z);
        return a;
    }
    // gravity follows the (virtual) device, not the UI (timer threads)
    switch (OrientVirtualOn() ? OrientDevice() : OrientUIAny()) {
        case 2: return (CMAcceleration){0, 1, 0};   // portraitUpsideDown
        case 3: return (CMAcceleration){-1, 0, 0};  // interface landscapeRight == device landscapeLeft
        case 4: return (CMAcceleration){1, 0, 0};   // interface landscapeLeft == device landscapeRight
        default: return (CMAcceleration){0, -1, 0}; // portrait
    }
}
static const void *kTimerKey = &kTimerKey, *kMTimerKey = &kMTimerKey;
static void OrientTrackStart(const char *by);
// The test faults' crash: a write through a null pointer the compiler can't see (atoi("0")). Always inlined, so the
// fault stays in the function that calls it (noinline, in our image).
static inline __attribute__((always_inline)) void TestFaultNullWriteInline(void) {
    volatile int *p = (volatile int *)(uintptr_t)atoi("0");
    *p = 1;
}
// SIMCAM_TEST_MOTION_FAULT=1 (activation.sh motion-crash): a fault in our image inside a motion timer tick, 1 s after
// the first tick
static _Atomic uint64_t gMotionFirstTick;
__attribute__((noinline)) static void MotionTestFaultCheck(void) {
    if (!gTestMotionFault) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), zero = 0;
    if (atomic_compare_exchange_strong(&gMotionFirstTick, &zero, now)) return;
    if (now - atomic_load(&gMotionFirstTick) < NSEC_PER_SEC) return;
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_MOTION_FAULT: fault in a motion tick now\n", getpid());
    TestFaultNullWriteInline();
}
// every tick runs as a motion entry point (the feature key, core/safety.inc), whatever the tick block does
static dispatch_source_t MotionTimer(id mgr, double interval, void (^tick)(void)) {
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
    dispatch_source_set_timer(t, DISPATCH_TIME_NOW, (uint64_t)(MAX(interval, 0.01) * NSEC_PER_SEC), NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{
        SC_MOTION_SCOPE;
        MotionTestFaultCheck();
        tick();
    });
    dispatch_resume(t);
    return t;
}
// the operation block that calls the app's handler: a motion entry point, with the key cleared around the app's
// handler, so a fault in app code never counts (design §5.4)
#define MOTION_CALL_APP(call)                   \
    do {                                    \
        SC_MOTION_SCOPE;                    \
        void *_app = SCFeatEnter(NULL);     \
        call;                               \
        SCFeatLeave(_app);                  \
    } while (0)
// a message inside a motion scope that app code can answer (a CMMotionManager subclass's interval getter, an
// NSOperationQueue subclass's addOperationWithBlock:, an app block's copy): the key is cleared around it, like around
// the app's handler too, so a crash in our image reached from that app code is never `motion`. Guard: activation.sh
// app-send-crash (an interval getter that makes the first camera call; SIMCAM_TEST_TRAP_FAULT);
// SIMCAM_OFF_APP_SEND_KEY_CLEAR=1 (ledger L65) keeps the key in all three (MOTION_APP_SEND, MOTION_APP_DO,
// MotionEnqueue)
#define MOTION_APP_SEND(expr)                                                  \
    ({                                                                     \
        void *_appS = gOffAppSendKeyClear ? NULL : SCFeatEnter(NULL);       \
        __auto_type _r = (expr);                                           \
        if (!gOffAppSendKeyClear) SCFeatLeave(_appS);                       \
        _r;                                                                \
    })
#define MOTION_APP_DO(stmt)                                                    \
    do {                                                                   \
        void *_appS = gOffAppSendKeyClear ? NULL : SCFeatEnter(NULL);       \
        stmt;                                                              \
        if (!gOffAppSendKeyClear) SCFeatLeave(_appS);                       \
    } while (0)
static SimcamAccelerometerData *MotionMakeAccelerometerData(void) {
    SimcamAccelerometerData *d = [SimcamAccelerometerData new];
    d.a = MotionGravity();
    d.ts = [NSProcessInfo processInfo].systemUptime;
    return d;
}
static SimcamDeviceMotion *MotionMakeDeviceMotion(void) {
    SimcamDeviceMotion *d = [SimcamDeviceMotion new];
    d.g = MotionGravity();
    d.att = MotionAttitudeFor(d.g);
    d.ts = [NSProcessInfo processInfo].systemUptime;
    return d;
}
static void MotionStopTimer(id mgr, const void *key) {
    dispatch_source_t t = objc_getAssociatedObject(mgr, key);
    if (t) dispatch_source_cancel(t);
    objc_setAssociatedObject(mgr, key, nil, OBJC_ASSOCIATION_RETAIN);
}
static const void *kFrameKey = &kFrameKey;   // the reference frame of the last device-motion start (NSNumber)
static void MotionSetFrame(id s, CMAttitudeReferenceFrame f) {
    objc_setAssociatedObject(s, kFrameKey, @(f), OBJC_ASSOCIATION_RETAIN);
}
static void MotionEnqueue(NSOperationQueue *q, void (^op)(void)) { MOTION_APP_DO([q addOperationWithBlock:op]); }

// availability queries never start orientation tracking; starts and data calls do
// Every hooked selector is a motion entry point (SC_MOTION_SCOPE: the feature key says `motion` on this thread until it
// returns).
static BOOL hook_motionAvailable(id s, SEL c) {
    SC_MOTION_SCOPE;
    return YES;
}
static BOOL hook_isAccelerometerActive(id s, SEL c) {
    SC_MOTION_SCOPE;
    return objc_getAssociatedObject(s, kTimerKey) != nil;
}
static BOOL hook_isDeviceMotionActive(id s, SEL c) {
    SC_MOTION_SCOPE;
    return objc_getAssociatedObject(s, kMTimerKey) != nil;
}
static id hook_accelerometerData(id s, SEL c) {
    SC_MOTION_SCOPE;
    OrientTrackStart("accelerometerData");
    return hook_isAccelerometerActive(s, c) ? MotionMakeAccelerometerData() : nil;
}
static id hook_deviceMotion(id s, SEL c) {
    SC_MOTION_SCOPE;
    OrientTrackStart("deviceMotion");
    return hook_isDeviceMotionActive(s, c) ? MotionMakeDeviceMotion() : nil;
}
static void hook_startAccelerometerUpdates(id s, SEL c) {
    SC_MOTION_SCOPE;
    OrientTrackStart("startAccelerometerUpdates");
    MotionStopTimer(s, kTimerKey);
    objc_setAssociatedObject(s, kTimerKey, MotionTimer(s, 1, ^{}), OBJC_ASSOCIATION_RETAIN);
}
static void hook_startAccelerometerUpdatesToQueue(id s, SEL c, NSOperationQueue *q, CMAccelerometerHandler h) {
    SC_MOTION_SCOPE;
    OrientTrackStart("startAccelerometerUpdatesToQueue:withHandler:");
    MotionStopTimer(s, kTimerKey);
    double interval = MOTION_APP_SEND([(CMMotionManager *)s accelerometerUpdateInterval]);
    LOG(@"CMMotionManager startAccelerometerUpdatesToQueue (interval %.3f) gravity portrait-ish", interval);
    MOTION_APP_DO(h = [h copy]);
    objc_setAssociatedObject(s, kTimerKey, MotionTimer(s, interval, ^{
                                 SimcamAccelerometerData *d = MotionMakeAccelerometerData();
                                 MotionEnqueue(q, ^{ MOTION_CALL_APP(h(d, nil)); });
                             }),
                             OBJC_ASSOCIATION_RETAIN);
}
static void hook_stopAccelerometerUpdates(id s, SEL c) {
    SC_MOTION_SCOPE;
    MotionStopTimer(s, kTimerKey);
}
static void MotionStartDeviceMotionPull(id s, CMAttitudeReferenceFrame f, const char *by) {
    OrientTrackStart(by);
    MotionStopTimer(s, kMTimerKey);
    MotionSetFrame(s, f);
    objc_setAssociatedObject(s, kMTimerKey, MotionTimer(s, 1, ^{}), OBJC_ASSOCIATION_RETAIN);
}
static void hook_startDeviceMotionUpdates(id s, SEL c) {
    SC_MOTION_SCOPE;
    MotionStartDeviceMotionPull(s, CMAttitudeReferenceFrameXArbitraryZVertical, "startDeviceMotionUpdates");
}
static void hook_startDeviceMotionUpdatesUsingReferenceFrame(id s, SEL c, CMAttitudeReferenceFrame f) {
    SC_MOTION_SCOPE;
    LOG(@"CMMotionManager startDeviceMotionUpdatesUsingReferenceFrame:%lu (pull)", (unsigned long)f);
    MotionStartDeviceMotionPull(s, f, "startDeviceMotionUpdatesUsingReferenceFrame:");
}
static void MotionStartDeviceMotionPush(id s, CMAttitudeReferenceFrame f, NSOperationQueue *q, CMDeviceMotionHandler h,
                        const char *by) {
    OrientTrackStart(by);
    MotionStopTimer(s, kMTimerKey);
    MotionSetFrame(s, f);
    double interval = MOTION_APP_SEND([(CMMotionManager *)s deviceMotionUpdateInterval]);
    LOG(@"CMMotionManager %s (interval %.3f, reference frame %lu)", by, interval, (unsigned long)f);
    MOTION_APP_DO(h = [h copy]);
    objc_setAssociatedObject(s, kMTimerKey, MotionTimer(s, interval, ^{
                                 SimcamDeviceMotion *d = MotionMakeDeviceMotion();
                                 MotionEnqueue(q, ^{ MOTION_CALL_APP(h(d, nil)); });
                             }),
                             OBJC_ASSOCIATION_RETAIN);
}
static void hook_startDeviceMotionUpdatesToQueue(id s, SEL c, NSOperationQueue *q, CMDeviceMotionHandler h) {
    SC_MOTION_SCOPE;
    MotionStartDeviceMotionPush(s, CMAttitudeReferenceFrameXArbitraryZVertical, q, h,
                                "startDeviceMotionUpdatesToQueue:withHandler:");
}
// any requested frame gets the same samples: a still device at yaw 0 (only xArbitraryZVertical is advertised, below)
static void hook_startDeviceMotionUpdatesUsingReferenceFrameToQueue(id s, SEL c, CMAttitudeReferenceFrame f,
                                                                    NSOperationQueue *q, CMDeviceMotionHandler h) {
    SC_MOTION_SCOPE;
    MotionStartDeviceMotionPush(s, f, q, h, "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:");
}
// +availableAttitudeReferenceFrames (a class method): the stand-in has no magnetometer and no drift correction
static NSUInteger hook_availableAttitudeReferenceFrames(id cls, SEL c) {
    SC_MOTION_SCOPE;
    return CMAttitudeReferenceFrameXArbitraryZVertical;
}
// -attitudeReferenceFrame: the last start's frame, xArbitraryZVertical before any start
static NSUInteger hook_attitudeReferenceFrame(id s, SEL c) {
    SC_MOTION_SCOPE;
    NSNumber *f = objc_getAssociatedObject(s, kFrameKey);
    return f ? f.unsignedIntegerValue : CMAttitudeReferenceFrameXArbitraryZVertical;
}
static void hook_stopDeviceMotionUpdates(id s, SEL c) {
    SC_MOTION_SCOPE;
    MotionStopTimer(s, kMTimerKey);
}

// Two independent parts:
//   MotionInstallHooks   the stand-in's 16 selector hooks, at load (SIMCAM_OFF_MOTION=1 keeps CoreMotion stock).
//                   A failed or latched camera activation never removes them mid-run (there is no uninstall).
//                   Decided once at load from markers only (core/markers.inc SCMotionStockAtLoad): a camera latch, a
//                   motion latch or a real preflight failure of this build makes CoreMotion stock from the next
//                   launch, with one-shot forwarding traps (MotionInstallStockTraps) that say so once.
//   OrientTrack   the main-queue orientation tracker (upd + 0.5 s timer + device-orientation notifications): a camera
//                   dependency too (rotation coordinator angles, scene orientation), so it starts at the first motion
//                   start or data call, or at the first trapped camera call, whichever comes first, also with
//                   SIMCAM_OFF_MOTION=1.
static int gMotionN;
static double gMotionAt;   // when the hooks went in at load (0 = not installed)
// the 16 CMMotionManager selectors: the stand-in hooks them (MotionInstallHooks), a motion-stock launch traps them
// (MotionInstallStockTraps). A leading '+' is a class method. The last three are the attitude stand-in's.
static const char *const kMotionSels[] = {
    "isAccelerometerAvailable", "isDeviceMotionAvailable", "isAccelerometerActive", "isDeviceMotionActive",
    "accelerometerData", "deviceMotion",
    "startAccelerometerUpdates", "startAccelerometerUpdatesToQueue:withHandler:", "stopAccelerometerUpdates",
    "startDeviceMotionUpdates", "startDeviceMotionUpdatesToQueue:withHandler:",
    "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:",
    "stopDeviceMotionUpdates", "startDeviceMotionUpdatesUsingReferenceFrame:", "attitudeReferenceFrame",
    "+availableAttitudeReferenceFrames"};
static Method MotionMethod(Class C, const char *name, SEL *sel) {   // "+sel" looks on the metaclass
    BOOL meta = name[0] == '+';
    *sel = sel_registerName(meta ? name + 1 : name);
    return class_getInstanceMethod(meta ? object_getClass(C) : C, *sel);
}
static Class MotionClass(void) {
    Class C = objc_getClass("CMMotionManager");
    if (!C) {
        dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW);
        C = objc_getClass("CMMotionManager");
    }
    if (!C) LOG(@"MISMATCH: no CMMotionManager");
    return C;
}
static void MotionInstallHooks(void) {
    static int done;
    if (done) return;
    done = 1;
    Class C = MotionClass();
    if (!C) return;
    const IMP imps[] = {(IMP)hook_motionAvailable, (IMP)hook_motionAvailable, (IMP)hook_isAccelerometerActive,
                        (IMP)hook_isDeviceMotionActive, (IMP)hook_accelerometerData, (IMP)hook_deviceMotion,
                        (IMP)hook_startAccelerometerUpdates, (IMP)hook_startAccelerometerUpdatesToQueue,
                        (IMP)hook_stopAccelerometerUpdates, (IMP)hook_startDeviceMotionUpdates,
                        (IMP)hook_startDeviceMotionUpdatesToQueue,
                        (IMP)hook_startDeviceMotionUpdatesUsingReferenceFrameToQueue, (IMP)hook_stopDeviceMotionUpdates,
                        (IMP)hook_startDeviceMotionUpdatesUsingReferenceFrame, (IMP)hook_attitudeReferenceFrame,
                        (IMP)hook_availableAttitudeReferenceFrames};
    _Static_assert(sizeof imps / sizeof *imps == sizeof kMotionSels / sizeof *kMotionSels, "one IMP per selector");
    int n = 0;
    for (size_t i = 0; i < sizeof kMotionSels / sizeof *kMotionSels; i++) {
        SEL sel;
        Method m = MotionMethod(C, kMotionSels[i], &sel);
        if (m) {
            method_setImplementation(m, imps[i]);
            n++;
        } else {
            LOG(@"motion: missing %s", kMotionSels[i]);
        }
    }
    gMotionN = n;
    gMotionAt = CFAbsoluteTimeGetCurrent();
    // While the stand-in is active, every attitude decoded in the process is a SimcamAttitude. An archive of the
    // stand-in is Apple's format (class name CMAttitude), so without this it decodes to a real CMAttitude, and Apple's
    // methods read the ARGUMENT's private storage. Measured in the 27.0 and 26.3 sims (motion-probe -m1): [decoded
    // multiplyByInverseOfAttitude:dm.attitude] segfaults in CoreMotion (address 0x8), and [decoded isEqual:
    // dm.attitude] answers NO for the same quaternion. The archive stays Apple's (encodeWithCoder: above), so an app
    // that decodes it with the engine off gets Apple's class. Not done when CoreMotion is stock (this function
    // doesn't run).
    if (n) [NSKeyedUnarchiver setClass:[SimcamAttitude class] forClassName:@"CMAttitude"];
    LOG(@"motion: hooked %d CMMotionManager selectors; archived CMAttitudes decode as the stand-in", n);
}
// ---- CoreMotion stock from a marker (no silent stock, design §5.1) ----
// The same 16 selectors get one-shot forwarding traps instead of the stand-in: each call does one atomic load; the
// first one in the process queues the stock line on the record queue (core/lazy.inc ActMotionStockNote), then every
// call forwards to the saved original IMP. All 16 take <= 3 integer/pointer arguments and return an integer, a BOOL, an
// object or nothing, so the void * trampoline of the camera traps (ActInstallTrap) fits them too. Not installed under
// SIMCAM_OFF_MOTION=1 (the user asked).
static void ActMotionStockNote(void);
static _Atomic int gMotionStockNoted;
static int gMotionStockN;
static void MotionInstallStockTraps(void) {
    static int done;
    if (done) return;
    done = 1;
    Class C = MotionClass();
    if (!C) return;
    for (size_t i = 0; i < sizeof kMotionSels / sizeof *kMotionSels; i++) {
        SEL s;
        Method m = MotionMethod(C, kMotionSels[i], &s);
        if (!m) {
            LOG(@"motion: missing %s", kMotionSels[i]);
            continue;
        }
        IMP orig = method_getImplementation(m);
        IMP rep = imp_implementationWithBlock(^void *(void *self_, void *a, void *b, void *c2) {
            if (!atomic_load_explicit(&gMotionStockNoted, memory_order_relaxed) &&
                !atomic_exchange(&gMotionStockNoted, 1))
                ActMotionStockNote();
            return ((void *(*)(void *, SEL, void *, void *, void *))orig)(self_, s, a, b, c2);
        });
        method_setImplementation(m, rep);
        gMotionStockN++;
    }
    gMotionAt = CFAbsoluteTimeGetCurrent();
    LOG(@"motion: CoreMotion stock this launch (%s); %d one-shot forwarding traps", gSCMotionStockAt, gMotionStockN);
}
// ---- the orientation tracker (main queue only) ----
// The tracker is camera code (a camera dependency: rotation coordinators, scene orientation), not a motion entry
// point: upd fires KVO on the coordinators, and their observers call our camera getters (hook_coordinatorPreviewAngle,
// hook_coordinatorCaptureAngle) on this thread. So upd runs with the feature key CLEARED (saved and restored, like the
// app's handler): a fault in it is `provider` (or an inflight op's feature) and closes the camera, never `motion` (as
// `motion` it only stocked CoreMotion, and the camera kept crashing every launch). SIMCAM_OFF_TRACKER_KEY_CLEAR=1
// (ledger L61) brings back the motion key here.
static NSHashTable *CoordTable(void);
// SIMCAM_TEST_ORIENT_FAULT=1: once ARMED with a coordinator, upd fires the coordinators' KVO, then faults in our image
__attribute__((noinline)) static void OrientTestFaultCheck(void) {
    if (gTestOrientFault != ORF_UPD || !ActIsArmed()) return;
    NSHashTable *coords = CoordTable();
    NSUInteger n = 0;
    if (coords) {
        @synchronized (coords) { n = coords.count; }
    }
    if (!n) return;
    CoordOrientationChanged();
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_ORIENT_FAULT: fault in the orientation tracker's upd now\n", getpid());
    TestFaultNullWriteInline();
}
// SIMCAM_TEST_ORIENT_FAULT=start|runmain|ensure: a fault outside upd, once per process; waits up to 5 s for ARMED (the
// crash handler)
__attribute__((noinline)) static void OrientTestEarlyFault(int a, int b, const char *where) {
    static _Atomic int once;
    if ((gTestOrientFault != a && gTestOrientFault != b) || atomic_exchange(&once, 1)) return;
    for (int i = 0; i < 1000 && !ActIsArmed(); i++) usleep(5000);
    if (!ActIsArmed()) {
        fprintf(stderr, "[simcam %d] SIMCAM_TEST_ORIENT_FAULT: not armed after 5 s; no fault in %s\n", getpid(), where);
        return;
    }
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_ORIENT_FAULT: fault in %s now (%s thread)\n", getpid(), where,
            pthread_main_np() ? "main" : "bg");
    TestFaultNullWriteInline();
}
static void OrientUpdate(void) {
    void *_scSaved __attribute__((cleanup(SCFeatLeaveP), unused)) =
        SCFeatEnter(gOffTrackerKeyClear ? kSCMotion : NULL);
    for (UIScene *sc in UIApplication.sharedApplication.connectedScenes)
        if ([sc isKindOfClass:[UIWindowScene class]]) {
            long o = ((UIWindowScene *)sc).effectiveGeometry.interfaceOrientation;
            if (o != gUIOrient) {
                gUIOrient = o;
                atomic_store(&gUIOrientA, o);
                CoordOrientationChanged();
            }
            break;
        }
    // device orientation (simulator rotation, or UIDevice set by our control file); falls back to the UI orientation
    long dev = UIDevice.currentDevice.orientation;
    if (dev < 1 || dev > 4) dev = gUIOrient;
    if (dev != atomic_load(&gDevOrientSeen)) {
        atomic_store(&gDevOrientSeen, dev);
        LOG(@"device orientation -> %ld (ui %ld, forced %ld)", dev, gUIOrient, atomic_load(&gDevOrientForced));
        CoordOrientationChanged();
    }
    OrientTestFaultCheck();
}
static BOOL gTrackRunning;   // main queue only
// main queue only (OrientUISnapF, below): how the snapshot went, logged when the tracker starts
static char gUIOrientSnap[256];
static id gUIOrientSnapObs;      // main queue only: the one-shot scene-connection observer
static void OrientUIUnwatch(void);
static void OrientTrackRunMain(void) {   // main: start tracking now if it isn't running yet, else refresh once
    if (gTrackRunning) {
        OrientUpdate();
        return;
    }
    gTrackRunning = YES;
    OrientUIUnwatch();   // the snapshot's observers (below): the tracker owns the value now
    LOG(@"motion: tracker starts; ui orientation at load: %s, now %ld",
        gUIOrientSnap[0] ? gUIOrientSnap : "not yet read", gUIOrient);
    [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
    OrientTestEarlyFault(ORF_RUNMAIN, ORF_ENSURE, "OrientTrackRunMain");
    OrientUpdate();
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) { OrientUpdate(); }];
}
static void OrientTrackRunMainF(void *ctx) {
    @autoreleasepool {
        if (!gTrackRunning) OrientTrackRunMain();
    }
}
static _Atomic int gTrackStarted;
// started at most once, by whichever comes first; never blocks (three threads making their first motion call at once:
// one wins)
static void OrientTrackStart(const char *by) {
    if (atomic_load(&gTrackStarted) || atomic_exchange(&gTrackStarted, 1)) return;
    // called from inside the stand-in's selectors (motion key set): the tracker is camera code.
    // SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1 (ledger L62) skips this clear.
    void *saved = gOffTrackStartKeyClear ? NULL : SCFeatEnter(NULL);
    LOG(@"motion: tracking started by %s (%s thread)", by, pthread_main_np() ? "main" : "bg");
    OrientTestEarlyFault(ORF_START, ORF_START, "OrientTrackStart");
    // a first motion call on main gets the right gravity in its first sample
    if (pthread_main_np()) OrientTrackRunMain();
    else dispatch_async_f(dispatch_get_main_queue(), NULL, OrientTrackRunMainF);
    if (!gOffTrackStartKeyClear) SCFeatLeave(saved);
}
// ---- the UI orientation snapshot at load (ledger L57) ----
// One main-queue block enqueued at load records the interface orientation where the coordinator getters read it
// (gUIOrient/gUIOrientA), so a RotationCoordinator built on a camera queue while main is busy reads the right preview
// angle at once. No timer, no device-orientation notifications, no camera call: the full tracker still starts at the
// first motion or camera call.
//   - no window scene connected yet when the block runs: read at the first UISceneWillConnectNotification (that scene),
//     then remove the observer;
//   - then watch that scene's effectiveGeometry (KVO, documented as KVO-compliant) for EVERY change of interface
//     orientation until the tracker starts: a UI that launches in portrait and turns to the root view controller's
//     landscape-only mask does so 250-450 ms after the scene activates (measured, 26.3 and 27.0), after the load read
//     and the scene-connection read; and an app that allows all orientations can turn again (back to portrait) before
//     its first camera call. Each change records the new value; the tracker's start removes the observer (it owns the
//     value from then on). A scene that never changes keeps one idle KVO observer.
// Main queue only; never blocks. SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1 turns it off (the tracker alone).
@interface SimcamSceneGeometryObserver : NSObject
@property (weak) UIWindowScene *scene;
@end
static SimcamSceneGeometryObserver *gUIOrientGeomObs;   // main queue only
static BOOL OrientUIRecord(UIWindowScene *ws, const char *how) {   // main: NO if the scene has no orientation yet
    long o = ws.effectiveGeometry.interfaceOrientation;
    if (o < 1 || o > 4) return NO;
    size_t n = strlen(gUIOrientSnap);
    // joined as "<first>, then <next>": e.g. "1 (load), then 3 (geometry change)" or "no scene at load, then 1 (scene
    // connection)"
    snprintf(gUIOrientSnap + n, sizeof gUIOrientSnap - n, "%s%ld (%s)", n ? ", then " : "", o, how);
    if (gTrackRunning) return YES;   // the tracker owns the value once it runs
    if (o != gUIOrient) {   // no-op unless ARMED with coordinators
        gUIOrient = o;
        atomic_store(&gUIOrientA, o);
        CoordOrientationChanged();
    }
    return YES;
}
static void OrientUIUnwatch(void) {   // main
    if (gUIOrientSnapObs) {
        [NSNotificationCenter.defaultCenter removeObserver:gUIOrientSnapObs];
        gUIOrientSnapObs = nil;
    }
    SimcamSceneGeometryObserver *obs = gUIOrientGeomObs;
    gUIOrientGeomObs = nil;
    if (obs) {
        UIWindowScene *ws = obs.scene;
        if (ws) [ws removeObserver:obs forKeyPath:@"effectiveGeometry" context:&gUIOrientGeomObs];
    }
}
@implementation SimcamSceneGeometryObserver
- (void)observeValueForKeyPath:(NSString *)kp ofObject:(id)o change:(NSDictionary *)ch context:(void *)ctx {
    if (ctx != &gUIOrientGeomObs) {
        [super observeValueForKeyPath:kp ofObject:o change:ch context:ctx];
        return;
    }
    if (!pthread_main_np() || gUIOrientGeomObs != self) return;
    // a resize, not a turn: keep waiting
    if (((UIWindowScene *)o).effectiveGeometry.interfaceOrientation == gUIOrient) return;
    OrientUIRecord(o, "geometry change");   // every turn until the tracker starts (its start removes this observer)
}
@end
static void OrientUIWatch(UIWindowScene *ws) {   // main: this scene's orientation changes, until the tracker starts
    if (gUIOrientGeomObs || gTrackRunning) return;
    SimcamSceneGeometryObserver *obs = [SimcamSceneGeometryObserver new];
    obs.scene = ws;
    gUIOrientGeomObs = obs;
    [ws addObserver:obs forKeyPath:@"effectiveGeometry" options:0 context:&gUIOrientGeomObs];
}
static void OrientUISnapF(void *ctx) {
    @autoreleasepool {
        if (gTrackRunning) {
            snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "skipped (tracker already running)");
            return;
        }
        UIApplication *app = UIApplication.sharedApplication;   // nil in a process that isn't a UIKit app (yet)
        for (UIScene *sc in app.connectedScenes)
            if ([sc isKindOfClass:[UIWindowScene class]]) {
                if (!OrientUIRecord((UIWindowScene *)sc, "load"))
                    snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "none at load");
                OrientUIWatch((UIWindowScene *)sc);
                return;
            }
        snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "no scene at load");
        gUIOrientSnapObs = [NSNotificationCenter.defaultCenter addObserverForName:UISceneWillConnectNotification
                                                                           object:nil
                                                                            queue:nil
                                                                       usingBlock:^(NSNotification *n) {
            if (!gUIOrientSnapObs || ![n.object isKindOfClass:[UIWindowScene class]]) return;
            [NSNotificationCenter.defaultCenter removeObserver:gUIOrientSnapObs];   // once
            gUIOrientSnapObs = nil;
            OrientUIRecord(n.object, "scene connection");
            OrientUIWatch(n.object);
        }];
    }
}
static void OrientUISnapAtLoad(void) {   // core/lazy.inc's constructor: enqueue only
    if (gOffUIOrientationAtLoad) {
        snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "off (SIMCAM_OFF_UI_ORIENTATION_AT_LOAD)");
        return;
    }
    dispatch_async_f(dispatch_get_main_queue(), NULL, OrientUISnapF);
}
// the first trapped camera call (core/lazy.inc's once body): enqueue only, so it runs while prepare does
static void OrientTrackStartForCamera(void) {
    if (atomic_load(&gTrackStarted) || atomic_exchange(&gTrackStarted, 1)) return;
    dispatch_async_f(dispatch_get_main_queue(), NULL, OrientTrackRunMainF);
}
