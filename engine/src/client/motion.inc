// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): CoreMotion accelerometer stand-in.
// ============================================================================
// m5: CoreMotion — CMMotionManager reports an accelerometer + device motion and
// delivers gravity matching the current UI orientation (portrait by default).
// Hooked: -isAccelerometerAvailable -isDeviceMotionAvailable -isAccelerometerActive -isDeviceMotionActive
//         -startAccelerometerUpdates -startAccelerometerUpdatesToQueue:withHandler: -stopAccelerometerUpdates -accelerometerData
//         -startDeviceMotionUpdates -startDeviceMotionUpdatesToQueue:withHandler: -startDeviceMotionUpdatesUsingReferenceFrame:
//         -startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler: -stopDeviceMotionUpdates -deviceMotion
//         -attitudeReferenceFrame +availableAttitudeReferenceFrames
// Data objects: subclasses of CMAccelerometerData (-acceleration), CMDeviceMotion (-gravity, -userAcceleration, -attitude,
// -magneticField) and CMAttitude (the attitude stand-in, below).
// ============================================================================
#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>
@interface M5Accel : CMAccelerometerData @property CMAcceleration a; @property NSTimeInterval ts; @end
@implementation M5Accel - (CMAcceleration)acceleration { return _a; } - (NSTimeInterval)timestamp { return _ts; } @end
// ---- the attitude stand-in (W4 e29) ----
// The device is held still, upright in the current orientation, yaw 0, in CMAttitudeReferenceFrameXArbitraryZVertical.
// Apple's conventions, measured against real CMAttitude/CMDeviceMotion objects built through CoreMotion's own (private)
// initializers (macOS 26.6; fixtures/motion-probe -attitude 1 compares every sample with them in the simulator):
//   - quaternion q (x, y, z, w) rotates device coordinates into the reference frame (v_ref = q v_dev q*);
//   - rotationMatrix is the transpose of q's matrix (reference -> device), so gravity = R * (0, 0, -1) = -(m13, m23, m33);
//   - Euler order: q = qz(yaw) * qx(pitch) * qy(roll); pitch = asin(m23), roll = atan2(-m13, m33), yaw = atan2(-m21, m22);
//   - [a multiplyByInverseOfAttitude:b] sets a = b^-1 * a (it mutates a); -copy gives a new object;
//   - a CMDeviceMotion returns the same CMAttitude on every -attitude read (a multiply on it sticks); -copy gives a new one.
//   - an archive of a CMAttitude holds the class name CMAttitude and four doubles kCMAttitudeCodingKeyQX/QY/QZ/QW (below).
// CMAttitude has no public initializer, so M5Attitude subclasses it and overrides every method that reads CMAttitude's
// private storage (its _internal stays nil; CMAttitude's dealloc releases nil). Upright portrait is at gimbal lock (pitch
// +pi/2): the stand-in reports roll 0 and yaw 0 there; Apple's conversion of the same quaternion gives -pi, -pi (the same
// rotation).
static CMQuaternion M5QMul(CMQuaternion a, CMQuaternion b) {   // Hamilton product a * b
    return (CMQuaternion){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static CMRotationMatrix M5QMatrix(CMQuaternion q) {   // reference -> device: the transpose of q's rotation matrix
    double x = q.x, y = q.y, z = q.z, w = q.w;
    return (CMRotationMatrix){1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                              2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                              2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}
@interface M5Attitude : CMAttitude { @public CMQuaternion _q; double _roll, _pitch, _yaw; } @end
@implementation M5Attitude
+ (BOOL)supportsSecureCoding { return YES; }
- (void)m5Recompute {   // Euler angles from the matrix, as CoreMotion computes them
    CMRotationMatrix r = M5QMatrix(_q);
    _pitch = asin(fmax(-1, fmin(1, r.m23))); _roll = atan2(-r.m13, r.m33); _yaw = atan2(-r.m21, r.m22);
}
- (CMQuaternion)quaternion { return _q; }
- (CMRotationMatrix)rotationMatrix { return M5QMatrix(_q); }
- (double)roll { return _roll; }
- (double)pitch { return _pitch; }
- (double)yaw { return _yaw; }
- (void)setQuaternion:(CMQuaternion)q { _q = q; [self m5Recompute]; }   // CMAttitude's private setter, in case anything calls it
- (void)multiplyByInverseOfAttitude:(CMAttitude *)b {   // nil: no change (Apple's segfaults; ours would zero _q)
    if (!b) return;
    CMQuaternion bq = b.quaternion; _q = M5QMul((CMQuaternion){-bq.x, -bq.y, -bq.z, bq.w}, _q); [self m5Recompute];
}
- (id)copyWithZone:(NSZone *)zone { M5Attitude *c = [M5Attitude new]; c->_q = _q; c->_roll = _roll; c->_pitch = _pitch; c->_yaw = _yaw; return c; }
- (BOOL)isEqual:(id)o {
    if (o == self) return YES;
    if (![o isKindOfClass:[CMAttitude class]]) return NO;
    CMQuaternion p = ((CMAttitude *)o).quaternion; return p.x == _q.x && p.y == _q.y && p.z == _q.z && p.w == _q.w;
}
- (NSUInteger)hash {   // the quaternion's bits (+ 0.0: -0 and 0 are equal under isEqual:, so they hash the same)
    double v[4] = {_q.x + 0.0, _q.y + 0.0, _q.z + 0.0, _q.w + 0.0}; uint64_t h = 14695981039346656037ull, b;
    for (int i = 0; i < 4; i++) { memcpy(&b, &v[i], 8); h = (h ^ b) * 1099511628211ull; }
    return (NSUInteger)h;
}
- (NSString *)description { return [NSString stringWithFormat:@"CMAttitude Pitch: %f, Roll: %f, Yaw: %f", _pitch * 180 / M_PI, _roll * 180 / M_PI, _yaw * 180 / M_PI]; }
// Coding (review, L5): archived as a plain CMAttitude under CoreMotion's own keys, so an app that saves a reference
// attitude decodes it later without the engine (a real CMAttitude, from CMAttitude's own -initWithCoder:). Measured on macOS
// 26.6: CMAttitude encodes four doubles under kCMAttitudeCodingKeyQX/QY/QZ/QW, and this archive is byte-identical to Apple's
// for the same quaternion (the probe asserts it in the simulator: coding.apple). With the stand-in active, M5MotionHooks maps
// the name CMAttitude to this class for every decode in the process (review round 2 M1), so a decoded attitude is an
// M5Attitude, never a real CMAttitude next to stand-ins (probe: coding.decode, m1.*).
- (Class)classForCoder { return [CMAttitude class]; }
- (Class)classForKeyedArchiver { return [CMAttitude class]; }
- (void)encodeWithCoder:(NSCoder *)c {
    [c encodeDouble:_q.x forKey:@"kCMAttitudeCodingKeyQX"]; [c encodeDouble:_q.y forKey:@"kCMAttitudeCodingKeyQY"];
    [c encodeDouble:_q.z forKey:@"kCMAttitudeCodingKeyQZ"]; [c encodeDouble:_q.w forKey:@"kCMAttitudeCodingKeyQW"];
}
- (instancetype)initWithCoder:(NSCoder *)c {   // CoreMotion's keys: an archive of a real CMAttitude or of this class
    if (!(self = [super init])) return nil;
    _q = (CMQuaternion){[c decodeDoubleForKey:@"kCMAttitudeCodingKeyQX"], [c decodeDoubleForKey:@"kCMAttitudeCodingKeyQY"],
                        [c decodeDoubleForKey:@"kCMAttitudeCodingKeyQZ"], [c decodeDoubleForKey:@"kCMAttitudeCodingKeyQW"]};
    [self m5Recompute]; return self;
}
@end
// held still, upright in the orientation that gives gravity g, yaw 0: q = qx(pitch) * qy(roll)
static M5Attitude *M5AttitudeFor(CMAcceleration g) {
    M5Attitude *a = [M5Attitude new];
    double n = sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
    if (n < 1e-9) { a->_q = (CMQuaternion){0, 0, 0, 1}; return a; }
    double gx = g.x / n + 0.0, gy = g.y / n, gz = g.z / n;   // + 0.0: no -0 into atan2
    double p = asin(fmax(-1, fmin(1, -gy))), r = atan2(gx, -gz + 0.0);
    double sp = sin(p / 2), cp = cos(p / 2), sr = sin(r / 2), cr = cos(r / 2);
    a->_q = (CMQuaternion){sp * cr, cp * sr, sp * sr, cp * cr};
    a->_pitch = p; a->_roll = r; a->_yaw = 0;   // stored, not recomputed: upright portrait is at gimbal lock (above)
    return a;
}
@interface M5Motion : CMDeviceMotion @property CMAcceleration g; @property NSTimeInterval ts; @property M5Attitude *att; @end
@implementation M5Motion - (CMAcceleration)gravity { return _g; } - (CMAcceleration)userAcceleration { return (CMAcceleration){0, 0, 0}; }
- (CMRotationRate)rotationRate { return (CMRotationRate){0, 0, 0}; } - (NSTimeInterval)timestamp { return _ts; } - (double)heading { return -1; }
- (CMAttitude *)attitude { return _att; }   // one object per sample, as CoreMotion does
- (CMCalibratedMagneticField)magneticField { return (CMCalibratedMagneticField){{0, 0, 0}, CMMagneticFieldCalibrationAccuracyUncalibrated}; }
- (CMDeviceMotionSensorLocation)sensorLocation { return CMDeviceMotionSensorLocationDefault; }
- (id)copyWithZone:(NSZone *)zone { M5Motion *c = [M5Motion new]; c.g = _g; c.ts = _ts; c.att = [_att copy]; return c; }
@end

static volatile long gUIOrient = 1;   // UIInterfaceOrientation, written and read on the main thread only; off main read
                                      // gUIOrientA (M5UIOrientAny, providers/scene.inc), which every writer stores too
static void M5CoordOrientationChanged(void);
static CMAcceleration M5Gravity(void) {
    const char *o = getenv("M5_GRAVITY"); // "x,y,z" override
    if (o) { CMAcceleration a; sscanf(o, "%lf,%lf,%lf", &a.x, &a.y, &a.z); return a; }
    switch (M12OrientOn() ? M12DevOrient() : M5UIOrientAny()) {   // m12: gravity follows the (virtual) device, not the UI (timer threads)
        case 2: return (CMAcceleration){0, 1, 0};   // portraitUpsideDown
        case 3: return (CMAcceleration){-1, 0, 0};  // interface landscapeRight == device landscapeLeft
        case 4: return (CMAcceleration){1, 0, 0};   // interface landscapeLeft == device landscapeRight
        default: return (CMAcceleration){0, -1, 0}; // portrait
    }
}
static const void *kTimerKey = &kTimerKey, *kMTimerKey = &kMTimerKey;
static void M5OrientTrackStart(const char *by);
// M16_MOTION_FAULT=1 (W4 PR B, motion-crash): a fault in our image inside a motion timer tick, 1 s after the first tick
static _Atomic uint64_t gM5FirstTick;
__attribute__((noinline)) static void M5MotionFaultCheck(void) {
    if (!gM16MotionFault) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), z = 0;
    if (atomic_compare_exchange_strong(&gM5FirstTick, &z, now)) return;
    if (now - atomic_load(&gM5FirstTick) < NSEC_PER_SEC) return;
    fprintf(stderr, "[simcam %d] M16_MOTION_FAULT: fault in a motion tick now\n", getpid());
    volatile int *p = (volatile int *)(uintptr_t)atoi("0"); *p = 1;
}
// every tick runs as a motion entry point (the feature key, core/safety.inc), whatever the tick block does
static dispatch_source_t M5MotionTimer(id mgr, double interval, void (^tick)(void)) {
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
    dispatch_source_set_timer(t, DISPATCH_TIME_NOW, (uint64_t)(MAX(interval, 0.01) * NSEC_PER_SEC), NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{ SC_MOTION_SCOPE; M5MotionFaultCheck(); tick(); }); dispatch_resume(t); return t;
}
// the operation block that calls the app's handler: a motion entry point, with the key cleared around the app's handler, so
// a fault in app code never counts (design §5.4)
#define M5_CALL_APP(call) do { SC_MOTION_SCOPE; void *_app = SCFeatEnter(NULL); call; SCFeatLeave(_app); } while (0)
// a message inside a motion scope that app code can answer (a CMMotionManager subclass's interval getter, an NSOperationQueue
// subclass's addOperationWithBlock:, an app block's copy): the key is cleared around it, like around the app's handler
// (review round 2, low 3), so a crash in our image reached from that app code is never `motion`. Guard: activation.sh
// app-send-crash (an interval getter that makes the first camera call; M16_TRAP_FAULT); M17_APP_SEND_KEEP_KEY=1 (ledger L65)
// keeps the key in all three (M5_APP_SEND, M5_APP_DO, M5Enqueue)
#define M5_APP_SEND(expr) ({ void *_appS = gM17AppSendKeepKey ? NULL : SCFeatEnter(NULL); __auto_type _r = (expr); if (!gM17AppSendKeepKey) SCFeatLeave(_appS); _r; })
#define M5_APP_DO(stmt) do { void *_appS = gM17AppSendKeepKey ? NULL : SCFeatEnter(NULL); stmt; if (!gM17AppSendKeepKey) SCFeatLeave(_appS); } while (0)
static M5Accel *M5MakeAccel(void) { M5Accel *d = [M5Accel new]; d.a = M5Gravity(); d.ts = [NSProcessInfo processInfo].systemUptime; return d; }
static M5Motion *M5MakeMotion(void) { M5Motion *d = [M5Motion new]; d.g = M5Gravity(); d.att = M5AttitudeFor(d.g); d.ts = [NSProcessInfo processInfo].systemUptime; return d; }
static void M5StopTimer(id mgr, const void *key) { dispatch_source_t t = objc_getAssociatedObject(mgr, key); if (t) dispatch_source_cancel(t); objc_setAssociatedObject(mgr, key, nil, OBJC_ASSOCIATION_RETAIN); }
static const void *kFrameKey = &kFrameKey;   // the reference frame of the last device-motion start (NSNumber)
static void M5SetFrame(id s, CMAttitudeReferenceFrame f) { objc_setAssociatedObject(s, kFrameKey, @(f), OBJC_ASSOCIATION_RETAIN); }
static void M5Enqueue(NSOperationQueue *q, void (^op)(void)) { M5_APP_DO([q addOperationWithBlock:op]); }

// availability queries never start orientation tracking; starts and data calls do (W4, plan §3)
// Every hooked selector is a motion entry point (SC_MOTION_SCOPE: the feature key says `motion` on this thread until it returns).
static BOOL m5_yes(id s, SEL c) { SC_MOTION_SCOPE; return YES; }
static BOOL m5_accActive(id s, SEL c) { SC_MOTION_SCOPE; return objc_getAssociatedObject(s, kTimerKey) != nil; }
static BOOL m5_dmActive(id s, SEL c) { SC_MOTION_SCOPE; return objc_getAssociatedObject(s, kMTimerKey) != nil; }
static id m5_accData(id s, SEL c) { SC_MOTION_SCOPE; M5OrientTrackStart("accelerometerData"); return m5_accActive(s, c) ? M5MakeAccel() : nil; }
static id m5_dmData(id s, SEL c) { SC_MOTION_SCOPE; M5OrientTrackStart("deviceMotion"); return m5_dmActive(s, c) ? M5MakeMotion() : nil; }
static void m5_accStartPull(id s, SEL c) { SC_MOTION_SCOPE; M5OrientTrackStart("startAccelerometerUpdates"); M5StopTimer(s, kTimerKey); objc_setAssociatedObject(s, kTimerKey, M5MotionTimer(s, 1, ^{}), OBJC_ASSOCIATION_RETAIN); }
static void m5_accStart(id s, SEL c, NSOperationQueue *q, CMAccelerometerHandler h) {
    SC_MOTION_SCOPE;
    M5OrientTrackStart("startAccelerometerUpdatesToQueue:withHandler:");
    M5StopTimer(s, kTimerKey);
    double iv = M5_APP_SEND([(CMMotionManager *)s accelerometerUpdateInterval]);
    LOG(@"CMMotionManager startAccelerometerUpdatesToQueue (interval %.3f) gravity portrait-ish", iv);
    M5_APP_DO(h = [h copy]);
    objc_setAssociatedObject(s, kTimerKey, M5MotionTimer(s, iv, ^{ M5Accel *d = M5MakeAccel(); M5Enqueue(q, ^{ M5_CALL_APP(h(d, nil)); }); }), OBJC_ASSOCIATION_RETAIN);
}
static void m5_accStop(id s, SEL c) { SC_MOTION_SCOPE; M5StopTimer(s, kTimerKey); }
static void m5_dmStartPullF(id s, CMAttitudeReferenceFrame f, const char *by) {
    M5OrientTrackStart(by); M5StopTimer(s, kMTimerKey); M5SetFrame(s, f);
    objc_setAssociatedObject(s, kMTimerKey, M5MotionTimer(s, 1, ^{}), OBJC_ASSOCIATION_RETAIN);
}
static void m5_dmStartPull(id s, SEL c) { SC_MOTION_SCOPE; m5_dmStartPullF(s, CMAttitudeReferenceFrameXArbitraryZVertical, "startDeviceMotionUpdates"); }
static void m5_dmStartRefPull(id s, SEL c, CMAttitudeReferenceFrame f) {
    SC_MOTION_SCOPE; LOG(@"CMMotionManager startDeviceMotionUpdatesUsingReferenceFrame:%lu (pull)", (unsigned long)f);
    m5_dmStartPullF(s, f, "startDeviceMotionUpdatesUsingReferenceFrame:");
}
static void m5_dmStartF(id s, CMAttitudeReferenceFrame f, NSOperationQueue *q, CMDeviceMotionHandler h, const char *by) {
    M5OrientTrackStart(by);
    M5StopTimer(s, kMTimerKey); M5SetFrame(s, f);
    double iv = M5_APP_SEND([(CMMotionManager *)s deviceMotionUpdateInterval]);
    LOG(@"CMMotionManager %s (interval %.3f, reference frame %lu)", by, iv, (unsigned long)f);
    M5_APP_DO(h = [h copy]);
    objc_setAssociatedObject(s, kMTimerKey, M5MotionTimer(s, iv, ^{ M5Motion *d = M5MakeMotion(); M5Enqueue(q, ^{ M5_CALL_APP(h(d, nil)); }); }), OBJC_ASSOCIATION_RETAIN);
}
static void m5_dmStart(id s, SEL c, NSOperationQueue *q, CMDeviceMotionHandler h) {
    SC_MOTION_SCOPE; m5_dmStartF(s, CMAttitudeReferenceFrameXArbitraryZVertical, q, h, "startDeviceMotionUpdatesToQueue:withHandler:");
}
// any requested frame gets the same samples: a still device at yaw 0 (only xArbitraryZVertical is advertised, below)
static void m5_dmStartRef(id s, SEL c, CMAttitudeReferenceFrame f, NSOperationQueue *q, CMDeviceMotionHandler h) {
    SC_MOTION_SCOPE; m5_dmStartF(s, f, q, h, "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:");
}
// +availableAttitudeReferenceFrames (a class method): the stand-in has no magnetometer and no drift correction
static NSUInteger m5_availFrames(id cls, SEL c) { SC_MOTION_SCOPE; return CMAttitudeReferenceFrameXArbitraryZVertical; }
// -attitudeReferenceFrame: the last start's frame, xArbitraryZVertical before any start
static NSUInteger m5_attFrame(id s, SEL c) { SC_MOTION_SCOPE; NSNumber *f = objc_getAssociatedObject(s, kFrameKey); return f ? f.unsignedIntegerValue : CMAttitudeReferenceFrameXArbitraryZVertical; }
static void m5_dmStop(id s, SEL c) { SC_MOTION_SCOPE; M5StopTimer(s, kMTimerKey); }

// W4: M5MotionInstall is split in two independent parts (plan §3):
//   M5MotionHooks   the stand-in's 16 selector hooks, at load (M5_NO_MOTION=1 keeps CoreMotion stock). A failed or latched
//                   camera activation never removes them mid-run (D2; M5MotionUninstall is gone). PR B: decided once at load
//                   from markers only (core/safety.inc SCMotionStockAtLoad): a camera latch, a motion latch or a real
//                   preflight failure of this build makes CoreMotion stock from the next launch, with one-shot forwarding
//                   traps (M5MotionStockTraps) that say so once.
//   M5OrientTrack   the main-queue orientation tracker (upd + 0.5 s timer + device-orientation notifications): a camera
//                   dependency too (rotation coordinator angles, scene orientation), so it starts at the first motion start or
//                   data call, or at the first trapped camera call, whichever comes first, also with M5_NO_MOTION=1.
static int gMotionN;
static double gMotionAt;   // when the hooks went in at load (0 = not installed)
// the 16 CMMotionManager selectors: the stand-in hooks them (M5MotionHooks), a motion-stock launch traps them (M5MotionStockTraps).
// A leading '+' is a class method. e29 added the last three (the attitude stand-in).
static const char *const kM5Sels[] = {
    "isAccelerometerAvailable", "isDeviceMotionAvailable", "isAccelerometerActive", "isDeviceMotionActive", "accelerometerData", "deviceMotion",
    "startAccelerometerUpdates", "startAccelerometerUpdatesToQueue:withHandler:", "stopAccelerometerUpdates",
    "startDeviceMotionUpdates", "startDeviceMotionUpdatesToQueue:withHandler:", "startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:",
    "stopDeviceMotionUpdates", "startDeviceMotionUpdatesUsingReferenceFrame:", "attitudeReferenceFrame", "+availableAttitudeReferenceFrames"};
static Method M5Method(Class C, const char *name, SEL *sel) {   // "+sel" looks on the metaclass
    BOOL meta = name[0] == '+'; *sel = sel_registerName(meta ? name + 1 : name);
    return class_getInstanceMethod(meta ? object_getClass(C) : C, *sel);
}
static Class M5MotionClass(void) {
    Class C = objc_getClass("CMMotionManager");
    if (!C) { dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW); C = objc_getClass("CMMotionManager"); }
    if (!C) LOG(@"MISMATCH: no CMMotionManager");
    return C;
}
static void M5MotionHooks(void) {
    static int done; if (done) return; done = 1;
    Class C = M5MotionClass(); if (!C) return;
    const IMP imps[] = {(IMP)m5_yes, (IMP)m5_yes, (IMP)m5_accActive, (IMP)m5_dmActive, (IMP)m5_accData, (IMP)m5_dmData,
        (IMP)m5_accStartPull, (IMP)m5_accStart, (IMP)m5_accStop, (IMP)m5_dmStartPull, (IMP)m5_dmStart, (IMP)m5_dmStartRef, (IMP)m5_dmStop,
        (IMP)m5_dmStartRefPull, (IMP)m5_attFrame, (IMP)m5_availFrames};
    _Static_assert(sizeof imps / sizeof *imps == sizeof kM5Sels / sizeof *kM5Sels, "one IMP per selector");
    int n = 0;
    for (size_t i = 0; i < sizeof kM5Sels / sizeof *kM5Sels; i++) {
        SEL sel; Method m = M5Method(C, kM5Sels[i], &sel);
        if (m) { method_setImplementation(m, imps[i]); n++; } else LOG(@"motion: missing %s", kM5Sels[i]);
    }
    gMotionN = n; gMotionAt = CFAbsoluteTimeGetCurrent();
    // review round 2 M1: while the stand-in is active, every attitude decoded in the process is an M5Attitude. An
    // archive of the stand-in is Apple's format (class name CMAttitude), so without this it decodes to a real CMAttitude,
    // and Apple's methods read the ARGUMENT's private storage. Measured in the 27.0 and 26.3 sims (motion-probe -m1):
    // [decoded multiplyByInverseOfAttitude:dm.attitude] segfaults in CoreMotion (address 0x8), and [decoded isEqual:
    // dm.attitude] answers NO for the same quaternion. The archive stays Apple's (encodeWithCoder: above), so an app that
    // decodes it with the engine off gets Apple's class. Not done when CoreMotion is stock (this function doesn't run).
    if (n) [NSKeyedUnarchiver setClass:[M5Attitude class] forClassName:@"CMAttitude"];
    LOG(@"motion: hooked %d CMMotionManager selectors; archived CMAttitudes decode as the stand-in", n);
}
// ---- CoreMotion stock from a marker (PR B, plan §2.6 B1: no silent stock) ----
// The same 16 selectors get one-shot forwarding traps instead of the stand-in: each call does one atomic load; the first one
// in the process queues the stock line on the record queue (core/lazy.inc ActMotionStockNote), then every call forwards to
// the saved original IMP. All 16 take <= 3 integer/pointer arguments and return an integer, a BOOL, an object or nothing, so
// the void * trampoline of the camera traps (m11_trap) fits them too. Not installed under M5_NO_MOTION=1 (the user asked).
static void ActMotionStockNote(void);
static _Atomic int gM5StockNoted;
static int gMotionStockN;
static void M5MotionStockTraps(void) {
    static int done; if (done) return; done = 1;
    Class C = M5MotionClass(); if (!C) return;
    for (size_t i = 0; i < sizeof kM5Sels / sizeof *kM5Sels; i++) {
        SEL s; Method m = M5Method(C, kM5Sels[i], &s);
        if (!m) { LOG(@"motion: missing %s", kM5Sels[i]); continue; }
        IMP orig = method_getImplementation(m);
        IMP rep = imp_implementationWithBlock(^void *(void *self_, void *a, void *b, void *c2) {
            if (!atomic_load_explicit(&gM5StockNoted, memory_order_relaxed) && !atomic_exchange(&gM5StockNoted, 1)) ActMotionStockNote();
            return ((void *(*)(void *, SEL, void *, void *, void *))orig)(self_, s, a, b, c2);
        });
        method_setImplementation(m, rep); gMotionStockN++;
    }
    gMotionAt = CFAbsoluteTimeGetCurrent();
    LOG(@"motion: CoreMotion stock this launch (%s); %d one-shot forwarding traps", gSCMotionStockAt, gMotionStockN);
}
// ---- the orientation tracker (main queue only) ----
// The tracker is camera code (a camera dependency since PR A: rotation coordinators, scene orientation), not a motion entry
// point: upd fires KVO on the coordinators, and their observers call our camera getters (m5_coordAngle, m12_coordCapAngle)
// on this thread. So upd runs with the feature key CLEARED (saved and restored, like the app's handler): a fault in it is
// `provider` (or an inflight op's feature) and closes the camera, never `motion` (review, the blocker: as `motion` it
// only stocked CoreMotion, and the camera kept crashing every launch). M17_TRACKER_MOTION_SCOPE=1 (ledger L61) brings back
// the motion key here.
static NSHashTable *M5Coords(void);
// M16_ORIENT_FAULT=1: once ARMED with a coordinator, upd fires the coordinators' KVO, then faults in our image
__attribute__((noinline)) static void M5OrientFaultCheck(void) {
    if (gM16OrientFault != ORF_UPD || !ActIsArmed()) return;
    NSHashTable *c = M5Coords(); NSUInteger n = 0; if (c) { @synchronized (c) { n = c.count; } }
    if (!n) return;
    M5CoordOrientationChanged();
    fprintf(stderr, "[simcam %d] M16_ORIENT_FAULT: fault in the orientation tracker's upd now\n", getpid());
    volatile int *p = (volatile int *)(uintptr_t)atoi("0"); *p = 1;
}
// M16_ORIENT_FAULT=start|runmain|ensure: a fault outside upd, once per process; waits up to 5 s for ARMED (the crash handler)
__attribute__((noinline)) static void M5OrientEarlyFault(int a, int b, const char *where) {
    static _Atomic int once;
    if ((gM16OrientFault != a && gM16OrientFault != b) || atomic_exchange(&once, 1)) return;
    for (int i = 0; i < 1000 && !ActIsArmed(); i++) usleep(5000);
    if (!ActIsArmed()) { fprintf(stderr, "[simcam %d] M16_ORIENT_FAULT: not armed after 5 s; no fault in %s\n", getpid(), where); return; }
    fprintf(stderr, "[simcam %d] M16_ORIENT_FAULT: fault in %s now (%s thread)\n", getpid(), where, pthread_main_np() ? "main" : "bg");
    volatile int *p = (volatile int *)(uintptr_t)atoi("0"); *p = 1;
}
static void M5OrientUpd(void) {
    void *_scSaved __attribute__((cleanup(SCFeatLeaveP), unused)) = SCFeatEnter(gM17TrackerMotionScope ? kSCMotion : NULL);
    for (UIScene *sc in UIApplication.sharedApplication.connectedScenes)
        if ([sc isKindOfClass:[UIWindowScene class]]) { long o = ((UIWindowScene *)sc).effectiveGeometry.interfaceOrientation; if (o != gUIOrient) { gUIOrient = o; atomic_store(&gUIOrientA, o); M5CoordOrientationChanged(); } break; }
    // m12: device orientation (simulator rotation, or UIDevice set by our control file); falls back to the UI orientation
    long d = UIDevice.currentDevice.orientation; if (d < 1 || d > 4) d = gUIOrient;
    if (d != atomic_load(&gDevOrientSeen)) { atomic_store(&gDevOrientSeen, d); LOG(@"m12 device orientation -> %ld (ui %ld, forced %ld)", d, gUIOrient, atomic_load(&gDevOrientForced)); M5CoordOrientationChanged(); }
    M5OrientFaultCheck();
}
static BOOL gTrackRunning;   // main queue only
static char gUIOrientSnap[256];   // main queue only (M5UIOrientSnapF, below): how the snapshot went, logged when the tracker starts
static id gUIOrientSnapObs;      // main queue only: the one-shot scene-connection observer
static void M5UIOrientUnwatch(void);
static void M5OrientTrackRunMain(void) {   // main: start tracking now if it isn't running yet, else refresh once
    if (gTrackRunning) { M5OrientUpd(); return; }
    gTrackRunning = YES;
    M5UIOrientUnwatch();   // the snapshot's observers (below): the tracker owns the value now
    LOG(@"motion: tracker starts; ui orientation at load: %s, now %ld", gUIOrientSnap[0] ? gUIOrientSnap : "not yet read", gUIOrient);
    [UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
    M5OrientEarlyFault(ORF_RUNMAIN, ORF_ENSURE, "M5OrientTrackRunMain");
    M5OrientUpd();
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) { M5OrientUpd(); }];
}
static void M5OrientTrackRunMainF(void *ctx) { @autoreleasepool { if (!gTrackRunning) M5OrientTrackRunMain(); } }
static _Atomic int gTrackStarted;
// started at most once, by whichever comes first; never blocks (three threads making their first motion call at once: one wins)
static void M5OrientTrackStart(const char *by) {
    if (atomic_load(&gTrackStarted) || atomic_exchange(&gTrackStarted, 1)) return;
    // called from inside the stand-in's selectors (motion key set): the tracker is camera code. M17_TRACKSTART_KEEP_KEY=1
    // (ledger L62) skips this clear.
    void *sv = gM17TrackStartKeepKey ? NULL : SCFeatEnter(NULL);
    LOG(@"motion: tracking started by %s (%s thread)", by, pthread_main_np() ? "main" : "bg");
    M5OrientEarlyFault(ORF_START, ORF_START, "M5OrientTrackStart");
    if (pthread_main_np()) M5OrientTrackRunMain();   // a first motion call on main gets the right gravity in its first sample
    else dispatch_async_f(dispatch_get_main_queue(), NULL, M5OrientTrackRunMainF);
    if (!gM17TrackStartKeepKey) SCFeatLeave(sv);
}
// ---- the UI orientation snapshot at load (W4 option (b), plan §8; ledger L57) ----
// One main-queue block enqueued at load records the interface orientation where the coordinator getters read it
// (gUIOrient/gUIOrientA), so a RotationCoordinator built on a camera queue while main is busy reads the right preview angle at
// once. No timer, no device-orientation notifications, no camera call: the full tracker still starts at the first motion or
// camera call, as before.
//   - no window scene connected yet when the block runs: read at the first UISceneWillConnectNotification (that scene), then
//     remove the observer;
//   - then watch that scene's effectiveGeometry (KVO, documented as KVO-compliant) for EVERY change of interface orientation
//     until the tracker starts: a UI that launches in portrait and turns to the root view controller's landscape-only mask
//     does so 250-450 ms after the scene activates (measured, 26.3 and 27.0), after the load read and the scene-connection
//     read; and an app that allows all orientations can turn again (back to portrait) before its first camera call. Each
//     change records the new value; the tracker's start removes the observer (it owns the value from then on). A scene that
//     never changes keeps one idle KVO observer.
// Main queue only; never blocks. M16_NO_UIORIENT_AT_LOAD=1 turns it off (the tracker alone, as before).
@interface M5SceneGeomObs : NSObject @property (weak) UIWindowScene *scene; @end
static M5SceneGeomObs *gUIOrientGeomObs;   // main queue only
static BOOL M5UIOrientRecord(UIWindowScene *ws, const char *how) {   // main: NO if the scene has no orientation yet
    long o = ws.effectiveGeometry.interfaceOrientation;
    if (o < 1 || o > 4) return NO;
    size_t n = strlen(gUIOrientSnap);
    // joined as "<first>, then <next>": e.g. "1 (load), then 3 (geometry change)" or "no scene at load, then 1 (scene connection)"
    snprintf(gUIOrientSnap + n, sizeof gUIOrientSnap - n, "%s%ld (%s)", n ? ", then " : "", o, how);
    if (gTrackRunning) return YES;   // the tracker owns the value once it runs
    if (o != gUIOrient) { gUIOrient = o; atomic_store(&gUIOrientA, o); M5CoordOrientationChanged(); }   // no-op unless ARMED with coordinators
    return YES;
}
static void M5UIOrientUnwatch(void) {   // main
    if (gUIOrientSnapObs) { [NSNotificationCenter.defaultCenter removeObserver:gUIOrientSnapObs]; gUIOrientSnapObs = nil; }
    M5SceneGeomObs *g = gUIOrientGeomObs; gUIOrientGeomObs = nil;
    if (g) { UIWindowScene *ws = g.scene; if (ws) [ws removeObserver:g forKeyPath:@"effectiveGeometry" context:&gUIOrientGeomObs]; }
}
@implementation M5SceneGeomObs
- (void)observeValueForKeyPath:(NSString *)kp ofObject:(id)o change:(NSDictionary *)ch context:(void *)ctx {
    if (ctx != &gUIOrientGeomObs) { [super observeValueForKeyPath:kp ofObject:o change:ch context:ctx]; return; }
    if (!pthread_main_np() || gUIOrientGeomObs != self) return;
    if (((UIWindowScene *)o).effectiveGeometry.interfaceOrientation == gUIOrient) return;   // a resize, not a turn: keep waiting
    M5UIOrientRecord(o, "geometry change");   // every turn until the tracker starts (its start removes this observer)
}
@end
static void M5UIOrientWatch(UIWindowScene *ws) {   // main: this scene's orientation changes, until the tracker starts
    if (gUIOrientGeomObs || gTrackRunning) return;
    M5SceneGeomObs *g = [M5SceneGeomObs new]; g.scene = ws; gUIOrientGeomObs = g;
    [ws addObserver:g forKeyPath:@"effectiveGeometry" options:0 context:&gUIOrientGeomObs];
}
static void M5UIOrientSnapF(void *ctx) {
    @autoreleasepool {
        if (gTrackRunning) { snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "skipped (tracker already running)"); return; }
        UIApplication *app = UIApplication.sharedApplication;   // nil in a process that isn't a UIKit app (yet)
        for (UIScene *sc in app.connectedScenes)
            if ([sc isKindOfClass:[UIWindowScene class]]) {
                if (!M5UIOrientRecord((UIWindowScene *)sc, "load")) snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "none at load");
                M5UIOrientWatch((UIWindowScene *)sc); return;
            }
        snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "no scene at load");
        gUIOrientSnapObs = [NSNotificationCenter.defaultCenter addObserverForName:UISceneWillConnectNotification object:nil queue:nil
                                                                     usingBlock:^(NSNotification *n) {
            if (!gUIOrientSnapObs || ![n.object isKindOfClass:[UIWindowScene class]]) return;
            [NSNotificationCenter.defaultCenter removeObserver:gUIOrientSnapObs]; gUIOrientSnapObs = nil;   // once
            M5UIOrientRecord(n.object, "scene connection");
            M5UIOrientWatch(n.object);
        }];
    }
}
static void M5UIOrientSnapAtLoad(void) {   // core/lazy.inc's constructor: enqueue only
    if (gM16NoUIOrientAtLoad) { snprintf(gUIOrientSnap, sizeof gUIOrientSnap, "off (M16_NO_UIORIENT_AT_LOAD)"); return; }
    dispatch_async_f(dispatch_get_main_queue(), NULL, M5UIOrientSnapF);
}
// the first trapped camera call (core/lazy.inc's once body): enqueue only, so it runs while prepare does
static void M5OrientTrackStartForCamera(void) {
    if (atomic_load(&gTrackStarted) || atomic_exchange(&gTrackStarted, 1)) return;
    dispatch_async_f(dispatch_get_main_queue(), NULL, M5OrientTrackRunMainF);
}
