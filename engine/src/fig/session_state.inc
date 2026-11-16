// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): FigCaptureSession: per-session state and sink routing.
// ============================================================================
// FigCaptureSession — per-session state, sink routing, frame pump,
// preview (CAImageQueue / IOSurface), photo (Iris slots), movie (FileSink slots),
// metadata (barcodes), section properties.
// ============================================================================
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <IOSurface/IOSurfaceRef.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>

static void *(*pCMNotificationCenterGetDefaultLocalCenter)(void);
static void *gSessDerived[64];
static void *gOrigDerived[64];
static M2VTable gSessVTable;
static dispatch_queue_t gSessQ;      // notification posting (serial, ordered)
static dispatch_queue_t gPumpQ;      // frame pump + sinks
// Photo encode, preview image and completion, in request order, off the pump (providers/photo.inc). StopRunning and an
// interruption drain it before posting DidStopRunning, so a photo taken right before a stop still completes first.
static dispatch_queue_t M5PhotoQ(void) { static dispatch_queue_t q; static dispatch_once_t o; dispatch_once(&o, ^{ q = dispatch_queue_create("simcam.photo", DISPATCH_QUEUE_SERIAL); }); return q; }
#pragma pack(push, 1)
typedef struct { int32_t type; CFTypeRef obj; int32_t pad[2]; } M2LQMsg;
#pragma pack(pop)

static NSString *M5Desc(id o, NSUInteger max) { NSString *d = [o description]; return d.length > max ? [[d substringToIndex:max] stringByAppendingString:@"…"] : d; }
static id M5KVC(id o, NSString *k) { @try { return [o valueForKey:k]; } @catch (NSException *e) { return nil; } }
// iOS 26 carries rotation as degrees (outputRotationDegrees / videoRotationDegrees / rotationDegrees); iOS 17.5 carries an
// AVCaptureVideoOrientation-style enum (outputOrientation / videoOrientation / orientation: 1 portrait, 2 upside-down, 3 landscapeRight, 4 landscapeLeft).
// The enum is the same for both cameras, but the degrees are not: for the same device orientation 26/27 send 0 (back) and
// 180 (front) in landscape, so the front camera swaps the two landscape values (17.5 front landscape photos/movies were 180° off,
// without this). position: AVCaptureDevicePosition of the active device (2 = front), 0 if unknown.
static int M6Deg(id o, NSString *degKey, NSString *orientKey, long position) {
    id d = M5KVC(o, degKey); if (d) return [d intValue];
    BOOL front = position == 2;
    switch ([M5KVC(o, orientKey) intValue]) { case 1: return 90; case 2: return 270; case 3: return front ? 180 : 0; case 4: return front ? 0 : 180; default: return 0; }
}
static void M5DumpProps(id o) {
    static NSMutableSet *seen; if (!seen) seen = [NSMutableSet set];
    Class c = [o class]; NSString *cn = NSStringFromClass(c);
    if ([seen containsObject:cn]) return; [seen addObject:cn];
    NSMutableArray *names = [NSMutableArray array];
    for (Class k = c; k && k != [NSObject class]; k = class_getSuperclass(k)) {
        unsigned n = 0; objc_property_t *ps = class_copyPropertyList(k, &n);
        for (unsigned i = 0; i < n; i++) { NSString *pn = @(property_getName(ps[i])); const char *at = property_getAttributes(ps[i]);
            if (!at || !(at[1] == '@' || strchr("cCiIsSlLqQfdB", at[1]))) { [names addObject:[NSString stringWithFormat:@"%@=<%c>", pn, at ? at[1] : '?']]; continue; }
            id v = M5KVC(o, pn); [names addObject:[NSString stringWithFormat:@"%@=%@", pn, v ? M5Desc(v, 80) : @"nil"]]; }
        free(ps);
    }
    LOG(@"PROPS %@: %@", cn, [names componentsJoinedByString:@" | "]);
}

@interface M5Sink : NSObject
@property (copy) NSString *kind, *sinkID; @property (assign) CFTypeRef queue; @property int w, h; @property OSType fmt;
@property (assign) CVPixelBufferPoolRef pool; @property (assign) VTPixelTransferSessionRef xfer; @property float rotation;
@property (assign) CFTypeRef imageQueue; @property (strong) NSMutableDictionary *regIDs; @property int frames; @property BOOL mirrored;
@property int vdoRot; @property (assign) VTPixelRotationSessionRef rotSess; @property (assign) CVPixelBufferPoolRef rotPool; // data-output connection rotation/mirroring
@property int photoW, photoH;
@property (assign) VTPixelRotationSessionRef flipSess; @property int flipKind;   // mirroring flip (M12Flip)
@property (assign) void *wcOut;   // webcam frame path: this sink's pool and last output (providers/webcam_path.inc, M13WcOutC)
@end
// Ledger L75: the CF objects above are `assign` and made lazily by the frame path (video.inc, M12Flip); a sink lives as long
// as its session's wiring (M5WireSinks makes new sinks on every commit, and every new session), so it releases them here.
// Without that, a video data output whose format/size needs a conversion leaks its pool + transfer session (~3 MB of
// pixel buffers and VT scaler state) per session; a plain app opening the camera 50 times would grow 3.1–3.2 MB per open.
// `queue` stays the wiring's (released there). Not released here: the preview sink's CAImageQueue and the IOSurfaces it
// registers (video.inc, only with the opt-in M5_PREVIEW_IQ path; the default posts surfaces directly) — TODO.
static void M13WcOutFree(void *p);   // providers/webcam_path.inc
@implementation M5Sink
- (void)dealloc {
    if (_xfer) { VTPixelTransferSessionInvalidate(_xfer); CFRelease(_xfer); }
    if (_pool) CVPixelBufferPoolRelease(_pool);
    if (_rotSess) { VTPixelRotationSessionInvalidate(_rotSess); CFRelease(_rotSess); }
    if (_rotPool) CVPixelBufferPoolRelease(_rotPool);
    if (_flipSess) { VTPixelRotationSessionInvalidate(_flipSess); CFRelease(_flipSess); }
    M13WcOutFree(_wcOut);
}
@end

@interface M5Rec : NSObject
@property (copy) NSString *sinkID; @property long long settingsID; @property (strong) AVAssetWriter *writer; @property (strong) AVAssetWriterInputPixelBufferAdaptor *adaptor;
@property CMTime start, last; @property BOOL started, stopping, stopRequested; @property (strong) NSURL *url; @property int w, h;   // stopping: no more appends (also set when startWriting fails); stopRequested: M12StopRec ran
@property (strong) AVAssetWriterInput *audioIn; @property int audioBufs; @property CMTime audioFirst; @property BOOL audioDone;
@property (strong) M5Sink *conv;   // movie at the active format size
@property BOOL mirror; @property int rot; @property (assign) VTPixelRotationSessionRef flipSess; @property int flipKind;   // mirroring flip (M12Flip)
@property (assign) void *wcOut;   // webcam frame path: the movie's own pool and last frame (webcam_path.inc, M13WcMovieOut)
@end
@implementation M5Rec   // L75: the mirror flip session (audio.inc's append path) and the webcam pool; `conv` frees its own (M5Sink above)
- (void)dealloc { if (_flipSess) { VTPixelRotationSessionInvalidate(_flipSess); CFRelease(_flipSess); } M13WcOutFree(_wcOut); }
@end

@interface M5Sess : NSObject
@property (assign) CMBaseObjectRef obj; @property (strong) id config; @property BOOL running; @property (strong) NSMutableArray<M5Sink *> *sinks;
@property (strong) dispatch_source_t timer; @property (strong) NSMutableDictionary<NSString *, M5Rec *> *recs; @property long long liveCID;
@property (strong) NSMutableSet<NSString *> *audioMovieSinks; @property BOOL wantsMic, micOn;
@property int fmtW, fmtH; @property double fps;   // from the genuine device's activeFormat / activeVideoMinFrameDuration
@property long position; @property BOOL interrupted; @property int interruptReason;   // device position, interruptions
@property (strong) dispatch_queue_t stopQ;   // this session's serial stop queue (M40StopQ), made at its first deferred stop
@end
@implementation M5Sess @end
static NSMutableDictionary<NSValue *, M5Sess *> *gSessions;
static M5Sess *M5S(CMBaseObjectRef s) { @synchronized (gSessions) { return gSessions[[NSValue valueWithPointer:s]]; } }
static CMVideoDimensions M8Dims(id v) { CMVideoDimensions d = {0, 0}; if ([v isKindOfClass:[NSValue class]] && strcmp([(NSValue *)v objCType], @encode(CMVideoDimensions)) == 0) [(NSValue *)v getValue:&d]; return d; }
static CMVideoDimensions gIrisMaxDims;

static void M5SessPost(CMBaseObjectRef s, NSString *name, NSDictionary *payload) {
    CFRetain(s);
    dispatch_async(gSessQ, ^{
        OSStatus e = pCMNotificationCenterPostNotification(pCMNotificationCenterGetDefaultLocalCenter(), (__bridge CFStringRef)name, s, (__bridge CFDictionaryRef)payload, 0);
        LOG(@"session posted %@ %@ -> %d", name, payload ? M5Desc(payload, 400) : @"", (int)e);
        CFRelease(s);
    });
}

// ============================================================================
// Ledger L66: an in-flight photo's delegate callbacks are delivered before the session's stop is reported.
// Draining the photo queue (sess_StopRunning) orders the Fig notifications: IrisStillImageComplete and
// ReadyForResponsiveRequest are posted before DidStopRunning on gSessQ. That doesn't order what the app sees. AVF runs the
// delegate's callbacks asynchronously on the request's delegate queue
// (-[AVCapturePhotoOutput _handleDidFinishCaptureNotificationWithPayload:forRequest:] -> -[AVWeakReferencingDelegateStorage
// invokeDelegateCallbackWithBlock:], a dispatch_async to the main queue), while the session's stop is reported on the
// thread that called stopRunning as soon as AVF's DidStopRunning handler wakes it (-[AVCaptureSession
// _stopFigCaptureSession] waits up to 9 s on a condition, then -_setRunning: flips isRunning and posts
// AVCaptureSessionDidStopRunningNotification there). That handler runs on the main thread too, but not queued in order
// with the delegate's dispatch_async: when main is busy it can run first, and then the stop is reported before
// didFinishCapture (17.5: once in 11 idle canary runs; 3 of 17 runs with the main queue made busy).
// The fix: every photo the engine takes is pending from IrisCaptureImage until AVF has run the delegate's
// didFinishCaptureForResolvedSettings: (client/photo.inc hooks the method above and marks the photo done from the delegate
// queue, after the callback). sess_StopRunning with photos of its session pending posts DidStopRunning from a private
// queue once they are done, bounded by kM40WaitS; AVF's stopRunning thread waits on its own condition meanwhile, holding
// none of its locks. Not waited for: a stopRunning called on a photo delegate queue (from a photo callback), since the
// callbacks it would wait for run on that queue after it returns; the stop is reported first, as AVF's own would be. The
// main thread counts as one unless every delegate queue seen so far was another queue: AVF's is
// the main queue, and a stopRunning in `sessionQueue.sync {}` from main runs on the main thread under another queue, where
// dispatch_get_specific doesn't see main's mark; before the first photo's queue is marked, main is assumed to be it.
// Off switches: M40_OLD_STOP=1 (post at once, without waiting); M13_NO_PHOTODRAIN=1 also skips this wait (L51's mutant
// is "stop doesn't wait for in-flight photos"). Test faults (client/photo.inc): M40_FINISH_DELAY_MS=<ms> hands
// didFinishCapture to the delegate queue <ms> late, the deterministic form of the 17.5 race; M40_BUSY_DELEGATEQ_MS=<ms>
// keeps the delegate queue (main) busy before it, the condition under which 17.5 loses the race on its own.
// Stale entries: an entry leaves gM40Pending when its didFinishCapture has run (M40PhotoDone), when a stop that
// waited for it reaches its deadline (only the photos pending when that stop came: one taken after a restart isn't dropped
// by an older stop's deadline), or when its session is finalized (M40Forget). A photo whose didFinishCapture never comes
// (M10_NO_RESPONSIVE, an error AVF finishes elsewhere) and that no stop waited for stays until then; the next stop with it
// pending waits the full kM40WaitS once and drops it. A stop flushed by a restart (M40FlushStop) leaves its photos pending:
// their callbacks still come.
// A restart: a stop deferred here is still outstanding while its photos' callbacks are due; a startRunning on another
// thread meanwhile would post DidStartRunning before that DidStopRunning, and AVF would take the session as stopped.
// sess_StartRunning first flushes it (M40FlushStop: the waiter stops waiting and posts DidStopRunning, then the start's).
// Defensive: AVF never forwards such a start on 17.5/26.3/27.0 (no second _setRunning:1 seen). Ordering:
// the flush waits on gM40Cond until the session has no deferred stop outstanding, and a stop stays outstanding until its
// DidStopRunning is queued on gSessQ (M40EndStop), so the start's DidStartRunning is queued after it however the threads
// interleave, including a start between M40BeginStop and the waiter's dispatch (the old dispatch_sync on the stop queue
// returned before a waiter not yet queued). The wait is bounded (kM40WaitS, logged). The flush
// marks the STOPS outstanding then (each deferred stop has its own id, M40BeginStop), not the session, and a mark lasts until
// that stop's waiter ends (M40EndStop), also when the start's wait timed out: a merely-late waiter (CPU starvation) still ends
// at once when it runs, instead of waiting up to another kM40WaitS for its photos with the start already reported; a stop
// that comes after the flush has a new id and isn't flushed by it. M59_OLD_FLUSHMARK=1 (L74's mutant) brings back the
// clearing of the mark on timeout; M59_LATE_WAITER=<ms> (test fault) makes the waiter start <ms> late, past the start's
// bound. Each session has its own SERIAL
// stop queue (M40StopQ). A custom serial queue targets an overcommit root queue, so its waiter gets a thread even when the app
// has used up GCD's pool (a custom concurrent queue doesn't, and could starve the flush); one queue per session means a flush
// never waits behind another session's waiter. Waiters of one session run in order; a flushed one ends at once, so a later one
// isn't held. M54_OLD_FLUSH=1 brings back a naive flush: a dispatch_sync on the (serial) stop queue instead of the
// wait, which returns before a waiter not yet queued; M54_FLUSH_RACE=1 (test fault) runs a flush in exactly that window and
// logs whether the DidStopRunning came first.
// Bound: 500 ms. Normal waits are 1.4-10 ms (17.5); the deterministic fault is 300 ms late (waits
// ~320 ms); a stop that can't be waited for (on a thread main is blocked on some other way) hitches at most 500 ms.
// ============================================================================
static const double kM40WaitS = 0.5;
static NSCondition *gM40Cond;                                   // guards gM40Pending, gM40Stops, gM40Flushed, gM40StopGen
static NSMutableDictionary<NSNumber *, NSValue *> *gM40Pending; // settingsID -> session: IrisCaptureImage .. didFinishCapture ran
static NSMutableDictionary<NSNumber *, NSValue *> *gM40Stops;   // stop id -> session: deferred stops outstanding (their M40StopQ)
static NSMutableSet<NSNumber *> *gM40Flushed;                   // of those, the stop ids a start flushed: stop waiting now
static long long gM40StopGen;                                   // the last stop id handed out
static _Atomic int gM40Hooked;                                  // the client hook resolved, so something clears gM40Pending
static _Atomic int gM40MainDelegateQ;                           // bits: 1 a photo delegate queue was main, 2 another queue
static char gM40DelegateQKey;                                   // set on every photo delegate queue the hooks have seen
static void M40Init(void) { static dispatch_once_t o; dispatch_once(&o, ^{ gM40Cond = [NSCondition new]; gM40Pending = [NSMutableDictionary dictionary]; gM40Stops = [NSMutableDictionary dictionary]; gM40Flushed = [NSMutableSet set]; }); }
static void M40PhotoBegin(CMBaseObjectRef s, long long sid) {
    if (!atomic_load(&gM40Hooked)) return;
    M40Init(); [gM40Cond lock]; gM40Pending[@(sid)] = [NSValue valueWithPointer:s]; [gM40Cond unlock];
}
static void M40PhotoDone(long long sid) {
    if (!gM40Cond) return;
    [gM40Cond lock]; if (gM40Pending[@(sid)]) { [gM40Pending removeObjectForKey:@(sid)]; [gM40Cond broadcast]; } [gM40Cond unlock];
}
static NSArray<NSNumber *> *M40PendingFor_locked(CMBaseObjectRef s) {
    NSValue *v = [NSValue valueWithPointer:s];
    return [gM40Pending keysOfEntriesPassingTest:^BOOL(NSNumber *k, NSValue *o, BOOL *stop) { return [o isEqual:v]; }].allObjects;
}
static BOOL M40HasPending(CMBaseObjectRef s) {
    if (!gM40Cond) return NO;
    [gM40Cond lock]; BOOL r = M40PendingFor_locked(s).count > 0; [gM40Cond unlock]; return r;
}
// a stop on this thread can't wait for photo callbacks: it runs on a marked delegate queue, or on the main thread while the
// main queue is (or, before any photo's queue is known, may be) a delegate queue
static BOOL M40OnDelegateQueue(void) {
    if (dispatch_get_specific(&gM40DelegateQKey)) return YES;
    int m = atomic_load(&gM40MainDelegateQ);
    return pthread_main_np() && (m == 0 || (m & 1));
}
static NSArray<NSNumber *> *M40StillPending_locked(NSArray<NSNumber *> *sids) {
    NSMutableArray *r = [NSMutableArray array]; for (NSNumber *k in sids) if (gM40Pending[k]) [r addObject:k]; return r;
}
// The photos of s pending now, and the stop that waits for them registered as outstanding under a new id (*stopId; its
// waiter ends it, M40EndStop); nil (nothing registered) if none is pending.
static NSArray<NSNumber *> *M40BeginStop(CMBaseObjectRef s, long long *stopId) {
    if (!gM40Cond) return nil;
    [gM40Cond lock];
    NSArray *r = M40PendingFor_locked(s);
    if (r.count) { *stopId = ++gM40StopGen; gM40Stops[@(*stopId)] = [NSValue valueWithPointer:s]; } else r = nil;
    [gM40Cond unlock];
    return r;
}
static NSArray<NSNumber *> *M40StopsOf_locked(CMBaseObjectRef s) {   // the ids of s's outstanding deferred stops
    NSValue *v = [NSValue valueWithPointer:s];
    return [gM40Stops keysOfEntriesPassingTest:^BOOL(NSNumber *k, NSValue *o, BOOL *stop) { return [o isEqual:v]; }].allObjects;
}
// Waits until none of sids (the photos pending when the stop came) is pending, a start flushes the stop (stopId), or the
// deadline. At the deadline the ones left are dropped, so a photo whose didFinishCapture never comes (M10_NO_RESPONSIVE, an
// error path AVF finishes elsewhere) can't stall later stops; photos taken after that (after a restart) are kept. Returns how
// many were left; *flushed says whether a start ended the wait (or had flushed the stop before the waiter ran).
static NSUInteger M40WaitPhotoCallbacks(long long stopId, NSArray<NSNumber *> *sids, double budget, BOOL *flushed) {
    NSNumber *id_ = @(stopId);
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:budget];
    [gM40Cond lock];
    while (M40StillPending_locked(sids).count && ![gM40Flushed containsObject:id_] && [gM40Cond waitUntilDate:until]) {}
    NSArray *left = M40StillPending_locked(sids);
    *flushed = [gM40Flushed containsObject:id_];
    if (!*flushed) [gM40Pending removeObjectsForKeys:left];   // flushed: their callbacks still come
    [gM40Cond unlock];
    return left.count;
}
// the waiter, once its DidStopRunning is queued on gSessQ: the stop isn't outstanding any more (a start waiting in
// M40FlushStop goes on)
static _Atomic long gM40StopsPosted;   // deferred stops whose DidStopRunning is queued (M54_FLUSH_RACE reads it)
static void M40EndStop(long long stopId) {
    atomic_fetch_add(&gM40StopsPosted, 1);
    [gM40Cond lock];
    [gM40Stops removeObjectForKey:@(stopId)];
    [gM40Flushed removeObject:@(stopId)];   // its mark ends with it (not when a start's wait times out)
    [gM40Cond broadcast];
    [gM40Cond unlock];
}
static void M40Forget(CMBaseObjectRef s) {   // session finalize
    if (!gM40Cond) return;
    [gM40Cond lock]; [gM40Pending removeObjectsForKeys:M40PendingFor_locked(s)]; [gM40Cond broadcast]; [gM40Cond unlock];
}
// s's serial stop queue (serial, one per session; see above). A session the engine doesn't know (not expected) gets a
// shared serial one.
static dispatch_queue_t M40StopQ(CMBaseObjectRef s) {
    static dispatch_queue_t shared; static dispatch_once_t o; dispatch_once(&o, ^{ shared = dispatch_queue_create("simcam.stop", DISPATCH_QUEUE_SERIAL); });
    M5Sess *S = M5S(s); if (!S) return shared;
    @synchronized (S) { if (!S.stopQ) S.stopQ = dispatch_queue_create("simcam.stop.session", DISPATCH_QUEUE_SERIAL); return S.stopQ; }
}
// sess_StartRunning: a deferred stop of s still outstanding is reported now, before the start: each such stop's waiter
// is told to stop waiting, and this returns once their DidStopRunning are queued (M40EndStop), bounded. Returns whether one
// was outstanding.
static BOOL M40FlushStop(CMBaseObjectRef s) {
    if (!gM40Cond) return NO;
    BOOL old = getenv("M54_OLD_FLUSH") != NULL, oldMark = getenv("M59_OLD_FLUSHMARK") != NULL;
    NSUInteger late = 0;
    [gM40Cond lock];
    NSArray<NSNumber *> *ids = M40StopsOf_locked(s);
    BOOL out = ids.count > 0;
    if (out) {
        [gM40Flushed addObjectsFromArray:ids]; [gM40Cond broadcast];
        if (!old) {
            // a waiter sees its mark as soon as it runs (its session's serial queue holds only flushed waiters before it),
            // so kM40WaitS is ample. On timeout the marks stay until each waiter ends, so a late one ends at once
            NSDate *until = [NSDate dateWithTimeIntervalSinceNow:kM40WaitS];
            NSUInteger (^left)(void) = ^NSUInteger { NSUInteger n = 0; for (NSNumber *k in ids) n += gM40Stops[k] != nil; return n; };
            while (left() && [gM40Cond waitUntilDate:until]) {}
            late = left();
            if (late && oldMark) [gM40Flushed minusSet:[NSSet setWithArray:ids]];   // M59_OLD_FLUSHMARK (L74): the mutant's clearing
        }
    }
    [gM40Cond unlock];
    if (out && old) dispatch_sync(M40StopQ(s), ^{});   // M54_OLD_FLUSH: the naive flush (returns before a waiter not yet queued)
    if (late) LOG(@"session %p StartRunning: %lu deferred stop(s) still outstanding after %.1f s; starting anyway (%s)", s, (unsigned long)late, kM40WaitS,
                  oldMark ? "M59_OLD_FLUSHMARK: flush mark cleared" : "flush mark kept: the late waiter ends when it runs");
    return out;
}

