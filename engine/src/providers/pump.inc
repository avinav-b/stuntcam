// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): frame pump, movie file sinks.
//
// Provides: sess_SetConfiguration, sess_StartRunning, sess_StopRunning, sess_SetSectionProperty,
//           sess_CopySectionProperty, sess_File{Start,Stop,Pause,Resume}, the session slots (fig/session_hook.inc)
//           PumpStart, MovieStop (core/control.inc)
// Owns:     nothing shared.
// Uses:     fig/sources.inc, fig/session_state.inc, core/preflight.inc, core/safety.inc, core/trace.inc,
//           core/control.inc, providers/video.inc, providers/webcam.inc, providers/webcam_path.inc,
//           providers/scene.inc, providers/mirror.inc, providers/barcodes.inc, providers/audio.inc
#include "../core/trace.h"
#include "../fig/session.h"
#include "pump.h"
#include "webcam.h"
#include "scene.h"
#include "barcodes.h"
#include "audio.h"
#include "../core/control.h"
#include "mirror.h"
#include "../core/sentinel.h"
#include "../core/preflight.h"

// ---- pump ---------------------------------------------------------------------
// Metadata outputs get a sample, with barcode detection run again, on every 5th pump frame only (6 a second at 30 fps).
enum { kMetadataEveryNFrames = 5 };
// the pump's highest frame rate, whatever the device's active format allows
#define kPumpMaxFps 60
// the first tick's delay after a start, unless SIMCAM_FIRST_TICK_MS sets it (see PumpStart)
#define kPumpFirstTickDefaultMs 10
// the watchdog's limit for a session slot call (SetConfiguration, StartRunning, StopRunning), seconds
#define kSessionOpTimeoutS 2.0

// One frame to a video data output: the webcam ring's frame when it serves this sink, else src converted (turned,
// mirrored, scaled) for it, enqueued on the sink's local queue.
static void PumpTickVideoData(SimcamSink *sink, CVPixelBufferRef src, CMTime pts) {
    CVPixelBufferRef frame = WebcamSinkOutput(sink, NO);   // webcam: straight from the ring frame
    if (!frame) {
        frame = SinkConvert(sink, src);
    }
    CMSampleBufferRef sampleBuffer = SinkSampleBuffer(frame, pts);
    LocalQueueMsg msg = { kLocalQueueMsgSampleBuffer, sampleBuffer, {0, 0} };
    OSStatus status = pFigLocalQueueEnqueue(sink.queue, &msg, NULL);
    if (sink.frames++ < 2 || status) {
        LOG(@"vdo %@ frame %d %dx%d '%.4s' -> %d", sink.sinkID, sink.frames, sink.width, sink.height,
            (char *)&(OSType){CFSwapInt32HostToBig(sink.pixelFormat)}, (int)status);
    }
    CFRelease(sampleBuffer);
    CVPixelBufferRelease(frame);
}

// One frame to a preview: the webcam ring's frame (mirroring included) when it serves this sink, else src converted
// and, for a mirrored front preview, flipped as on a device.
static void PumpTickPreview(SimcamSession *sess, SimcamSink *sink, CVPixelBufferRef src) {
    CVPixelBufferRef frame = WebcamSinkOutput(sink, YES);   // webcam: straight from the ring frame, mirroring included

    // a mirrored preview (the front camera), as on a device
    NSArray *note = !frame && OrientVirtualOn() ? PreviewNoteFor(sink.sinkID) : nil;
    if (!frame) {
        frame = SinkConvert(sink, src);
    }
    if ([note[1] boolValue]) {
        VTPixelRotationSessionRef flipSess = sink.flipSess;
        int flipKind = sink.flipKind;
        CVPixelBufferRef flipped = MirrorFlip(frame, [note[0] intValue], &flipSess, &flipKind);
        sink.flipSess = flipSess;
        sink.flipKind = flipKind;
        CVPixelBufferRelease(frame);
        frame = flipped;
    }
    PreviewPush(sess.obj, sink, frame);
    CVPixelBufferRelease(frame);
}

// A metadata output's sample: the codes detected in src, each stamped with the frame's time, as the buffer's barcode
// attachments.
static void PumpTickMetadata(SimcamSink *sink, CVPixelBufferRef src, CMTime pts) {
    if (SceneGeneratedOn()) {
        BarcodeDetectScene(src);
    } else {
        BarcodeDetectFixture(src);
    }
    CMSampleBufferRef sampleBuffer = SinkSampleBuffer(src, pts);
    NSMutableArray *barcodes = [NSMutableArray array];
    for (NSDictionary *barcode in gBarcodes.dicts) {
        NSMutableDictionary *stamped = [barcode mutableCopy];
        stamped[@"Timestamp"] = (__bridge_transfer id)CMTimeCopyAsDictionary(pts, NULL);
        [barcodes addObject:stamped];
    }
    CMSetAttachment(sampleBuffer, CFSTR("BarcodeMetadataObjectsCount"), (__bridge CFTypeRef)@(barcodes.count),
                    kCMAttachmentMode_ShouldPropagate);
    CMSetAttachment(sampleBuffer, CFSTR("BarcodeMetadataObjectsArray"), (__bridge CFTypeRef)barcodes,
                    kCMAttachmentMode_ShouldPropagate);
    LocalQueueMsg msg = { kLocalQueueMsgSampleBuffer, sampleBuffer, {0, 0} };
    OSStatus status = pFigLocalQueueEnqueue(sink.queue, &msg, NULL);
    if (sink.frames++ < 2 || status) {
        LOG(@"metadata %@ sbuf %lu codes -> %d", sink.sinkID, (unsigned long)barcodes.count, (int)status);
    }
    CFRelease(sampleBuffer);
}

// One pump tick: render the source frame (it becomes gPump.latest, the photo grab's frame), hand it to every video data
// output, preview and (every kMetadataEveryNFrames-th frame) metadata output, then to every active recording.
static void PumpTick(SimcamSession *sess) {
    uint64_t webcamTickStart = WebcamIsOn() ? WebcamNowNs() : 0;
    CVPixelBufferRef src = SceneGeneratedOn() ? SceneRenderSource(sess) : SceneRenderFixture();
    if (gPump.latest) {
        CVPixelBufferRelease(gPump.latest);
    }
    gPump.latest = CVPixelBufferRetain(src);

    // webcam: the frame's capture time
    CMTime pts = WebcamIsOn() ? WebcamPts() : CMClockGetTime(CMClockGetHostTimeClock());

    for (SimcamSink *sink in sess.sinks) {
        if ([sink.kind isEqualToString:@"vdo"] && sink.queue) {
            PumpTickVideoData(sink, src, pts);
        } else if ([sink.kind isEqualToString:@"preview"]) {
            PumpTickPreview(sess, sink, src);
        } else if ([sink.kind isEqualToString:@"metadata"] && sink.queue &&
                   (gPump.frameNo % kMetadataEveryNFrames == 0)) {
            PumpTickMetadata(sink, src, pts);
        }
    }

    NSArray *recs;
    @synchronized (sess.recs) {
        recs = sess.recs.allValues;
    }

    // a lock per recording (a stop races the pump)
    for (SimcamRecording *recording in recs) {
        @synchronized (recording) {
            MovieAppendVideo(recording, src, pts);
        }
    }

    CVPixelBufferRelease(src);
    if (webcamTickStart) {
        WebcamTickEnd(webcamTickStart);
    }
}

// The session's active format: its size, frame rate and device position, read from the genuine AVCaptureDevice.
static void PumpReadActiveFormat(SimcamSession *sess) {
    // The Fig source configuration carries no format in the simulator ("/0x0, ?-?"), so the genuine AVCaptureDevice
    // behind the connection is read instead. sourceID is the string "<AVCaptureFigVideoDevice 0x...>", matched against
    // the discovered devices (no pointer is dereferenced).
    sess.formatWidth = kSrcW;
    sess.formatHeight = kSrcH;
    sess.fps = kPumpDefaultFps;
    if (!ActiveFormatOn()) {
        return;   // off switch: the format's size and rate are not read (1080p30 stays)
    }
    for (id connection in SafeKVC(sess.config, @"connectionConfigurations")) {
        if ([SafeKVC(connection, @"mediaType") unsignedIntValue] == 'soun') {
            continue;
        }
        id sourceID = SafeKVC(SafeKVC(connection, @"sourceConfiguration"), @"sourceID");
        AVCaptureDevice *device = [sourceID isKindOfClass:[AVCaptureDevice class]] ? sourceID : nil;
        if (!device && [sourceID isKindOfClass:[NSString class]]) {
            NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera,
                               AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInDualCamera,
                               AVCaptureDeviceTypeBuiltInDualWideCamera, AVCaptureDeviceTypeBuiltInTripleCamera,
                               AVCaptureDeviceTypeBuiltInTrueDepthCamera, AVCaptureDeviceTypeBuiltInLiDARDepthCamera];
            static NSArray *allDevices;
            static dispatch_once_t once;
            dispatch_once(&once, ^{
                allDevices = [AVCaptureDeviceDiscoverySession
                                 discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo
                                                        position:AVCaptureDevicePositionUnspecified].devices;
            });
            for (AVCaptureDevice *candidate in allDevices) {
                if ([sourceID isEqualToString:[NSString stringWithFormat:@"<%@ %p>",
                                                                         NSStringFromClass([candidate class]),
                                                                         candidate]]) {
                    device = candidate;
                }
            }
            if (!device) {
                LOG(@"active format: sourceID %@ matched no device", sourceID);
            }
        }
        if (!device) {
            continue;
        }
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription);
        if (dims.width > 0) {
            sess.formatWidth = dims.width;
            sess.formatHeight = dims.height;
        }
        sess.position = device.position;   // front or back decides the buffer content's rotation
        CMTime minFrameDuration = device.activeVideoMinFrameDuration;
        if (CMTIME_IS_VALID(minFrameDuration) && minFrameDuration.value > 0) {
            sess.fps = MIN(kPumpMaxFps, round(1.0 / CMTimeGetSeconds(minFrameDuration)));
        }
        LOG(@"active format: device=%@ %dx%d minFD=%.4f", device.localizedName, sess.formatWidth, sess.formatHeight,
            CMTimeGetSeconds(minFrameDuration));
        break;
    }
}

// A sink for one connection configuration: the output's size and format (else the active format's size, BGRA), and
// its kind from the sink configuration's class (vdo, preview, metadata, photo, movie, other), with what that kind
// reads.
static SimcamSink *SinkFromConnection(SimcamSession *sess, id connection, id sinkConfig, NSString *cls) {
    SimcamSink *sink = [SimcamSink new];
    sink.sinkID = SafeKVC(sinkConfig, @"sinkID");
    sink.width = sess.formatWidth;
    sink.height = sess.formatHeight;
    sink.pixelFormat = kCVPixelFormatType_32BGRA;
    id outputWidth = SafeKVC(connection, @"outputWidth");
    id outputHeight = SafeKVC(connection, @"outputHeight");
    id outputFormat = SafeKVC(connection, @"outputFormat");

    if ([outputWidth intValue] > 0 && [outputHeight intValue] > 0) {
        sink.width = [outputWidth intValue];
        sink.height = [outputHeight intValue];
    }

    if ([outputFormat unsignedIntValue]) {
        sink.pixelFormat = [outputFormat unsignedIntValue];
    }

    // 17.5 carries the connection angle as `orientation` (an enum), 26 and 27 as `rotationDegrees`;
    // SettingsRotationDeg reads both
    if ([cls containsString:@"VideoData"]) {
        sink.kind = @"vdo";
        sink.dataOutputRotationDeg = SettingsRotationDeg(connection, @"rotationDegrees", @"orientation", sess.position);
        sink.mirrored = [SafeKVC(connection, @"mirroringEnabled") boolValue];
    } else if ([cls containsString:@"Preview"]) {
        sink.kind = @"preview";
        sink.pixelFormat = kCVPixelFormatType_32BGRA;
        sink.width = kSrcW;
        sink.height = kSrcH;
        sink.rotation = [SafeKVC(connection, @"rotationDegrees") floatValue];
        // override: a fixed angle
        if (getenv(ENV_PREVIEW_ROTATION)) {
            sink.rotation = atof(getenv(ENV_PREVIEW_ROTATION));
        }
    } else if ([cls containsString:@"Metadata"]) {
        sink.kind = @"metadata";
    } else if ([cls containsString:@"Iris"] || [cls containsString:@"StillImage"]) {
        sink.kind = @"photo";
        CMVideoDimensions maxDims = DimsFromValue(SafeKVC(sinkConfig, @"maxPhotoDimensions"));
        if (maxDims.width > 0) {
            gSessionLayer.irisMaxDims = maxDims;
        }
    } else if ([cls containsString:@"MovieFile"] || [cls containsString:@"FileSink"]) {
        sink.kind = @"movie";
    } else {
        sink.kind = @"other";
    }

    return sink;
}

static BOOL ConnectionIsAudio(id connection) {
    return [NSStringFromClass([connection class]) containsString:@"Audio"] ||
           [SafeKVC(connection, @"mediaType") unsignedIntValue] == 'soun';
}

// An audio connection: the session wants the microphone. A movie sink takes the audio into its file (noted in
// audioMovieSinks; no sink of its own), an audio data output becomes an "ado" sink, and anything else is ignored.
// Returns whether sink is kept.
static BOOL SinkRouteAudio(SimcamSession *sess, SimcamSink *sink, NSString *cls) {
    sess.wantsMic = YES;
    if ([sink.kind isEqualToString:@"movie"]) {
        [sess.audioMovieSinks addObject:sink.sinkID];
        LOG(@"audio connection -> movie sink %@", sink.sinkID);
        return NO;
    }
    if (![cls containsString:@"AudioData"]) {
        LOG(@"audio connection -> %@ sink ignored", cls);
        return NO;
    }
    sink.kind = @"ado";   // AVCaptureAudioDataOutput
    return YES;
}

// vdo, metadata and ado sinks get their buffers through a Fig local queue, announced to AVF (LocalQueueUpdated)
static void SinkCreateLocalQueue(SimcamSession *sess, SimcamSink *sink) {
    BOOL takesLocalQueue = [sink.kind isEqualToString:@"vdo"] || [sink.kind isEqualToString:@"metadata"] ||
                           [sink.kind isEqualToString:@"ado"];
    if (!takesLocalQueue) {
        return;
    }
    CFTypeRef queue = NULL;
    OSStatus status = pFigLocalQueueCreate(NULL, 0, &queue);
    if (!status && queue) {
        sink.queue = queue;
        SessPost(sess.obj, @"LocalQueueUpdated", @{@"SectionID": sink.sinkID, @"LocalQueue": (__bridge id)queue});
    }
}

// The session's sinks, rebuilt from its configuration (at each start, and at each commit while running): the old
// sinks' local queues are released, then there is one sink per connection.
static void PumpWireSinks(SimcamSession *sess) {
    for (SimcamSink *sink in sess.sinks) {
        if (sink.queue) {
            CFRelease(sink.queue);
        }
    }

    sess.sinks = [NSMutableArray array];
    sess.audioMovieSinks = [NSMutableSet set];
    sess.wantsMic = NO;
    PumpReadActiveFormat(sess);

    @try {
        for (id connection in SafeKVC(sess.config, @"connectionConfigurations")) {
            id sinkConfig = SafeKVC(connection, @"sinkConfiguration");
            NSString *cls = NSStringFromClass([sinkConfig class]);
            DiagDumpProps(connection);
            DiagDumpProps(sinkConfig);
            SimcamSink *sink = SinkFromConnection(sess, connection, sinkConfig, cls);
            if (ConnectionIsAudio(connection) && !SinkRouteAudio(sess, sink, cls)) {
                continue;
            }
            LOG(@"sink %@ kind=%@ id=%@ %dx%d fmt=%u rot=%.0f connRot=%@ conn=%@", cls, sink.kind, sink.sinkID,
                sink.width, sink.height, (unsigned)sink.pixelFormat, sink.rotation,
                SafeKVC(connection, @"rotationDegrees"), NSStringFromClass([connection class]));
            SinkCreateLocalQueue(sess, sink);
            [sess.sinks addObject:sink];
        }
    } @catch (NSException *exception) {
        LOG(@"EXC wiring sinks: %@", exception);
    }
}

// Starts the session's pump timer at its frame rate, or restarts it when the rate changed.
static void PumpStart(SimcamSession *sess) {
    double fps = sess.fps > 0 ? sess.fps : kPumpDefaultFps;
    if (sess.timer && gPump.fps == fps) {
        return;   // restart at the active format's frame rate
    }
    if (sess.timer) {
        dispatch_source_cancel(sess.timer);
        sess.timer = nil;
    }

    gPump.fps = fps;
    sess.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gSessionLayer.pumpQueue);

    // The first tick comes 10 ms after the start. 100, 30, 10 and 0 ms were measured on 26.3 and 27.0 (32 launches):
    // no enqueue errors or dropped frames at any value; 10 ms gives the first frames ~100 ms sooner, and 0 adds
    // nothing. SIMCAM_FIRST_TICK_MS overrides it.
    static long firstTickMs = -1;

    if (firstTickMs < 0) {
        const char *firstTickEnv = getenv(ENV_FIRST_TICK_MS);
        firstTickMs = firstTickEnv ? MAX(0, atol(firstTickEnv)) : kPumpFirstTickDefaultMs;
    }

    dispatch_source_set_timer(sess.timer, dispatch_time(DISPATCH_TIME_NOW, firstTickMs * NSEC_PER_MSEC),
                              (uint64_t)(NSEC_PER_SEC / fps), 0);
    __weak SimcamSession *weakSess = sess;
    dispatch_source_set_event_handler(sess.timer, ^{
        SimcamSession *live = weakSess;
        if (live && live.running) {
            @autoreleasepool {
                PumpTick(live);
            }
        }
    });

    dispatch_resume(sess.timer);
}

// Slot kSessSlotSetConfiguration: the new configuration is committed; a running session rewires its sinks to it.
static OSStatus sess_SetConfiguration(CMBaseObjectRef session, id config) {
    int sessionOp = SentinelOpBegin("session", kSessionOpTimeoutS);
    SimcamSession *sess = SessLookup(session);
    long long configID = [SafeKVC(config, @"configurationID") longLongValue];
    LOG(@"session %p SetConfiguration id=%lld\n%@", session, configID, DescribeShort(config, 4000));
    dispatch_sync(gSessionLayer.pumpQueue, ^{
        sess.config = config;
        SessPost(session, @"ConfigurationCommitted", @{@"ConfigurationID": @(configID)});
        if (sess.running) {
            PumpWireSinks(sess);
            MicUpdate(sess);
            PumpStart(sess);
            SessPost(session, @"ConfigurationDidBecomeLive", @{@"ConfigurationID": @(configID)});
        }
    });
    SentinelOpEnd(sessionOp);
    return 0;
}

// Slot kSessSlotStartRunning: the session runs, its sinks wired and its pump started.
static OSStatus sess_StartRunning(CMBaseObjectRef session) {
    // A stop of this session still waiting for photo callbacks (sess_StopRunning) is reported first, so AVF never gets
    // this DidStartRunning before that DidStopRunning.
    if (StopWaitFlush(session)) {
        LOG(@"session %p StartRunning: a deferred stop was outstanding; reported it first", session);
    }
    int sessionOp = SentinelOpBegin("session", kSessionOpTimeoutS);
    SimcamSession *sess = SessLookup(session);
    long long configID = [SafeKVC(sess.config, @"configurationID") longLongValue];
    LOG(@"session %p StartRunning (cid=%lld)", session, configID);
    dispatch_sync(gSessionLayer.pumpQueue, ^{
        // A start during an interruption ends it here: this DidStartRunning is the one AVF waits for, and a second one
        // from InterruptEnd would restart a session AVF thinks is running (AVF then forwards none of its stops).
        // SIMCAM_OFF_INTERRUPTION_RESET keeps the old behaviour.
        if (sess.interrupted && InterruptionResetOn()) {
            LOG(@"session %p started while interrupted: interruption over", session);
            sess.interrupted = NO;
        }
        sess.running = YES;
        SessPost(session, @"ConfigurationDidBecomeLive", @{@"ConfigurationID": @(configID)});
        SessPost(session, @"DidStartRunning", @{@"ConfigurationID": @(configID)});
        PumpWireSinks(sess);
        MicUpdate(sess);
        PumpStart(sess);
    });

    SentinelOpEnd(sessionOp);
    TraceRecord(kTraceOpSessionStart, -1, configID);

    // At most one runtime error per 5 s (SentinelSessionErrorAllowed), so an app that restarts on runtime errors can't
    // spin. It is process-wide, and SessInjectRuntimeError stops every running session: under a session latch none may
    // run anyway.
    if (SentinelLatched("session")) {
        LOG(@"session latched: stopping it (runtime error %d unless one was posted in the last 5 s)",
            kSentinelLatchStatus);
        SessInjectRuntimeError(kSentinelLatchAuto, NO);
    }

    return 0;
}

// The pump-queue half of a stop: the session stops running (microphone, pump timer) and its previews report stopped.
static void StopSessionOnPump(CMBaseObjectRef session, SimcamSession *sess) {
    // A stop during an interruption ends it for this session: AVF posts InterruptionEnded on the DidStopRunning that
    // follows, and InterruptEnd must not restart a session the app stopped. Otherwise AVF takes the unasked
    // DidStartRunning and never forwards the app's later start or stop, so the engine pumps until teardown (6 of 6 runs
    // on 26.3 and 27.0). SIMCAM_OFF_INTERRUPTION_RESET keeps the old behaviour.
    if (sess.interrupted && InterruptionResetOn()) {
        LOG(@"session %p stopped while interrupted: not restarted when the interruption ends", session);
        sess.interrupted = NO;
    }
    sess.running = NO;
    MicUpdate(sess);
    if (sess.timer) {
        dispatch_source_cancel(sess.timer);
        sess.timer = nil;
    }
    for (SimcamSink *sink in sess.sinks) {
        if ([sink.kind isEqualToString:@"preview"]) {
            SessPost(session, @"DidStopPreviewing", @{@"SectionID": sink.sinkID});
        }
    }
}

// how long the SIMCAM_TEST_FLUSH_RACE fault blocks the stopping thread, microseconds (50 ms)
enum { kFlushRaceStallUs = 50000 };

// Test fault SIMCAM_TEST_FLUSH_RACE: a start's flush lands between StopWaitBeginStop and the waiter's dispatch; it must
// return only after this stop's DidStopRunning is queued. Blocks the stopping thread for kFlushRaceStallUs.
static void TestFaultFlushRace(CMBaseObjectRef session) {
    long postedBefore = atomic_load(&gStopWait.stopsPosted);
    CFRetain(session);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL outstanding = StopWaitFlush(session);
        LOG(@"SIMCAM_TEST_FLUSH_RACE: flush in the window returned (outstanding=%d); this stop's DidStopRunning %s",
            outstanding,
            atomic_load(&gStopWait.stopsPosted) > postedBefore ? "was queued first (in order)"
                                                              : "NOT queued yet (out of order)");
        dispatch_async(gSessionLayer.pumpQueue, ^{
            CFRelease(session);
        });
    });
    usleep(kFlushRaceStallUs);
}

// DidStopRunning once the photos settingsIDs (of the stop stopId) have had their didFinishCapture, or after
// kStopWaitBoundS, or when a start flushes the stop. The wait runs on the session's own stop queue (StopWaitQueue),
// not on the stopping thread.
static void ReportStopAfterPhotoCallbacks(CMBaseObjectRef session, long long stopId, NSArray<NSNumber *> *settingsIDs) {
    CFRetain(session);
    long lateMs = TestFaultLong(ENV_TEST_LATE_WAITER_MS);
    dispatch_async(StopWaitQueue(session), ^{
        // test fault SIMCAM_TEST_LATE_WAITER_MS: the waiter runs <ms> late, as if starved of CPU, past a start's
        // kStopWaitBoundS flush bound
        if (lateMs > 0) {
            usleep((useconds_t)lateMs * 1000);
            LOG(@"SIMCAM_TEST_LATE_WAITER_MS: session %p stop waiter ran %ld ms late", session, lateMs);
        }
        uint64_t waitStartNs = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        BOOL flushed = NO;
        NSUInteger left = StopWaitForPhotos(stopId, settingsIDs, kStopWaitBoundS, &flushed);
        double waitedMs = (clock_gettime_nsec_np(CLOCK_MONOTONIC) - waitStartNs) / 1e6;
        if (flushed) {
            LOG(@"session %p StopRunning: flushed by a start after %.1f ms (%lu photo(s) still due)", session, waitedMs,
                (unsigned long)left);
        } else if (left) {
            LOG(@"session %p StopRunning: %lu photo(s) without didFinishCapture after %.1f s; stop reported anyway",
                session, (unsigned long)left, kStopWaitBoundS);
        } else {
            LOG(@"session %p StopRunning: in-flight photo callbacks done after %.1f ms", session, waitedMs);
        }
        SessPost(session, @"DidStopRunning", nil);
        StopWaitEndStop(stopId);   // after the post: a start's flush waits for this
        dispatch_async(gSessionLayer.pumpQueue, ^{
            CFRelease(session);   // a last release finalizes the session, which syncs to the pump queue
        });
    });
}

// Slot kSessSlotStopRunning: the session stops; DidStopRunning comes after the photos already requested complete.
static OSStatus sess_StopRunning(CMBaseObjectRef session) {
    TraceRecord(kTraceOpSessionStop, -1, 0);
    int sessionOp = SentinelOpBegin("session", kSessionOpTimeoutS);
    SimcamSession *sess = SessLookup(session);
    LOG(@"session %p StopRunning", session);
    dispatch_sync(gSessionLayer.pumpQueue, ^{
        StopSessionOnPump(session, sess);
    });

    SentinelOpEnd(sessionOp);

    // Photos requested before the stop complete before DidStopRunning. This is outside the session op, so a slow
    // encode isn't counted as a session hang. It can't deadlock: photo blocks only post asynchronously.
    BOOL drain = PhotoDrainOn();   // off switch: the canary's photo-then-stop must then fail
    if (drain) {
        dispatch_sync(PhotoQueue(), ^{});
    }

    // ... and after AVF has run their delegates' didFinishCapture (why: fig/session_state.inc). The wait runs on
    // a private queue, so AVF's stopRunning thread waits on its own condition, holding none of its locks, while the
    // delegate queue catches up. A stop on a photo delegate queue, main included (StopWaitOnDelegateQueue), doesn't
    // wait: the callbacks it would wait for run there after it. SIMCAM_OFF_STOP_WAIT reports the stop at once (the old
    // behaviour).
    if (drain && StopWaitOn()) {
        NSArray<NSNumber *> *settingsIDs = nil;
        long long stopId = 0;
        if (StopWaitOnDelegateQueue()) {
            if (StopWaitHasPending(session)) {
                LOG(@"session %p StopRunning on a photo delegate queue (%s): not waiting for its callbacks", session,
                    pthread_main_np() ? "main thread" : "marked queue");
            }
        } else {
            settingsIDs = StopWaitBeginStop(session, &stopId);
        }
        if (settingsIDs && TestFaultOn(ENV_TEST_FLUSH_RACE)) {
            TestFaultFlushRace(session);
        }
        if (settingsIDs) {
            ReportStopAfterPhotoCallbacks(session, stopId, settingsIDs);
            return 0;
        }
    }

    SessPost(session, @"DidStopRunning", nil);
    return 0;
}

// Slot kSessSlotSetSectionProperty: SetSectionProperty(session, sectionID, key, value). A rotation is noted for the
// previews; the original slot sets the property.
static OSStatus sess_SetSectionProperty(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                        CFTypeRef value) {
    SimcamSession *sess = SessLookup(session);
    LOG(@"SetSectionProperty %@ %@ = %@", section, key, DescribeShort((__bridge id)value, 200));
    // a rotation change applies to every preview sink, whatever the section
    if ([(__bridge NSString *)key containsString:@"Rotation"]) {
        for (SimcamSink *sink in sess.sinks) {
            if ([sink.kind isEqualToString:@"preview"]) {
                sink.rotation = [(__bridge id)value floatValue];
            }
        }
    }
    SessSetSectionPropertyFn original =
        (SessSetSectionPropertyFn)gSessionLayer.originalDerived[kSessSlotSetSectionProperty];
    return original(session, section, key, value);
}

// Slot kSessSlotCopySectionProperty: CopySectionProperty(session, sectionID, key, allocator, out). A recording's
// duration and file size are answered here; any other key goes to the original slot.
static OSStatus sess_CopySectionProperty(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                         CFAllocatorRef allocator, CFTypeRef *out) {
    SimcamSession *sess = SessLookup(session);
    SimcamRecording *recording;
    @synchronized (sess.recs) {
        recording = sess.recs[(__bridge NSString *)section];
    }

    NSString *name = (__bridge NSString *)key;

    if ([name hasSuffix:@"RecordedDuration"]) {
        CMTime duration =
            recording && recording.started ? CMTimeSubtract(recording.lastPts, recording.startPts) : kCMTimeZero;
        *out = CMTimeCopyAsDictionary(duration, allocator);
        return 0;
    }

    if ([name hasSuffix:@"RecordedSize"]) {
        NSNumber *size = @0;
        if (recording) {
            size = [[NSFileManager defaultManager] attributesOfItemAtPath:recording.url.path
                                                                    error:nil][NSFileSize] ?: @0;
        }
        *out = CFBridgingRetain(@(size.longLongValue));
        return 0;
    }

    static NSMutableSet *seenKeys;
    if (!seenKeys) {
        seenKeys = [NSMutableSet set];
    }

    if (![seenKeys containsObject:name]) {
        [seenKeys addObject:name];
        LOG(@"CopySectionProperty %@ %@ -> original", section, name);
    }

    SessCopySectionPropertyFn original =
        (SessCopySectionPropertyFn)gSessionLayer.originalDerived[kSessSlotCopySectionProperty];
    return original(session, section, key, allocator, out);
}

// A CoreMedia failure status for a movie that couldn't be written; AVF reports it to the app as AVErrorUnknown
// (-11800).
enum { kMovieFailedStatus = -12780 };
// the movie's audio when AVF's settings don't fit the source: AAC at this bit rate; and the sample rate when the
// source's is not known yet
#define kMovieAudioBitRate 64000
#define kMovieAudioDefaultRate 48000
// a movie with audio waits this long after its stop for the microphone's in-flight buffers (~20 ms buffer + ~20 ms
// lag) before finishing
#define kMovieAudioTailMs 80
// a movie ends one frame at this rate after its last frame's time
#define kMovieEndFrameRate 30

// A new recording for slot kSessSlotFileSinkStartRecording's settings: its file, rotation (*rotationOut, degrees
// clockwise, for the track transform), mirroring and size (the active format's, converted from the canvas when it
// differs). No writer yet.
static SimcamRecording *MovieRecordingNew(SimcamSession *sess, CFStringRef sinkID, id settings, float *rotationOut) {
    SimcamRecording *recording = [SimcamRecording new];
    recording.sinkID = (__bridge NSString *)sinkID;
    recording.settingsID = [SafeKVC(settings, @"settingsID") longLongValue];
    recording.url = SafeKVC(settings, @"outputURL");

    // a movie in the portrait-device orientation: a landscape buffer and a rotation transform (like a real device)
    *rotationOut = SettingsRotationDeg(settings, @"videoRotationDegrees", @"videoOrientation", sess.position);
    recording.rotationDeg = (int)*rotationOut;

    // mirrored only with the virtual orientation on
    recording.mirror = OrientVirtualOn() && [SafeKVC(settings, @"videoMirrored") boolValue];
    recording.width = sess.formatWidth ?: kSrcW;
    recording.height = sess.formatHeight ?: kSrcH;

    if (recording.width != kSrcW || recording.height != kSrcH) {
        SimcamSink *converter = [SimcamSink new];
        converter.width = recording.width;
        converter.height = recording.height;
        converter.pixelFormat = '420v';
        recording.converter = converter;
    }

    return recording;
}

// The recording's writer (replacing any file at its URL; recording.writer stays nil on failure, *err says why) and its
// video input: HEVC when the settings ask for it (and SIMCAM_OFF_ACTIVE_FORMAT is unset), else H.264, at the
// recording's size, turned by rotation. Returns the codec.
static NSString *MovieAddWriterAndVideo(SimcamRecording *recording, id settings, NSString *fileType, float rotation,
                                        NSError **err) {
    NSDictionary *videoSettings = SafeKVC(settings, @"videoSettings");
    NSString *codec = videoSettings[AVVideoCodecKey];
    if (![codec isEqual:AVVideoCodecTypeHEVC] || !ActiveFormatOn()) {
        codec = AVVideoCodecTypeH264;
    }
    [[NSFileManager defaultManager] removeItemAtURL:recording.url error:nil];
    recording.writer = [AVAssetWriter assetWriterWithURL:recording.url fileType:fileType error:err];
    NSDictionary *outputSettings =
        @{AVVideoCodecKey: codec, AVVideoWidthKey: @(recording.width), AVVideoHeightKey: @(recording.height)};
    AVAssetWriterInput *videoInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                         outputSettings:outputSettings];
    videoInput.expectsMediaDataInRealTime = YES;
    if (rotation) {
        videoInput.transform = CGAffineTransformMakeRotation(rotation * M_PI / 180.0);
    }
    [recording.writer addInput:videoInput];
    recording.adaptor =
        [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                                                         sourcePixelBufferAttributes:nil];
    return codec;
}

// The recording's audio input: AVF's audioSettings with the rate and channel count matched to the engine's audio
// source (tone or microphone) and no channel layout, else mono AAC at kMovieAudioBitRate; none if the writer takes
// neither.
static void MovieAddAudio(SimcamRecording *recording, id settings) {
    double rate = gAudio.source.format.mSampleRate > 0 ? gAudio.source.format.mSampleRate : kMovieAudioDefaultRate;
    NSMutableDictionary *avfSettings = [SafeKVC(settings, @"audioSettings") mutableCopy];

    if (avfSettings) {
        avfSettings[AVSampleRateKey] = @(rate);
        avfSettings[AVNumberOfChannelsKey] = @1;
        [avfSettings removeObjectForKey:AVChannelLayoutKey];
    }

    NSDictionary *defaultSettings = @{AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @(rate),
                                      AVNumberOfChannelsKey: @1, AVEncoderBitRateKey: @(kMovieAudioBitRate)};
    AVAssetWriterInput *audioInput = nil;

    for (NSDictionary *candidate in avfSettings ? @[avfSettings, defaultSettings] : @[defaultSettings]) {
        @try {
            if ([recording.writer canApplyOutputSettings:candidate forMediaType:AVMediaTypeAudio]) {
                audioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                outputSettings:candidate];
                LOG(@"movie audio input settings %@%@", candidate == defaultSettings ? @"(default) " : @"(from AVF) ",
                    DescribeShort(candidate, 300));
                break;
            }
        } @catch (NSException *exception) {
            LOG(@"movie audio settings rejected %@", exception.reason);
        }
    }

    audioInput.expectsMediaDataInRealTime = YES;

    if (audioInput && [recording.writer canAddInput:audioInput]) {
        [recording.writer addInput:audioInput];
        recording.audioInput = audioInput;
    } else {
        LOG(@"movie: no audio input (as=%@)", avfSettings);
    }
}

// Slot kSessSlotFileSinkStartRecording: FileSinkStartRecording(session, sinkID, settings). A new recording with its
// writer; the movie starts at the next pump frame.
static OSStatus sess_FileStart(CMBaseObjectRef session, CFStringRef sinkID, id settings) {
    SimcamSession *sess = SessLookup(session);
    LOG(@"FileSinkStartRecording %@ settings=%@", sinkID, DescribeShort(settings, 1500));
    DiagDumpProps(settings);
    float rotation;
    SimcamRecording *recording = MovieRecordingNew(sess, sinkID, settings, &rotation);
    NSString *fileType = SafeKVC(settings, @"outputFileType") ?: AVFileTypeQuickTimeMovie;
    NSError *err = nil;
    NSString *codec = MovieAddWriterAndVideo(recording, settings, fileType, rotation, &err);
    if ([sess.audioMovieSinks containsObject:recording.sinkID] && recording.writer && AudioOn()) {
        MovieAddAudio(recording, settings);
    }
    LOG(@"movie writer %dx%d %@ @%.0f fps audio=%@", recording.width, recording.height, codec, sess.fps,
        recording.audioInput ? @"yes" : @"no");
    if (!recording.writer) {
        LOG(@"movie writer failed %@", err);
        TraceRecord(kTraceOpMovieStop, recording.settingsID, kMovieFailedStatus);
        SessPost(session, @"DidStopRecording",
                 @{@"SectionID": recording.sinkID, @"SettingsID": @(recording.settingsID),
                   @"ErrorStatus": @(kMovieFailedStatus)});
        return 0;
    }

    @synchronized (sess.recs) {
        sess.recs[recording.sinkID] = recording;
    }

    SessPost(session, @"DidStartRecording",
             @{@"SectionID": recording.sinkID, @"SettingsID": @(recording.settingsID),
               @"MovieWidth": @(recording.width), @"MovieHeight": @(recording.height)});
    TraceRecord(kTraceOpMovieStart, recording.settingsID, 0);

    if (SentinelLatched("movie")) {
        LOG(@"movie latched: recording stops with %d", kSentinelLatchStatus);
        MovieStop(session, sinkID, kSentinelLatchStatus);
    }

    return 0;
}

// Slot kSessSlotFileSinkStopRecording: FileSinkStopRecording(session, sinkID).
// Test probe: SIMCAM_TEST_MOVIE_ERR=<status> makes every app-requested stop report that status (to map Fig statuses to
// AVF errors).
static OSStatus sess_FileStop(CMBaseObjectRef session, CFStringRef sinkID) {
    static int testStatus;   // the test status (0: none)
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        testStatus = TestFaultInt(ENV_TEST_MOVIE_ERR);
    });
    return MovieStop(session, sinkID, testStatus);
}

// The recording's file ends at endTime and is finished; done gets 0 when the writer completed, else
// kMovieFailedStatus.
static void MovieFinish(SimcamRecording *recording, CMTime endTime, void (^done)(int)) {
    [recording.writer endSessionAtSourceTime:endTime];
    [recording.writer finishWritingWithCompletionHandler:^{
        LOG(@"movie finished status=%ld err=%@ url=%@", (long)recording.writer.status, recording.writer.error,
            recording.url.path);
        done(recording.writer.status == AVAssetWriterStatusCompleted ? 0 : kMovieFailedStatus);
    }];
}

// Stops the recording of sinkID (on the pump queue). A stopStatus other than 0 means an interruption stopped it: the
// file is finished normally but reported with that status (a device reports AVErrorSessionWasInterrupted with
// AVErrorRecordingSuccessfullyFinishedKey).
static OSStatus MovieStop(CMBaseObjectRef session, CFStringRef sinkID, int stopStatus) {
    SimcamSession *sess = SessLookup(session);
    SimcamRecording *recording;
    @synchronized (sess.recs) {
        recording = sess.recs[(__bridge NSString *)sinkID];
    }

    LOG(@"FileSinkStopRecording %@ rec=%@", sinkID, recording);
    if (!recording) {
        return 0;
    }
    CFRetain(session);
    dispatch_async(gSessionLayer.pumpQueue, ^{
        // One stop per recording: a latch, a movie-error event or an interruption can race the app's own stop, and a
        // second markAsFinished/finishWriting (and a second DidStopRecording) must not happen.
        BOOL already;
        @synchronized (recording) {
            already = recording.stopRequested;
            recording.stopRequested = YES;
            recording.stopping = YES;
        }
        if (already) {
            LOG(@"FileSinkStopRecording %@: already stopping", recording.sinkID);
            CFRelease(session);
            return;
        }
        void (^done)(int) = ^(int status) {
            CMTime duration = recording.started ? CMTimeSubtract(recording.lastPts, recording.startPts) : kCMTimeZero;
            // the writer's failure, else the stop's own status (an interruption's; 0 for the app's stop)
            int reportedStatus = status ? status : stopStatus;
            SessPost(session, @"DidStopRecording", @{
                @"SectionID": recording.sinkID, @"SettingsID": @(recording.settingsID),
                @"ErrorStatus": @(reportedStatus), @"RecordingSucceeded": @(status == 0),
                @"MovieDuration": (__bridge_transfer id)CMTimeCopyAsDictionary(duration, NULL)});
            TraceRecord(kTraceOpMovieStop, recording.settingsID, reportedStatus);
            @synchronized (sess.recs) {
                [sess.recs removeObjectForKey:recording.sinkID];
            }
            CFRelease(session);
        };
        if (!recording.started) {   // a latched or failed stop before the first frame keeps its status
            done(stopStatus ?: kMovieFailedStatus);
            return;
        }
        CMTime endTime = CMTimeAdd(recording.lastPts, CMTimeMake(1, kMovieEndFrameRate));
        [recording.adaptor.assetWriterInput markAsFinished];
        if (recording.audioInput) {   // let the microphone's in-flight buffers reach the movie's end
            dispatch_time_t tailEnd = dispatch_time(DISPATCH_TIME_NOW, kMovieAudioTailMs * NSEC_PER_MSEC);
            dispatch_after(tailEnd, gSessionLayer.pumpQueue, ^{
                recording.audioDone = YES;
                [recording.audioInput markAsFinished];
                LOG(@"movie audio: %d buffers, first at %+.1f ms", recording.audioBufferCount,
                    recording.audioBufferCount
                        ? CMTimeGetSeconds(CMTimeSubtract(recording.audioFirstPts, recording.startPts)) * 1000
                        : 0);
                MovieFinish(recording, endTime, done);
            });
            return;
        }
        MovieFinish(recording, endTime, done);
    });

    return 0;
}

// Slot kSessSlotFileSinkPauseRecording (not implemented)
static OSStatus sess_FilePause(CMBaseObjectRef session, CFStringRef sinkID) {
    LOG(@"FileSinkPause %@ (not implemented)", sinkID);
    return 0;
}

// Slot kSessSlotFileSinkResumeRecording (not implemented)
static OSStatus sess_FileResume(CMBaseObjectRef session, CFStringRef sinkID) {
    LOG(@"FileSinkResume %@ (not implemented)", sinkID);
    return 0;
}
