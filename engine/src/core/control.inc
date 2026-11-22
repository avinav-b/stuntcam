// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): host control file, per-launch env, virtual device
// orientation, interruptions.
//
// Provides: ControlReadAtPrepare, ControlResolve, ControlSwap, ControlEnqueue, ControlStart, activation's control
//           steps (core/lazy.inc)
//           SessInjectRuntimeError, MovieStopAll (core/safety.inc's latches; the first also providers/pump.inc)
// Owns:     nothing shared: its state, gControl (core/control.h), is used only here
// Uses:     core/safety.inc, fig/session_state.inc, fig/session_hook.inc, providers/scene.inc (sets
//           its source and orientation), providers/audio.inc, providers/pump.inc, providers/photo.inc,
//           client/rotation.inc
//
//   SIMCAM_SOURCE=scene|image:<path>|video:<path>|webcam[:<uniqueID>]|qr:<text>   per-launch source (default scene)
//   SIMCAM_ORIENT=portrait|upsideDown|landscapeLeft|landscapeRight|auto
//                per-launch virtual device orientation (UIDevice names)
//   SIMCAM_CONTROL=<host file>   polled every 250 ms; lines key=value:
//       source=...   orientation=...
//       event=<name>[@<seq>]  (interrupt[:reason] | end | runtime-error[:status] | server-died)
// Off switches: SIMCAM_OFF_CONTROL_FILE (no polling), SIMCAM_OFF_INTERRUPTIONS (no lifecycle/audio interruptions),
// SIMCAM_OFF_VIRTUAL_ORIENTATION.
#include <sys/stat.h>
#include "control.h"
#include "../fig/session.h"
#include "../providers/pump.h"
#include "../providers/scene.h"
#include "../providers/audio.h"
#include "../client/rotation.h"
#include "sentinel.h"
#include "../fig/session_hook.h"
#include "../providers/photo.h"

// The control state. core/control.h describes each field and who owns it.
static struct Control gControl = {.poll.isFirst = YES};

static long OrientParse(NSString *name) {
    name = name.lowercaseString;
    if ([name isEqualToString:@"portrait"] || [name isEqualToString:@"1"]) {
        return UIDeviceOrientationPortrait;
    }
    if ([name isEqualToString:@"upsidedown"] || [name isEqualToString:@"portraitupsidedown"] ||
        [name isEqualToString:@"2"]) {
        return UIDeviceOrientationPortraitUpsideDown;
    }

    // UIDeviceOrientationLandscapeLeft: home button on the right
    if ([name isEqualToString:@"landscapeleft"] || [name isEqualToString:@"3"]) {
        return UIDeviceOrientationLandscapeLeft;
    }
    if ([name isEqualToString:@"landscaperight"] || [name isEqualToString:@"4"]) {
        return UIDeviceOrientationLandscapeRight;
    }
    return 0;  // auto
}

// portrait, upside-down, landscape left or right: not unknown, face up or face down
static BOOL IsDeviceOrientation(long orientation) {
    return orientation >= UIDeviceOrientationPortrait && orientation <= UIDeviceOrientationLandscapeRight;
}

// Main queue. The orientation `auto` goes back to after `forced`: the Simulator's own if it rotated meanwhile
// (Hardware > Rotate), else the one before forcing began, else portrait.
static long OrientAfterForce(long forced) {
    long current = UIDevice.currentDevice.orientation;
    if (IsDeviceOrientation(current) && current != forced) {
        return current;
    }
    if (gControl.orientBeforeForce) {
        return gControl.orientBeforeForce;
    }
    return UIDeviceOrientationPortrait;
}

// The interface orientation autorotation gives for a device orientation (landscape left and right swap).
static UIInterfaceOrientationMask OrientInterfaceMask(long deviceOrientation) {
    if (deviceOrientation == UIDeviceOrientationPortrait) {
        return UIInterfaceOrientationMaskPortrait;
    }
    if (deviceOrientation == UIDeviceOrientationPortraitUpsideDown) {
        return UIInterfaceOrientationMaskPortraitUpsideDown;
    }
    if (deviceOrientation == UIDeviceOrientationLandscapeLeft) {
        return UIInterfaceOrientationMaskLandscapeRight;
    }
    return UIInterfaceOrientationMaskLandscapeLeft;
}

// Main queue: the device orientation to apply for a request of `orientation` that replaces `previous` (0: auto). When
// forcing begins it records the device's own orientation; `auto` after a forced one goes back to the device's own
// (clearing the override alone leaves a running app in the forced orientation until it relaunches). 0: nothing to set.
static long OrientToApply(long orientation, long previous) {
    if (orientation && !previous) {
        long current = UIDevice.currentDevice.orientation;
        gControl.orientBeforeForce = IsDeviceOrientation(current) ? current : UIDeviceOrientationPortrait;
    }
    if (orientation || !previous) {
        return orientation;
    }
    long applied = OrientAfterForce(previous);
    // OrientDevice falls back to it now; the motion timer would leave it stale for 0.5 s
    atomic_store(&gOrientation.deviceSeen, applied);
    LOG(@"orientation auto: back to %ld", applied);
    return applied;
}

// Main queue: sets UIDevice's orientation (KVC) and tells its observers.
static void OrientSetDevice(long orientation) {
    @try {
        [UIDevice.currentDevice setValue:@(orientation) forKey:@"orientation"];
    } @catch (NSException *e) {
        LOG(@"UIDevice orientation KVC threw %@", e.reason);
    }
    // the KVC set does not reach observers of the notification (Flutter's camera plugin); post it like backboardd
    // would (SIMCAM_OFF_ORIENTATION_NOTE off)
    if (OrientationNoteOn()) {
        [NSNotificationCenter.defaultCenter postNotificationName:UIDeviceOrientationDidChangeNotification
                                                          object:UIDevice.currentDevice];
    }
}

// Main queue: rotates the UI like autorotation would, if the app allows that orientation (a portrait-locked app keeps
// its UI, like on a device).
static void OrientRotateScenes(long deviceOrientation) {
    UIInterfaceOrientationMask mask = OrientInterfaceMask(deviceOrientation);
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) {
            continue;
        }
        UIWindowScene *windowScene = (UIWindowScene *)scene;
        for (UIWindow *window in windowScene.windows) {
            [window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
        }
        UIWindowSceneGeometryPreferencesIOS *preferences =
            [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
        [windowScene requestGeometryUpdateWithPreferences:preferences
                                             errorHandler:^(NSError *e) {
                                                 LOG(@"UI rotation refused by the app: %@", e.localizedDescription);
                                             }];
    }
}

// Forces the virtual device orientation (0: auto, back to the device's own), then applies it on the main queue: the
// device's orientation, the UI's, and the rotation coordinators'.
static void OrientApply(long orientation, const char *why) {
    // the rotation control itself works with SIMCAM_OFF_VIRTUAL_ORIENTATION too (that switch only reverts the
    // camera behaviour)
    long previous = atomic_exchange(&gOrientation.deviceForced, orientation);
    LOG(@"orientation -> %ld (%s)", orientation, why);
    dispatch_async(dispatch_get_main_queue(), ^{
        long applied = OrientToApply(orientation, previous);
        if (applied) {
            OrientSetDevice(applied);
            OrientRotateScenes(applied);
        }
        CoordOrientationChanged();
    });
}

// ---- interruptions ----
// AVF takes a DidStopRunning with ErrorStatus -16402 as an interruption (the camera was taken), ended by the next
// DidStartRunning; most other statuses become AVCaptureSessionRuntimeErrorNotification.
static const int kFigInterruptedStatus = -16402;

static NSArray<SimcamSession *> *SessAll(void) {
    if (!gSessionLayer.sessions) {
        return @[];
    }
    @synchronized (gSessionLayer.sessions) {
        return gSessionLayer.sessions.allValues;
    }
}

// on the pump queue: every recording of sess stops with status (the keys are read under sess.recs' lock, the stops run
// outside it). Returns how many there were.
static NSUInteger StopSessionRecordings(SimcamSession *sess, int status) {
    NSArray *recordingIDs;
    @synchronized (sess.recs) {
        recordingIDs = sess.recs.allKeys;
    }
    for (NSString *recordingID in recordingIDs) {
        MovieStop(sess.obj, (__bridge CFStringRef)recordingID, status);
    }
    return recordingIDs.count;
}

static void InterruptBegin(int reason, const char *why) {
    if (!gSessionLayer.pumpQueue) {
        return;
    }
    dispatch_async(gSessionLayer.pumpQueue, ^{
        for (SimcamSession *sess in SessAll()) {
            if (!sess.obj || !sess.running || sess.interrupted) {
                continue;
            }
            // audio interruptions only hit sessions with an audio input
            if (reason == AVCaptureSessionInterruptionReasonAudioDeviceInUseByAnotherClient && !sess.wantsMic) {
                continue;
            }
            sess.interrupted = YES;
            sess.interruptReason = reason;
            SessStopPump(sess);
            NSUInteger stopped = StopSessionRecordings(sess, AVErrorSessionWasInterrupted);
            LOG(@"interrupt session %p reason %d (%s), %lu recordings stopped", sess.obj, reason, why,
                (unsigned long)stopped);
            for (SimcamSink *sink in sess.sinks) {
                if ([sink.kind isEqualToString:@"preview"]) {
                    SessPost(sess.obj, @"DidStopPreviewing", @{@"SectionID": sink.sinkID});
                }
            }
            // in-flight photos complete first; photo blocks never wait on the pump queue. Holds the pump during the
            // encode (a known gap)
            if (PhotoDrainOn()) {
                dispatch_sync(PhotoQueue(), ^{});
            }
            SessPost(sess.obj, @"DidStopRunning",
                     @{@"ErrorStatus": @(kFigInterruptedStatus), @"ErrorReason": @(reason)});
        }
    });
}

// Restarts only sessions still interrupted: the app's own stop or start during the interruption already ended it for
// that session (sess_StopRunning / sess_StartRunning). Both run on the pump queue, so either order with this one is
// consistent.
static void InterruptEnd(int reason, const char *why) {
    if (!gSessionLayer.pumpQueue) {
        return;
    }
    dispatch_async(gSessionLayer.pumpQueue, ^{
        for (SimcamSession *sess in SessAll()) {
            if (!sess.obj || !sess.interrupted || (reason && sess.interruptReason != reason)) {
                continue;
            }
            sess.interrupted = NO;
            sess.running = YES;
            long long configurationID = [SafeKVC(sess.config, @"configurationID") longLongValue];
            LOG(@"interruption ended session %p (%s)", sess.obj, why);
            for (SimcamSink *sink in sess.sinks) {
                if ([sink.kind isEqualToString:@"preview"]) {
                    sink.frames = 0;   // re-announce DidStartPreviewing
                }
            }
            SessPost(sess.obj, @"DidStartRunning", @{@"ConfigurationID": @(configurationID)});
            MicUpdate(sess);
            PumpStart(sess);
        }
    });
}

// Stop every active recording with status (sentinel movie latch, movie-error control event); the session keeps
// running. The session walk runs on the pump queue (as InterruptBegin's and SessInjectRuntimeError's do): that is where
// finalize clears sess.obj (SessForget), so a session can't be freed between reading sess.obj and stopping its
// recording.
// (The app's own FileSinkStop calls MovieStop on AVFoundation's thread with a session it holds.)
static void MovieStopAll(int status) {
    if (!gSessionLayer.pumpQueue) {
        return;
    }
    dispatch_async(gSessionLayer.pumpQueue, ^{
        for (SimcamSession *sess in SessAll()) {
            if (!sess.obj) {
                continue;
            }
            StopSessionRecordings(sess, status);
        }
    });
}

static void SessInjectRuntimeError(int status, BOOL serverDied) {
    if (!gSessionLayer.pumpQueue) {
        return;
    }
    dispatch_async(gSessionLayer.pumpQueue, ^{
        int errorStatus = status;
        BOOL decided = status != kSentinelLatchAuto;
        for (SimcamSession *sess in SessAll()) {
            if (!sess.obj) {
                continue;
            }
            if (serverDied) {
                LOG(@"inject ServerConnectionDied on session %p", sess.obj);
                SessPost(sess.obj, @"ServerConnectionDied", @{});
                continue;
            }
            if (!sess.running) {
                continue;
            }
            if (!decided) {   // only once a running session is found
                errorStatus = SentinelSessionErrorAllowed() ? kSentinelLatchStatus : 0;
                decided = YES;
            }
            LOG(@"%@ on session %p",
                errorStatus ? [NSString stringWithFormat:@"inject runtime error %d", errorStatus]
                   : @"stop (session latched; no runtime error: one in the last 5 s)",
                sess.obj);
            SessStopPump(sess);
            StopSessionRecordings(sess, errorStatus);
            // 0: a plain stop, no runtime error
            SessPost(sess.obj, @"DidStopRunning", errorStatus ? @{@"ErrorStatus": @(errorStatus)} : nil);
        }
    });
}

static void ControlHandleEvent(NSString *event) {
    NSArray *parts = [event componentsSeparatedByString:@":"];
    NSString *name = parts[0];
    int arg = parts.count > 1 ? [parts[1] intValue] : 0;

    if ([name isEqualToString:@"interrupt"]) {
        InterruptBegin(arg ?: AVCaptureSessionInterruptionReasonVideoDeviceInUseByAnotherClient, "control file");
    } else if ([name isEqualToString:@"end"]) {
        InterruptEnd(0, "control file");
    } else if ([name isEqualToString:@"runtime-error"]) {
        SessInjectRuntimeError(arg ?: AVErrorMediaServicesWereReset, NO);
    } else if ([name isEqualToString:@"server-died"]) {
        SessInjectRuntimeError(0, YES);
    } else if ([name isEqualToString:@"photo-error"]) {
        // test probes for mid-session error completion (what a latch does to an op already running): the next photo
        // (photo-error) or every active recording (movie-error) ends with this status
        atomic_store(&gPhotoErrNext, arg ?: kSentinelLatchStatus);
        LOG(@"control: next photo fails with %d", arg ?: kSentinelLatchStatus);
    } else if ([name isEqualToString:@"movie-error"]) {
        int errorStatus = arg ?: kSentinelLatchStatus;
        MovieStopAll(errorStatus);
        LOG(@"control: stopping active recordings with %d", errorStatus);
    } else {
        LOG(@"control: unknown event %@", event);
    }
}

// ---- control file ----
// the control file's lines, in order, as key=value: each line is trimmed; blank lines, # comments and lines without
// '=' are skipped; the key is what comes before the first '=', the value everything after it
static void ControlForEachKeyValue(NSString *text, void (^each)(NSString *key, NSString *value)) {
    for (NSString *rawLine in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *line = [rawLine stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSRange equals = [line rangeOfString:@"="];
        if (!line.length || [line hasPrefix:@"#"] || equals.location == NSNotFound) {
            continue;
        }
        each([line substringToIndex:equals.location], [line substringFromIndex:equals.location + 1]);
    }
}

static BOOL ControlSameMtime(struct timespec a, struct timespec b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static void ControlPoll(const char *path) {
    struct stat fileStat;

    if (stat(path, &fileStat) != 0) {
        gControl.poll.isFirst = NO;
        return;
    }

    if (ControlSameMtime(fileStat.st_mtimespec, gControl.poll.mtime)) {
        return;
    }
    gControl.poll.mtime = fileStat.st_mtimespec;
    NSString *text = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    if (!text) {
        return;
    }
    ControlForEachKeyValue(text, ^(NSString *key, NSString *value) {
        if ([key isEqualToString:@"source"]) {
            SceneSetSource(value, "control file");
        } else if ([key isEqualToString:@"orientation"]) {
            long orientation = OrientParse(value);
            if (orientation != atomic_load(&gOrientation.deviceForced)) {
                OrientApply(orientation, "control file");
            }
        } else if ([key isEqualToString:@"event"]) {
            if (![value isEqualToString:gControl.poll.lastEvent]) {
                gControl.poll.lastEvent = value;
                NSString *eventName = [value componentsSeparatedByString:@"@"][0];
                if (gControl.poll.isFirst) {
                    LOG(@"control: event %@ present at start, not replayed", value);
                } else {
                    ControlHandleEvent(eventName);
                }
            }
        }
    });

    gControl.poll.isFirst = NO;
}

// ---- the control setup, split over activation's phases: the control file read (lookups step), the env/control-derived
// state (resolve step), plain stores (SWAP, first), enqueues (SWAP, after the installs), then the control timer and
// observers (START). The first frame after ARMED already uses SIMCAM_SOURCE / SIMCAM_ORIENT and the control file's
// source= / orientation= (which override the env, as at the first poll), even if START is late. The first two steps'
// results are gControl.atPrepare and gControl.atSwap.

// SIMCAM_TEST_PHOTO_ERR=<status>: the first photo fails with it (stored at SWAP with the rest)
static void ControlResolveTestFaults(void) {
    if (!TestFaultOn(ENV_TEST_PHOTO_ERR)) {
        return;
    }
    gControl.atSwap.photoError = TestFaultInt(ENV_TEST_PHOTO_ERR);
    gControl.atSwap.photoErrorSet = YES;
}

static void ControlReadAtPrepare(void) {   // lookups step: one stat and parse, as ControlPoll does, applying nothing
    const char *controlPath = getenv(ENV_CONTROL);
    if (!controlPath || !ControlFileOn()) {
        return;
    }
    struct stat fileStat;
    if (stat(controlPath, &fileStat) != 0) {
        return;
    }
    NSString *text = [NSString stringWithContentsOfFile:@(controlPath) encoding:NSUTF8StringEncoding error:nil];
    gControl.atPrepare.read = YES;
    gControl.atPrepare.mtime = fileStat.st_mtimespec;
    ControlForEachKeyValue(text, ^(NSString *key, NSString *value) {
        if ([key isEqualToString:@"source"]) {
            if (gControl.atPrepare.source) {
                CFRelease(gControl.atPrepare.source);
            }
            gControl.atPrepare.source = (void *)CFBridgingRetain([value copy]);
        } else if ([key isEqualToString:@"orientation"]) {
            gControl.atPrepare.orient = OrientParse(value);
            gControl.atPrepare.hasOrient = YES;
        } else if ([key isEqualToString:@"event"]) {
            if (gControl.atPrepare.event) {
                CFRelease(gControl.atPrepare.event);
            }
            gControl.atPrepare.event = (void *)CFBridgingRetain([value copy]);
            LOG(@"control: event %@ present at start, not replayed", value);
        }
    });

    LOG(@"control file %s read at activation (source=%@ orientation=%@)", controlPath,
        gControl.atPrepare.source ? (__bridge NSString *)gControl.atPrepare.source : @"-",
        gControl.atPrepare.hasOrient ? @(gControl.atPrepare.orient) : @"-");
}

// resolve step: allocate, parse; objects SWAP stores are retained here and kept as void *
static void ControlResolve(void) {
    gScene.request.lock = [NSObject new];
    ControlResolveTestFaults();
    const char *sourceEnv = getenv(ENV_SOURCE);
    if (gControl.atPrepare.source) {
        gControl.atSwap.sourceSpec = gControl.atPrepare.source;
        gControl.atPrepare.source = NULL;
    } else if (sourceEnv) {
        gControl.atSwap.sourceSpec = (void *)CFBridgingRetain(@(sourceEnv));
    }
    const char *orientEnv = getenv(ENV_ORIENT);
    if (gControl.atPrepare.hasOrient) {
        gControl.atSwap.orientation = gControl.atPrepare.orient;
        gControl.atSwap.orientationReason = "control file";
    } else if (orientEnv) {
        gControl.atSwap.orientation = OrientParse(@(orientEnv));
        gControl.atSwap.orientationReason = "SIMCAM_ORIENT";
    }
}

// SWAP, first: plain stores only (no lock, no log, no message): no session is adopted yet
static void ControlSwap(void) {
    if (gControl.atSwap.sourceSpec) {   // what SceneSetSource does
        *(void **)(void *)&gScene.request.spec = gControl.atSwap.sourceSpec;
        gControl.atSwap.sourceSpec = NULL;
        atomic_fetch_add(&gScene.request.gen, 1);
    }
    if (gControl.atSwap.photoErrorSet) {
        atomic_store(&gPhotoErrNext, gControl.atSwap.photoError);
    }
    if (gControl.atSwap.orientation) {
        atomic_store(&gOrientation.deviceForced, gControl.atSwap.orientation);
    }
    if (gControl.atPrepare.read) {
        gControl.poll.mtime = gControl.atPrepare.mtime;
        if (gControl.atPrepare.event) {
            *(void **)(void *)&gControl.poll.lastEvent = gControl.atPrepare.event;
            gControl.atPrepare.event = NULL;
        }
    }
    gControl.poll.isFirst = NO;   // START's first poll returns on the unchanged mtime; the next real event= is replayed
}

static void OrientApplyPendingF(void *ctx) {
    OrientApply(gControl.atSwap.orientation, gControl.atSwap.orientationReason);
}

static void SceneQRWarmF(void *ctx) {
    @autoreleasepool {
        SceneQRWarm();
    }
}

static void ControlEnqueue(void) {   // SWAP, after the installs: nothing waits on these
    if (gControl.atSwap.orientation) {
        dispatch_async_f(dispatch_get_main_queue(), NULL, OrientApplyPendingF);
    }
    // build the scene QR off the first pump tick
    if (SceneGeneratedOn()) {
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL, SceneQRWarmF);
    }
}

// the control file is polled every 250 ms (the log line says so), give or take 50 ms
enum { kControlPollIntervalMs = 250, kControlPollLeewayMs = 50 };

static void ControlStart(void) {   // START: the control-file timer, observers
    const char *controlPath = getenv(ENV_CONTROL);

    if (controlPath && ControlFileOn()) {
        char *path = strdup(controlPath);
        dispatch_queue_t queue = dispatch_queue_create("simcam.control", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, kControlPollIntervalMs * NSEC_PER_MSEC,
                                  kControlPollLeewayMs * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{
            @autoreleasepool {
                ControlPoll(path);
            }
        });
        dispatch_resume(timer);
        static dispatch_source_t keep;
        keep = timer;
        (void)keep;
        LOG(@"control file %s (polled every 250 ms)", path);
    }

    if (InterruptionsOn()) {
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        // device: an app's capture session is interrupted (reason 1, VideoDeviceNotAvailableInBackground) when it
        // leaves the foreground, and resumes when it comes back
        [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) {
                            InterruptBegin(AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableInBackground,
                                           "app entered background");
                        }];
        [center addObserverForName:UIApplicationWillEnterForegroundNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) {
                            InterruptEnd(AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableInBackground,
                                         "app will enter foreground");
                        }];
        // audio session interruption (phone call, Siri, another app's non-mixable session): sessions with audio get
        // reason 2
        [center addObserverForName:@"AVAudioSessionInterruptionNotification" object:nil queue:nil
                    usingBlock:^(NSNotification *note) {
                        long type = [note.userInfo[@"AVAudioSessionInterruptionTypeKey"] longValue];
                        LOG(@"AVAudioSession interruption type %ld", type);
                        AVCaptureSessionInterruptionReason audio =
                            AVCaptureSessionInterruptionReasonAudioDeviceInUseByAnotherClient;
                        if (type == AVAudioSessionInterruptionTypeBegan) {
                            InterruptBegin(audio, "audio session interruption began");
                        } else {
                            InterruptEnd(audio, "audio session interruption ended");
                        }
                    }];
        LOG(@"lifecycle interruptions armed");
    }

    if (gScene.request.spec) {
        LOG(@"source -> %@ (at activation)", gScene.request.spec);
    }
    if (gControl.atSwap.orientation) {
        LOG(@"orientation %ld forced at activation (%s)", gControl.atSwap.orientation,
            gControl.atSwap.orientationReason);
    }
}
