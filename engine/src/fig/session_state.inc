// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): FigCaptureSession: per-session state and sink routing.
//
// Provides: SimcamSession, SimcamSink, SimcamRecording, SessLookup, SessPost, SafeKVC, PhotoQueue, LocalQueueMsg
//           (the providers, fig/session_hook.inc, core/control.inc)
//           StopWait*, the photo stop wait (providers/, fig/session_hook.inc, client/photo.inc)
// Owns:     gSessionLayer and gStopWait, defined in fig/session.h with which queue or lock owns what
//           gSessionLayer: fig/session_hook.inc makes it; providers/pump.inc writes irisMaxDims; all use the pump queue
//           gStopWait, kStopWaitBoundS: also used by providers/pump.inc and client/photo.inc
//           pCMNotificationCenterGetDefaultLocalCenter (resolved by fig/session_hook.inc), also in fig/session.h
// Uses:     fig/sources.inc, providers/webcam_path.inc
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <IOSurface/IOSurfaceRef.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#include "session.h"
#include "../providers/webcam.h"
#include "sources.h"

// Photo encode, preview image and completion, in request order, off the pump (providers/photo.inc). StopRunning and an
// interruption drain it before posting DidStopRunning, so a photo taken right before a stop still completes first.
static dispatch_queue_t PhotoQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("simcam.photo", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

// object's description, cut to maxLength characters (with an ellipsis), for log lines
static NSString *DescribeShort(id object, NSUInteger maxLength) {
    NSString *text = [object description];
    if (text.length <= maxLength) {
        return text;
    }
    return [[text substringToIndex:maxLength] stringByAppendingString:@"…"];
}

// object's value for key, or nil when object has no such key (valueForKey: throws)
static id SafeKVC(id object, NSString *key) {
    @try {
        return [object valueForKey:key];
    } @catch (NSException *exception) {
        return nil;
    }
}

// iOS 26 carries rotation as degrees (outputRotationDegrees / videoRotationDegrees / rotationDegrees); iOS 17.5 carries
// an AVCaptureVideoOrientation-style enum (outputOrientation / videoOrientation / orientation: 1 portrait, 2
// upside-down, 3 landscapeRight, 4 landscapeLeft). The enum is the same for both cameras, but the degrees are not: for
// the same device orientation 26 and 27 send 0 (back) and 180 (front) in landscape, so for the front camera the two
// landscape values are swapped (without this, 17.5's front-camera landscape photos and movies were 180° off).
// position is the active device's AVCaptureDevicePosition, 0 if unknown.
// The enum's values (AVCaptureVideoOrientation, named here because the SDK deprecates it):
enum {
    kVideoOrientationPortrait = 1,
    kVideoOrientationPortraitUpsideDown = 2,
    kVideoOrientationLandscapeRight = 3,   // home button on the right
    kVideoOrientationLandscapeLeft = 4,
};

static int SettingsRotationDeg(id settings, NSString *degKey, NSString *orientKey, long position) {
    id degrees = SafeKVC(settings, degKey);
    if (degrees) {
        return [degrees intValue];
    }
    BOOL front = position == AVCaptureDevicePositionFront;
    switch ([SafeKVC(settings, orientKey) intValue]) {
        case kVideoOrientationPortrait: return 90;
        case kVideoOrientationPortraitUpsideDown: return 270;
        case kVideoOrientationLandscapeRight: return front ? 180 : 0;
        case kVideoOrientationLandscapeLeft: return front ? 0 : 180;
        default: return 0;
    }
}

// Logs every property of object's class and superclasses with its value, once per class (a diagnostic of what AVF
// hands the session).
static void DiagDumpProps(id object) {
    static NSMutableSet *seenClasses;
    if (!seenClasses) {
        seenClasses = [NSMutableSet set];
    }
    Class objectClass = [object class];
    NSString *className = NSStringFromClass(objectClass);
    if ([seenClasses containsObject:className]) {
        return;
    }
    [seenClasses addObject:className];
    NSMutableArray *names = [NSMutableArray array];

    for (Class cls = objectClass; cls && cls != [NSObject class]; cls = class_getSuperclass(cls)) {
        unsigned propCount = 0;
        objc_property_t *props = class_copyPropertyList(cls, &propCount);
        for (unsigned i = 0; i < propCount; i++) {
            NSString *propName = @(property_getName(props[i]));
            const char *attrs = property_getAttributes(props[i]);
            // an object or a scalar KVC can box; anything else (structs, pointers, blocks) is listed by its type only
            BOOL boxable = attrs && (attrs[1] == '@' || strchr("cCiIsSlLqQfdB", attrs[1]));
            if (!boxable) {
                [names addObject:[NSString stringWithFormat:@"%@=<%c>", propName, attrs ? attrs[1] : '?']];
                continue;
            }
            id value = SafeKVC(object, propName);
            [names addObject:[NSString stringWithFormat:@"%@=%@", propName, value ? DescribeShort(value, 80) : @"nil"]];
        }
        free(props);
    }

    LOG(@"PROPS %@: %@", className, [names componentsJoinedByString:@" | "]);
}

// The per-session classes are declared in fig/session.h.

// SimcamSink's CF objects are `assign`, and the frame path makes them lazily (providers/video.inc, and MirrorFlip in
// providers/mirror.inc). So the sink releases them here.
// A sink lives as long as its session's wiring: PumpWireSinks makes new sinks on every commit and for every new
// session. Without this release, a video data output that needs a conversion leaks its pool and transfer session on
// every session. That is about 3 MB of pixel buffers and scaler state: a plain app opening the camera 50 times grew
// 3.1–3.2 MB per open.
// `queue` belongs to the wiring, which releases it.
// Not released here (a TODO): the preview sink's CAImageQueue and the IOSurfaces it registers (providers/video.inc).
// They exist only on the opt-in SIMCAM_PREVIEW_IMAGE_QUEUE path; by default, surfaces are posted directly.

@implementation SimcamSink

- (void)dealloc {
    if (_transferSess) {
        VTPixelTransferSessionInvalidate(_transferSess);
        CFRelease(_transferSess);
    }
    if (_pool) {
        CVPixelBufferPoolRelease(_pool);
    }
    if (_rotationSess) {
        VTPixelRotationSessionInvalidate(_rotationSess);
        CFRelease(_rotationSess);
    }
    if (_rotationPool) {
        CVPixelBufferPoolRelease(_rotationPool);
    }
    if (_flipSess) {
        VTPixelRotationSessionInvalidate(_flipSess);
        CFRelease(_flipSess);
    }
    WebcamOutputCacheFree(_wcOut);
}
@end

// Releases the mirror flip session (made by providers/audio.inc's append path) and the webcam pool, as SimcamSink
// does above; `converter` frees its own.
@implementation SimcamRecording

- (void)dealloc {
    if (_flipSess) {
        VTPixelRotationSessionInvalidate(_flipSess);
        CFRelease(_flipSess);
    }
    WebcamOutputCacheFree(_wcOut);
}
@end

@implementation SimcamSession @end

static SimcamSession *SessLookup(CMBaseObjectRef session) {
    @synchronized (gSessionLayer.sessions) {
        return gSessionLayer.sessions[[NSValue valueWithPointer:session]];
    }
}

// a CMVideoDimensions boxed in an NSValue, else 0 x 0
static CMVideoDimensions DimsFromValue(id value) {
    CMVideoDimensions dims = {0, 0};
    if ([value isKindOfClass:[NSValue class]] && strcmp([(NSValue *)value objCType], @encode(CMVideoDimensions)) == 0) {
        [(NSValue *)value getValue:&dims];
    }
    return dims;
}

// Posts the notification name for session with payload on gSessionLayer.notifyQueue (asynchronously, in order).
static void SessPost(CMBaseObjectRef session, NSString *name, NSDictionary *payload) {
    CFRetain(session);
    dispatch_async(gSessionLayer.notifyQueue, ^{
        OSStatus status = pCMNotificationCenterPostNotification(pCMNotificationCenterGetDefaultLocalCenter(),
                                                                (__bridge CFStringRef)name, session,
                                                                (__bridge CFDictionaryRef)payload, 0);
        LOG(@"session posted %@ %@ -> %d", name, payload ? DescribeShort(payload, 400) : @"", (int)status);
        CFRelease(session);
    });
}

// ============================================================================
// The photo stop wait: an in-flight photo's delegate callbacks must reach the app before the session's stop is
// reported.
//
// Why. AVF runs didFinishCapture with a dispatch_async to the delegate queue (main), and its own DidStopRunning
// handler, which wakes -[AVCaptureSession _stopFigCaptureSession], is not ordered with that block: the stop could be
// reported first (seen on 17.5, more often with main busy). Draining the photo queue orders only the Fig notifications.
//
// How. A photo is pending from IrisCaptureImage until client/photo.inc marks it done on the delegate queue, after its
// didFinishCapture. When sess_StopRunning finds photos of its session pending, it posts DidStopRunning once they are
// done, from the session's serial stop queue (StopWaitQueue), waiting at most kStopWaitBoundS. AVF's stopRunning
// thread meanwhile waits on its own condition, holding none of its locks.
// A stop called on a delegate queue is not deferred: the callbacks it would wait for run after it returns. The main
// thread counts as a delegate queue unless delegate queues were seen and none was main, because a stopRunning in
// `sessionQueue.sync {}` from main runs on the main thread, where dispatch_get_specific doesn't see main's mark.
//
// Leaving the pending set. A photo leaves gStopWait.pending when its didFinishCapture runs (StopWaitPhotoDone), when a
// stop that waited for it times out, or when its session is finalized (StopWaitForget). So a lost callback costs one
// stop the full bound, once.
//
// Restart. A startRunning while a deferred stop is outstanding would post DidStartRunning before that DidStopRunning,
// and AVF would take the session as stopped (a defensive case: AVF has never forwarded such a start). So
// sess_StartRunning first flushes the stop (StopWaitFlush): it marks each outstanding stop id, wakes the waiters, and
// waits at most kStopWaitBoundS until each has queued its DidStopRunning (StopWaitEndStop). A mark lasts until its
// waiter ends, so a late waiter ends at once; a stop after the flush has a new id.
//
// Queues. Each waiter runs on a serial queue that targets an overcommit root queue, so it gets a thread even when the
// app has used up GCD's pool (a concurrent queue could starve the flush). One queue per session, so a flush never
// waits behind another session's waiter.
//
// The bound, 500 ms, is far above a normal wait (1.4-10 ms) and the SIMCAM_TEST_FINISH_DELAY_MS=300 control's 320 ms;
// it is the longest a stop hitches when the main thread is blocked on the stopping thread in some other way.
// Off switches, which post the stop at once: SIMCAM_OFF_STOP_WAIT=1, and SIMCAM_OFF_PHOTO_DRAIN=1 (also a mutant).
// Mutants: SIMCAM_OFF_FLUSH_MARK_KEPT, SIMCAM_OFF_FLUSH_WAIT.
// Test faults: SIMCAM_TEST_FINISH_DELAY_MS, SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS (client/photo.inc),
// SIMCAM_TEST_LATE_WAITER_MS, SIMCAM_TEST_FLUSH_RACE.
// The bound is kStopWaitBoundS, and the wait's state gStopWait (fig/session.h).
// ============================================================================

static void StopWaitInit(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        gStopWait.cond = [NSCondition new];
        gStopWait.pending = [NSMutableDictionary dictionary];
        gStopWait.stops = [NSMutableDictionary dictionary];
        gStopWait.flushed = [NSMutableSet set];
    });
}

// IrisCaptureImage: the photo settingsID of session is pending until its didFinishCapture
static void StopWaitPhotoBegin(CMBaseObjectRef session, long long settingsID) {
    if (!atomic_load(&gStopWait.hooked)) {
        return;
    }
    StopWaitInit();
    [gStopWait.cond lock];
    gStopWait.pending[@(settingsID)] = [NSValue valueWithPointer:session];
    [gStopWait.cond unlock];
}

// the photo's didFinishCapture ran (client/photo.inc): it is no longer pending
static void StopWaitPhotoDone(long long settingsID) {
    if (!gStopWait.cond) {
        return;
    }
    [gStopWait.cond lock];
    if (gStopWait.pending[@(settingsID)]) {
        [gStopWait.pending removeObjectForKey:@(settingsID)];
        [gStopWait.cond broadcast];
    }
    [gStopWait.cond unlock];
}

// the settings IDs of session's pending photos
static NSArray<NSNumber *> *StopWaitPendingFor_locked(CMBaseObjectRef session) {
    NSValue *sessionKey = [NSValue valueWithPointer:session];
    return [gStopWait.pending keysOfEntriesPassingTest:^BOOL(NSNumber *settingsID, NSValue *owner, BOOL *stop) {
        return [owner isEqual:sessionKey];
    }].allObjects;
}

static BOOL StopWaitHasPending(CMBaseObjectRef session) {
    if (!gStopWait.cond) {
        return NO;
    }
    [gStopWait.cond lock];
    BOOL pending = StopWaitPendingFor_locked(session).count > 0;
    [gStopWait.cond unlock];
    return pending;
}

// A stop on this thread can't wait for photo callbacks: it runs on a marked delegate queue, or on the main thread while
// the main queue is (or, before any photo's queue is known, may be) a delegate queue.
static BOOL StopWaitOnDelegateQueue(void) {
    if (dispatch_get_specific(&gStopWait.delegateQueueKey)) {
        return YES;
    }
    int delegateQueuesSeen = atomic_load(&gStopWait.delegateQueuesSeen);
    return pthread_main_np() && (delegateQueuesSeen == 0 || (delegateQueuesSeen & kStopWaitSawMainDelegateQ));
}

// those of settingsIDs still pending
static NSArray<NSNumber *> *StopWaitStillPending_locked(NSArray<NSNumber *> *settingsIDs) {
    NSMutableArray *stillPending = [NSMutableArray array];
    for (NSNumber *settingsID in settingsIDs) {
        if (gStopWait.pending[settingsID]) {
            [stillPending addObject:settingsID];
        }
    }
    return stillPending;
}

// The photos of session pending now, with the stop that waits for them registered as outstanding under a new id
// (*stopId; its waiter ends it with StopWaitEndStop). nil, and nothing registered, if none is pending.
static NSArray<NSNumber *> *StopWaitBeginStop(CMBaseObjectRef session, long long *stopId) {
    if (!gStopWait.cond) {
        return nil;
    }
    [gStopWait.cond lock];
    NSArray *pending = StopWaitPendingFor_locked(session);
    if (pending.count) {
        *stopId = ++gStopWait.lastStopId;
        gStopWait.stops[@(*stopId)] = [NSValue valueWithPointer:session];
    } else {
        pending = nil;
    }
    [gStopWait.cond unlock];
    return pending;
}

// the ids of session's outstanding deferred stops
static NSArray<NSNumber *> *StopWaitStopsOf_locked(CMBaseObjectRef session) {
    NSValue *sessionKey = [NSValue valueWithPointer:session];
    return [gStopWait.stops keysOfEntriesPassingTest:^BOOL(NSNumber *stopKey, NSValue *owner, BOOL *stop) {
        return [owner isEqual:sessionKey];
    }].allObjects;
}

// Waits until none of settingsIDs (the photos pending when the stop came) is pending, until a start flushes the stop
// (stopId), or until budgetSeconds have passed. At the deadline the ones left are dropped, so a photo whose
// didFinishCapture never comes (SIMCAM_OFF_RESPONSIVE_NOTE, or an error path AVF finishes elsewhere) can't stall later
// stops; photos taken after that (after a restart) are kept. Returns how many were left; *flushed says whether a start
// ended the wait (or had flushed the stop before the waiter ran).
static NSUInteger StopWaitForPhotos(long long stopId, NSArray<NSNumber *> *settingsIDs, double budgetSeconds,
                                    BOOL *flushed) {
    NSNumber *stopKey = @(stopId);
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:budgetSeconds];
    [gStopWait.cond lock];
    while (StopWaitStillPending_locked(settingsIDs).count && ![gStopWait.flushed containsObject:stopKey] &&
           [gStopWait.cond waitUntilDate:deadline]) {}
    NSArray *left = StopWaitStillPending_locked(settingsIDs);
    *flushed = [gStopWait.flushed containsObject:stopKey];
    if (!*flushed) {
        [gStopWait.pending removeObjectsForKeys:left];   // when flushed, their callbacks still come
    }
    [gStopWait.cond unlock];
    return left.count;
}

// The waiter calls this once its DidStopRunning is queued on the notification queue. The stop is then no longer
// outstanding, so a start waiting in StopWaitFlush goes on.
static void StopWaitEndStop(long long stopId) {
    atomic_fetch_add(&gStopWait.stopsPosted, 1);
    [gStopWait.cond lock];
    [gStopWait.stops removeObjectForKey:@(stopId)];
    [gStopWait.flushed removeObject:@(stopId)];   // its mark ends with it (not when a start's wait times out)
    [gStopWait.cond broadcast];
    [gStopWait.cond unlock];
}

// session finalize: its pending photos are forgotten
static void StopWaitForget(CMBaseObjectRef session) {
    if (!gStopWait.cond) {
        return;
    }
    [gStopWait.cond lock];
    [gStopWait.pending removeObjectsForKeys:StopWaitPendingFor_locked(session)];
    [gStopWait.cond broadcast];
    [gStopWait.cond unlock];
}

// session's serial stop queue (one per session; see above). A session the engine doesn't know (not expected) gets a
// shared serial one.
static dispatch_queue_t StopWaitQueue(CMBaseObjectRef session) {
    static dispatch_queue_t shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        shared = dispatch_queue_create("simcam.stop", DISPATCH_QUEUE_SERIAL);
    });
    SimcamSession *sess = SessLookup(session);
    if (!sess) {
        return shared;
    }
    @synchronized (sess) {
        if (!sess.stopQueue) {
            sess.stopQueue = dispatch_queue_create("simcam.stop.session", DISPATCH_QUEUE_SERIAL);
        }
        return sess.stopQueue;
    }
}

// Under gStopWait.cond: waits until each stop in stopIds has queued its DidStopRunning (StopWaitEndStop), at most
// kStopWaitBoundS. A waiter sees its mark as soon as it runs (its session's serial queue holds only flushed waiters
// before it), so the bound is ample. Returns how many are still outstanding.
static NSUInteger StopWaitForFlushedStops_locked(NSArray<NSNumber *> *stopIds) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:kStopWaitBoundS];
    NSUInteger (^outstanding)(void) = ^NSUInteger {
        NSUInteger count = 0;
        for (NSNumber *stopKey in stopIds) {
            count += gStopWait.stops[stopKey] != nil;
        }
        return count;
    };
    while (outstanding() && [gStopWait.cond waitUntilDate:deadline]) {}
    return outstanding();
}

// sess_StartRunning: a deferred stop of session still outstanding is reported now, before the start. Each such stop's
// waiter is told to stop waiting, and this returns once their DidStopRunning are queued (StopWaitEndStop), waiting at
// most kStopWaitBoundS. Returns whether one was outstanding.
static BOOL StopWaitFlush(CMBaseObjectRef session) {
    if (!gStopWait.cond) {
        return NO;
    }
    BOOL flushWait = FlushWaitOn();
    BOOL markKept = FlushMarkKeptOn();
    NSUInteger late = 0;
    [gStopWait.cond lock];
    NSArray<NSNumber *> *stopIds = StopWaitStopsOf_locked(session);
    BOOL hadOutstanding = stopIds.count > 0;

    if (hadOutstanding) {
        [gStopWait.flushed addObjectsFromArray:stopIds];
        [gStopWait.cond broadcast];
        if (flushWait) {
            late = StopWaitForFlushedStops_locked(stopIds);
            // On a timeout the marks stay until each waiter ends, so a late one ends at once;
            // SIMCAM_OFF_FLUSH_MARK_KEPT is the mutant that clears them.
            if (late && !markKept) {
                [gStopWait.flushed minusSet:[NSSet setWithArray:stopIds]];
            }
        }
    }

    [gStopWait.cond unlock];

    // SIMCAM_OFF_FLUSH_WAIT: the naive flush (it returns before a waiter that is not queued yet)
    if (hadOutstanding && !flushWait) {
        dispatch_sync(StopWaitQueue(session), ^{});
    }
    if (late) {
        LOG(@"session %p StartRunning: %lu deferred stop(s) still outstanding after %.1f s; starting anyway (%s)",
            session, (unsigned long)late, kStopWaitBoundS,
            markKept ? "flush mark kept: the late waiter ends when it runs"
                     : "SIMCAM_OFF_FLUSH_MARK_KEPT: flush mark cleared");
    }
    return hadOutstanding;
}
