// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): FigCaptureSession: per-session state and sink routing.
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <IOSurface/IOSurfaceRef.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>

static void *(*pCMNotificationCenterGetDefaultLocalCenter)(void);
static void *gSessDerived[64];
static void *gOrigDerived[64];
static BaseObjVTable gSessVTable;
static dispatch_queue_t gSessQ;      // notification posting (serial, ordered)
static dispatch_queue_t gPumpQ;      // frame pump + sinks
// Photo encode, preview image and completion, in request order, off the pump (providers/photo.inc). StopRunning and an
// interruption drain it before posting DidStopRunning, so a photo taken right before a stop still completes first.
static dispatch_queue_t PhotoQueue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        q = dispatch_queue_create("simcam.photo", DISPATCH_QUEUE_SERIAL);
    });
    return q;
}
#pragma pack(push, 1)
typedef struct { int32_t type; CFTypeRef obj; int32_t pad[2]; } LocalQueueMsg;
#pragma pack(pop)

static NSString *DescribeShort(id o, NSUInteger max) {
    NSString *d = [o description];
    return d.length > max ? [[d substringToIndex:max] stringByAppendingString:@"…"] : d;
}
static id SafeKVC(id o, NSString *k) {
    @try {
        return [o valueForKey:k];
    } @catch (NSException *e) {
        return nil;
    }
}
// iOS 26 carries rotation as degrees (outputRotationDegrees / videoRotationDegrees / rotationDegrees); iOS 17.5 carries
// an AVCaptureVideoOrientation-style enum (outputOrientation / videoOrientation / orientation: 1 portrait, 2
// upside-down, 3 landscapeRight, 4 landscapeLeft). The enum is the same for both cameras, but the degrees are not: for
// the same device orientation 26/27 send 0 (back) and 180 (front) in landscape, so the front camera swaps the two
// landscape values (17.5 front landscape photos/movies were 180° off, without this). position: AVCaptureDevicePosition
// of the active device (2 = front), 0 if unknown.
static int SettingsRotationDeg(id o, NSString *degKey, NSString *orientKey, long position) {
    id d = SafeKVC(o, degKey);
    if (d) return [d intValue];
    BOOL front = position == 2;
    switch ([SafeKVC(o, orientKey) intValue]) {
        case 1: return 90;
        case 2: return 270;
        case 3: return front ? 180 : 0;
        case 4: return front ? 0 : 180;
        default: return 0;
    }
}
static void DiagDumpProps(id o) {
    static NSMutableSet *seen;
    if (!seen) seen = [NSMutableSet set];
    Class c = [o class];
    NSString *className = NSStringFromClass(c);
    if ([seen containsObject:className]) return;
    [seen addObject:className];
    NSMutableArray *names = [NSMutableArray array];
    for (Class k = c; k && k != [NSObject class]; k = class_getSuperclass(k)) {
        unsigned n = 0;
        objc_property_t *props = class_copyPropertyList(k, &n);
        for (unsigned i = 0; i < n; i++) {
            NSString *propName = @(property_getName(props[i]));
            const char *attrs = property_getAttributes(props[i]);
            if (!attrs || !(attrs[1] == '@' || strchr("cCiIsSlLqQfdB", attrs[1]))) {
                [names addObject:[NSString stringWithFormat:@"%@=<%c>", propName, attrs ? attrs[1] : '?']];
                continue;
            }
            id v = SafeKVC(o, propName);
            [names addObject:[NSString stringWithFormat:@"%@=%@", propName, v ? DescribeShort(v, 80) : @"nil"]];
        }
        free(props);
    }
    LOG(@"PROPS %@: %@", className, [names componentsJoinedByString:@" | "]);
}

@interface SimcamSink : NSObject
@property (copy) NSString *kind, *sinkID;
@property (assign) CFTypeRef queue;
@property int w, h;
@property OSType fmt;
@property (assign) CVPixelBufferPoolRef pool;
@property (assign) VTPixelTransferSessionRef xfer;
@property float rotation;
@property (assign) CFTypeRef imageQueue;
@property (strong) NSMutableDictionary *regIDs;
@property int frames;
@property BOOL mirrored;
// data-output connection rotation/mirroring
@property int vdoRot;
@property (assign) VTPixelRotationSessionRef rotSess;
@property (assign) CVPixelBufferPoolRef rotPool;
@property int photoW, photoH;
// mirroring flip (MirrorFlip)
@property (assign) VTPixelRotationSessionRef flipSess;
@property int flipKind;
// webcam frame path: this sink's pool and last output (providers/webcam_path.inc, WcOutCache)
@property (assign) void *wcOut;
@end
// Ledger L75: the CF objects above are `assign` and made lazily by the frame path (video.inc, mirror.inc MirrorFlip); a
// sink lives as long as its session's wiring (PumpWireSinks makes new sinks on every commit, and every new session), so
// it releases them here. Without that, a video data output whose format/size needs a conversion leaks its pool +
// transfer session (~3 MB of pixel buffers and VT scaler state) per session; a plain app opening the camera 50 times
// would grow 3.1–3.2 MB per open. `queue` stays the wiring's (released there). Not released here: the preview sink's
// CAImageQueue and the IOSurfaces it registers (video.inc, only with the opt-in SIMCAM_PREVIEW_IMAGE_QUEUE path; the
// default posts surfaces directly) — TODO.
static void WcOutFree(void *p);   // providers/webcam_path.inc
@implementation SimcamSink
- (void)dealloc {
    if (_xfer) {
        VTPixelTransferSessionInvalidate(_xfer);
        CFRelease(_xfer);
    }
    if (_pool) CVPixelBufferPoolRelease(_pool);
    if (_rotSess) {
        VTPixelRotationSessionInvalidate(_rotSess);
        CFRelease(_rotSess);
    }
    if (_rotPool) CVPixelBufferPoolRelease(_rotPool);
    if (_flipSess) {
        VTPixelRotationSessionInvalidate(_flipSess);
        CFRelease(_flipSess);
    }
    WcOutFree(_wcOut);
}
@end

@interface SimcamRecording : NSObject
@property (copy) NSString *sinkID;
@property long long settingsID;
@property (strong) AVAssetWriter *writer;
@property (strong) AVAssetWriterInputPixelBufferAdaptor *adaptor;
@property CMTime start, last;
// stopping: no more appends (also set when startWriting fails); stopRequested: MovieStop ran
@property BOOL started, stopping, stopRequested;
@property (strong) NSURL *url;
@property int w, h;
@property (strong) AVAssetWriterInput *audioIn;
@property int audioBufs;
@property CMTime audioFirst;
@property BOOL audioDone;
@property (strong) SimcamSink *conv;   // movie at the active format size
@property BOOL mirror;
@property int rot;
// mirroring flip (MirrorFlip)
@property (assign) VTPixelRotationSessionRef flipSess;
@property int flipKind;
// webcam frame path: the movie's own pool and last frame (webcam_path.inc, WcMovieOut)
@property (assign) void *wcOut;
@end
// L75: the mirror flip session (audio.inc's append path) and the webcam pool; `conv` frees its own (SimcamSink above)
@implementation SimcamRecording
- (void)dealloc {
    if (_flipSess) {
        VTPixelRotationSessionInvalidate(_flipSess);
        CFRelease(_flipSess);
    }
    WcOutFree(_wcOut);
}
@end

@interface SimcamSession : NSObject
@property (assign) CMBaseObjectRef obj;
@property (strong) id config;
@property BOOL running;
@property (strong) NSMutableArray<SimcamSink *> *sinks;
@property (strong) dispatch_source_t timer;
@property (strong) NSMutableDictionary<NSString *, SimcamRecording *> *recs;
@property long long liveCID;
@property (strong) NSMutableSet<NSString *> *audioMovieSinks;
@property BOOL wantsMic, micOn;
// from the genuine device's activeFormat / activeVideoMinFrameDuration
@property int fmtW, fmtH;
@property double fps;
// device position, interruptions
@property long position;
@property BOOL interrupted;
@property int interruptReason;
// this session's serial stop queue (StopWaitQueue), made at its first deferred stop
@property (strong) dispatch_queue_t stopQ;
@end
@implementation SimcamSession @end
static NSMutableDictionary<NSValue *, SimcamSession *> *gSessions;
static SimcamSession *SessLookup(CMBaseObjectRef s) {
    @synchronized (gSessions) {
        return gSessions[[NSValue valueWithPointer:s]];
    }
}
static CMVideoDimensions DimsFromValue(id v) {
    CMVideoDimensions d = {0, 0};
    if ([v isKindOfClass:[NSValue class]] && strcmp([(NSValue *)v objCType], @encode(CMVideoDimensions)) == 0)
        [(NSValue *)v getValue:&d];
    return d;
}
static CMVideoDimensions gIrisMaxDims;

static void SessPost(CMBaseObjectRef s, NSString *name, NSDictionary *payload) {
    CFRetain(s);
    dispatch_async(gSessQ, ^{
        OSStatus e = pCMNotificationCenterPostNotification(pCMNotificationCenterGetDefaultLocalCenter(),
                                                           (__bridge CFStringRef)name, s,
                                                           (__bridge CFDictionaryRef)payload, 0);
        LOG(@"session posted %@ %@ -> %d", name, payload ? DescribeShort(payload, 400) : @"", (int)e);
        CFRelease(s);
    });
}

// ============================================================================
// Ledger L66: an in-flight photo's delegate callbacks must reach the app before the session's stop is reported.
// Draining the photo queue (sess_StopRunning) orders the Fig notifications, not what the app sees: AVF runs
// didFinishCapture with a dispatch_async to the delegate queue (main), while -[AVCaptureSession _stopFigCaptureSession]
// (waits up to 9 s on a condition) is woken by AVF's own DidStopRunning handler, which is not ordered with that block,
// so the stop can be reported first (17.5: 1 in 11 idle canary runs, 3 in 17 with main busy).
// So every photo is pending from IrisCaptureImage until AVF has run didFinishCaptureForResolvedSettings:
// (client/photo.inc marks it done on the delegate queue, after the callback). sess_StopRunning with photos of its
// session pending posts DidStopRunning from the session's serial stop queue (StopWaitQueue) once they are done, bounded
// by kStopWaitBoundS; AVF's stopRunning thread waits on its own condition meanwhile, holding none of its locks. A stop
// called on a delegate queue is not deferred (the callbacks it would wait for run after it returns). The main thread
// counts as a delegate queue unless delegate queues have been seen and none was main: a stopRunning in
// `sessionQueue.sync {}` from main runs on main under another queue, where dispatch_get_specific doesn't see main's
// mark.
// A pending entry leaves gStopWaitPending when its didFinishCapture runs (StopWaitPhotoDone), when a stop that waited
// for it times out (only photos pending when that stop came), or at session finalize (StopWaitForget). A photo whose
// didFinishCapture never comes stays until then: the next stop with it pending waits the full kStopWaitBoundS once and
// drops it.
// Restart: a startRunning while a deferred stop is outstanding would post DidStartRunning before that DidStopRunning,
// and AVF would take the session as stopped (defensive: AVF never forwards such a start on 17.5/26.3/27.0).
// sess_StartRunning first flushes it (StopWaitFlush): it marks each outstanding stop id, wakes the waiters and waits
// (kStopWaitBoundS) until each has queued its DidStopRunning (StopWaitEndStop). A mark lasts until its waiter ends,
// even if the start's wait timed out, so a late waiter ends at once; a stop that comes after the flush has a new id and
// isn't flushed by it. A custom serial queue targets an overcommit root queue, so a waiter gets a thread even when the
// app has used up GCD's pool (a concurrent queue doesn't, and could starve the flush); one per session, so a flush
// never waits behind another session's waiter.
// Bound: 500 ms. Normal waits are 1.4-10 ms (17.5); the SIMCAM_TEST_FINISH_DELAY_MS=300 control waits ~320 ms; a stop
// on a thread main is blocked on some other way hitches at most 500 ms.
// Off switches (post at once): SIMCAM_OFF_STOP_WAIT=1, SIMCAM_OFF_PHOTO_DRAIN=1 (ledger L51's mutant). Mutants:
// SIMCAM_OFF_FLUSH_MARK_KEPT (ledger L74), SIMCAM_OFF_FLUSH_WAIT. Test faults: SIMCAM_TEST_FINISH_DELAY_MS,
// SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS (client/photo.inc), SIMCAM_TEST_LATE_WAITER_MS, SIMCAM_TEST_FLUSH_RACE.
// ============================================================================
static const double kStopWaitBoundS = 0.5;
static NSCondition *gStopWaitCond;   // guards gStopWaitPending, gStopWaitStops, gStopWaitFlushed, gStopWaitStopGen
// settingsID -> session: IrisCaptureImage .. didFinishCapture ran
static NSMutableDictionary<NSNumber *, NSValue *> *gStopWaitPending;
// stop id -> session: deferred stops outstanding (their StopWaitQueue)
static NSMutableDictionary<NSNumber *, NSValue *> *gStopWaitStops;
static NSMutableSet<NSNumber *> *gStopWaitFlushed;   // of those, the stop ids a start flushed: stop waiting now
static long long gStopWaitStopGen;                   // the last stop id handed out
static _Atomic int gStopWaitHooked;                  // the client hook resolved, so something clears gStopWaitPending
static _Atomic int gStopWaitMainDelegateQ;           // bits: 1 a photo delegate queue was main, 2 another queue
static char gStopWaitDelegateQKey;                   // set on every photo delegate queue the hooks have seen
static void StopWaitInit(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        gStopWaitCond = [NSCondition new];
        gStopWaitPending = [NSMutableDictionary dictionary];
        gStopWaitStops = [NSMutableDictionary dictionary];
        gStopWaitFlushed = [NSMutableSet set];
    });
}
static void StopWaitPhotoBegin(CMBaseObjectRef s, long long sid) {
    if (!atomic_load(&gStopWaitHooked)) return;
    StopWaitInit();
    [gStopWaitCond lock];
    gStopWaitPending[@(sid)] = [NSValue valueWithPointer:s];
    [gStopWaitCond unlock];
}
static void StopWaitPhotoDone(long long sid) {
    if (!gStopWaitCond) return;
    [gStopWaitCond lock];
    if (gStopWaitPending[@(sid)]) {
        [gStopWaitPending removeObjectForKey:@(sid)];
        [gStopWaitCond broadcast];
    }
    [gStopWaitCond unlock];
}
static NSArray<NSNumber *> *StopWaitPendingFor_locked(CMBaseObjectRef s) {
    NSValue *v = [NSValue valueWithPointer:s];
    return [gStopWaitPending keysOfEntriesPassingTest:^BOOL(NSNumber *k, NSValue *o, BOOL *stop) {
        return [o isEqual:v];
    }].allObjects;
}
static BOOL StopWaitHasPending(CMBaseObjectRef s) {
    if (!gStopWaitCond) return NO;
    [gStopWaitCond lock];
    BOOL r = StopWaitPendingFor_locked(s).count > 0;
    [gStopWaitCond unlock];
    return r;
}
// a stop on this thread can't wait for photo callbacks: it runs on a marked delegate queue, or on the main thread while
// the main queue is (or, before any photo's queue is known, may be) a delegate queue
static BOOL StopWaitOnDelegateQueue(void) {
    if (dispatch_get_specific(&gStopWaitDelegateQKey)) return YES;
    int m = atomic_load(&gStopWaitMainDelegateQ);
    return pthread_main_np() && (m == 0 || (m & 1));
}
static NSArray<NSNumber *> *StopWaitStillPending_locked(NSArray<NSNumber *> *sids) {
    NSMutableArray *r = [NSMutableArray array];
    for (NSNumber *k in sids)
        if (gStopWaitPending[k]) [r addObject:k];
    return r;
}
// The photos of s pending now, and the stop that waits for them registered as outstanding under a new id (*stopId; its
// waiter ends it, StopWaitEndStop); nil (nothing registered) if none is pending.
static NSArray<NSNumber *> *StopWaitBeginStop(CMBaseObjectRef s, long long *stopId) {
    if (!gStopWaitCond) return nil;
    [gStopWaitCond lock];
    NSArray *r = StopWaitPendingFor_locked(s);
    if (r.count) {
        *stopId = ++gStopWaitStopGen;
        gStopWaitStops[@(*stopId)] = [NSValue valueWithPointer:s];
    } else r = nil;
    [gStopWaitCond unlock];
    return r;
}
static NSArray<NSNumber *> *StopWaitStopsOf_locked(CMBaseObjectRef s) {   // the ids of s's outstanding deferred stops
    NSValue *v = [NSValue valueWithPointer:s];
    return [gStopWaitStops keysOfEntriesPassingTest:^BOOL(NSNumber *k, NSValue *o, BOOL *stop) {
        return [o isEqual:v];
    }].allObjects;
}
// Waits until none of sids (the photos pending when the stop came) is pending, a start flushes the stop (stopId), or
// the deadline. At the deadline the ones left are dropped, so a photo whose didFinishCapture never comes
// (SIMCAM_OFF_RESPONSIVE_NOTE, an error path AVF finishes elsewhere) can't stall later stops; photos taken after that
// (after a restart) are kept. Returns how many were left; *flushed says whether a start ended the wait (or had flushed
// the stop before the waiter ran).
static NSUInteger StopWaitForPhotos(long long stopId, NSArray<NSNumber *> *sids, double budget, BOOL *flushed) {
    NSNumber *id_ = @(stopId);
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:budget];
    [gStopWaitCond lock];
    while (StopWaitStillPending_locked(sids).count && ![gStopWaitFlushed containsObject:id_] &&
           [gStopWaitCond waitUntilDate:until]) {}
    NSArray *left = StopWaitStillPending_locked(sids);
    *flushed = [gStopWaitFlushed containsObject:id_];
    if (!*flushed) [gStopWaitPending removeObjectsForKeys:left];   // flushed: their callbacks still come
    [gStopWaitCond unlock];
    return left.count;
}
// the waiter, once its DidStopRunning is queued on gSessQ: the stop isn't outstanding any more (a start waiting in
// StopWaitFlush goes on)
// deferred stops whose DidStopRunning is queued (SIMCAM_TEST_FLUSH_RACE reads it)
static _Atomic long gStopWaitStopsPosted;
static void StopWaitEndStop(long long stopId) {
    atomic_fetch_add(&gStopWaitStopsPosted, 1);
    [gStopWaitCond lock];
    [gStopWaitStops removeObjectForKey:@(stopId)];
    [gStopWaitFlushed removeObject:@(stopId)];   // its mark ends with it (not when a start's wait times out)
    [gStopWaitCond broadcast];
    [gStopWaitCond unlock];
}
static void StopWaitForget(CMBaseObjectRef s) {   // session finalize
    if (!gStopWaitCond) return;
    [gStopWaitCond lock];
    [gStopWaitPending removeObjectsForKeys:StopWaitPendingFor_locked(s)];
    [gStopWaitCond broadcast];
    [gStopWaitCond unlock];
}
// s's serial stop queue (serial, one per session; see above). A session the engine doesn't know (not expected) gets a
// shared serial one.
static dispatch_queue_t StopWaitQueue(CMBaseObjectRef s) {
    static dispatch_queue_t shared;
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        shared = dispatch_queue_create("simcam.stop", DISPATCH_QUEUE_SERIAL);
    });
    SimcamSession *S = SessLookup(s);
    if (!S) return shared;
    @synchronized (S) {
        if (!S.stopQ) S.stopQ = dispatch_queue_create("simcam.stop.session", DISPATCH_QUEUE_SERIAL);
        return S.stopQ;
    }
}
// sess_StartRunning: a deferred stop of s still outstanding is reported now, before the start: each such stop's waiter
// is told to stop waiting, and this returns once their DidStopRunning are queued (StopWaitEndStop), bounded. Returns
// whether one was outstanding.
static BOOL StopWaitFlush(CMBaseObjectRef s) {
    if (!gStopWaitCond) return NO;
    BOOL old = getenv(ENV_OFF_FLUSH_WAIT) != NULL, oldMark = getenv(ENV_OFF_FLUSH_MARK_KEPT) != NULL;
    NSUInteger late = 0;
    [gStopWaitCond lock];
    NSArray<NSNumber *> *ids = StopWaitStopsOf_locked(s);
    BOOL out = ids.count > 0;
    if (out) {
        [gStopWaitFlushed addObjectsFromArray:ids];
        [gStopWaitCond broadcast];
        if (!old) {
            // a waiter sees its mark as soon as it runs (its session's serial queue holds only flushed waiters before
            // it), so kStopWaitBoundS is ample. On timeout the marks stay until each waiter ends, so a late one ends
            // at once
            NSDate *until = [NSDate dateWithTimeIntervalSinceNow:kStopWaitBoundS];
            NSUInteger (^left)(void) = ^NSUInteger {
                NSUInteger n = 0;
                for (NSNumber *k in ids) n += gStopWaitStops[k] != nil;
                return n;
            };
            while (left() && [gStopWaitCond waitUntilDate:until]) {}
            late = left();
            // SIMCAM_OFF_FLUSH_MARK_KEPT (L74): the mutant's clearing
            if (late && oldMark) [gStopWaitFlushed minusSet:[NSSet setWithArray:ids]];
        }
    }
    [gStopWaitCond unlock];
    // SIMCAM_OFF_FLUSH_WAIT: the naive flush (returns before a waiter not yet queued)
    if (out && old) dispatch_sync(StopWaitQueue(s), ^{});
    if (late)
        LOG(@"session %p StartRunning: %lu deferred stop(s) still outstanding after %.1f s; starting anyway (%s)", s,
            (unsigned long)late, kStopWaitBoundS,
            oldMark ? "SIMCAM_OFF_FLUSH_MARK_KEPT: flush mark cleared"
                    : "flush mark kept: the late waiter ends when it runs");
    return out;
}

