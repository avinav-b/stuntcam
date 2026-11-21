// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): frame pump, movie file sinks.
// ---- pump ---------------------------------------------------------------------
static void PumpTick(SimcamSession *S) {
    uint64_t wcT0 = WcIsOn() ? WcNowNs() : 0;
    CVPixelBufferRef src = SceneGeneratedOn() ? SceneRenderSource(S) : SceneRenderFixture();
    if (gLatest) CVPixelBufferRelease(gLatest);
    gLatest = CVPixelBufferRetain(src);
    // webcam: the frame's capture time
    CMTime pts = WcIsOn() ? WcPts() : CMClockGetTime(CMClockGetHostTimeClock());
    for (SimcamSink *k in S.sinks) {
        if ([k.kind isEqualToString:@"vdo"] && k.queue) {
            CVPixelBufferRef pb = WcSinkOut(k, NO);   // webcam: straight from the ring frame
            if (!pb) pb = SinkConvert(k, src);
            CMSampleBufferRef sb = SinkSampleBuffer(pb, pts);
            LocalQueueMsg m = { kLocalQueueMsgSampleBuffer, sb, {0, 0} };
            OSStatus e = pFigLocalQueueEnqueue(k.queue, &m, NULL);
            if (k.frames++ < 2 || e)
                LOG(@"vdo %@ frame %d %dx%d '%.4s' -> %d", k.sinkID, k.frames, k.w, k.h,
                    (char *)&(OSType){CFSwapInt32HostToBig(k.fmt)}, (int)e);
            CFRelease(sb);
            CVPixelBufferRelease(pb);
        } else if ([k.kind isEqualToString:@"preview"]) {
            CVPixelBufferRef pb = WcSinkOut(k, YES);   // webcam: straight from the ring frame, mirroring included
            // mirrored preview (front camera), as on a device
            NSArray *note = !pb && OrientVirtualOn() ? PreviewNoteFor(k.sinkID) : nil;
            if (!pb) pb = SinkConvert(k, src);
            if ([note[1] boolValue]) {
                VTPixelRotationSessionRef flipSess = k.flipSess;
                int flipKind = k.flipKind;
                CVPixelBufferRef flipped = MirrorFlip(pb, [note[0] intValue], &flipSess, &flipKind);
                k.flipSess = flipSess;
                k.flipKind = flipKind;
                CVPixelBufferRelease(pb);
                pb = flipped;
            }
            PreviewPush(S.obj, k, pb);
            CVPixelBufferRelease(pb);
        } else if ([k.kind isEqualToString:@"metadata"] && k.queue && (gFrameNo % 5 == 0)) {
            if (SceneGeneratedOn()) BarcodeDetectScene(src);
            else BarcodeDetectFixture(src);
            CMSampleBufferRef sb = SinkSampleBuffer(src, pts);
            NSMutableArray *objs = [NSMutableArray array];
            for (NSDictionary *d in gBarcodeDicts) {
                NSMutableDictionary *m = [d mutableCopy];
                m[@"Timestamp"] = (__bridge_transfer id)CMTimeCopyAsDictionary(pts, NULL);
                [objs addObject:m];
            }
            CMSetAttachment(sb, CFSTR("BarcodeMetadataObjectsCount"), (__bridge CFTypeRef)@(objs.count),
                            kCMAttachmentMode_ShouldPropagate);
            CMSetAttachment(sb, CFSTR("BarcodeMetadataObjectsArray"), (__bridge CFTypeRef)objs,
                            kCMAttachmentMode_ShouldPropagate);
            LocalQueueMsg m = { kLocalQueueMsgSampleBuffer, sb, {0, 0} };
            OSStatus e = pFigLocalQueueEnqueue(k.queue, &m, NULL);
            if (k.frames++ < 2 || e)
                LOG(@"metadata %@ sbuf %lu codes -> %d", k.sinkID, (unsigned long)objs.count, (int)e);
            CFRelease(sb);
        }
    }
    NSArray *recs;
    @synchronized (S.recs) { recs = S.recs.allValues; }
    // per-recording lock (stop races the pump)
    for (SimcamRecording *r in recs) @synchronized (r) { MovieAppendVideo(r, src, pts); }
    CVPixelBufferRelease(src);
    if (wcT0) WcTickEnd(wcT0);
}

static void PumpReadActiveFormat(SimcamSession *S) {
    // the fig source configuration carries no format in the simulator ("/0x0, ?-?"); read the genuine AVCaptureDevice
    // behind the connection instead. sourceID is the string "<AVCaptureFigVideoDevice 0x...>": matched against the
    // discovered devices (no pointer deref).
    S.fmtW = kSrcW;
    S.fmtH = kSrcH;
    S.fps = 30;
    if (getenv(ENV_OFF_ACTIVE_FORMAT)) return;   // off switch: the format's size and rate are not read (1080p30 stays)
    for (id cc in SafeKVC(S.config, @"connectionConfigurations")) {
        if ([SafeKVC(cc, @"mediaType") unsignedIntValue] == 'soun') continue;
        id sid = SafeKVC(SafeKVC(cc, @"sourceConfiguration"), @"sourceID");
        AVCaptureDevice *device = [sid isKindOfClass:[AVCaptureDevice class]] ? sid : nil;
        if (!device && [sid isKindOfClass:[NSString class]]) {
            NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera,
                               AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInDualCamera,
                               AVCaptureDeviceTypeBuiltInDualWideCamera, AVCaptureDeviceTypeBuiltInTripleCamera,
                               AVCaptureDeviceTypeBuiltInTrueDepthCamera, AVCaptureDeviceTypeBuiltInLiDARDepthCamera];
            static NSArray *all;
            static dispatch_once_t o;
            dispatch_once(&o, ^{
                all = [AVCaptureDeviceDiscoverySession
                          discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo
                                                 position:AVCaptureDevicePositionUnspecified].devices;
            });
            for (AVCaptureDevice *d in all)
                if ([sid isEqualToString:[NSString stringWithFormat:@"<%@ %p>", NSStringFromClass([d class]), d]])
                    device = d;
            if (!device) LOG(@"active format: sourceID %@ matched no device", sid);
        }
        if (!device) continue;
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription);
        if (dims.width > 0) {
            S.fmtW = dims.width;
            S.fmtH = dims.height;
        }
        S.position = device.position;   // front/back decides the buffer content rotation
        CMTime minFrameDuration = device.activeVideoMinFrameDuration;
        if (CMTIME_IS_VALID(minFrameDuration) && minFrameDuration.value > 0)
            S.fps = MIN(60, round(1.0 / CMTimeGetSeconds(minFrameDuration)));
        LOG(@"active format: device=%@ %dx%d minFD=%.4f", device.localizedName, S.fmtW, S.fmtH,
            CMTimeGetSeconds(minFrameDuration));
        break;
    }
}
static void PumpWireSinks(SimcamSession *S) {
    for (SimcamSink *k in S.sinks) if (k.queue) CFRelease(k.queue);
    S.sinks = [NSMutableArray array];
    S.audioMovieSinks = [NSMutableSet set];
    S.wantsMic = NO;
    PumpReadActiveFormat(S);
    @try {
        for (id cc in SafeKVC(S.config, @"connectionConfigurations")) {
            id sink = SafeKVC(cc, @"sinkConfiguration");
            NSString *sinkID = SafeKVC(sink, @"sinkID");
            NSString *cls = NSStringFromClass([sink class]);
            DiagDumpProps(cc);
            DiagDumpProps(sink);
            SimcamSink *k = [SimcamSink new];
            k.sinkID = sinkID;
            k.w = S.fmtW;
            k.h = S.fmtH;
            k.fmt = kCVPixelFormatType_32BGRA;
            id ow = SafeKVC(cc, @"outputWidth"), oh = SafeKVC(cc, @"outputHeight"), of = SafeKVC(cc, @"outputFormat");
            if ([ow intValue] > 0 && [oh intValue] > 0) {
                k.w = [ow intValue];
                k.h = [oh intValue];
            }
            if ([of unsignedIntValue]) k.fmt = [of unsignedIntValue];
            // 17.5 carries the connection angle as `orientation` (enum), 26/27 as `rotationDegrees`;
            // SettingsRotationDeg reads both
            if ([cls containsString:@"VideoData"]) {
                k.kind = @"vdo";
                k.vdoRot = SettingsRotationDeg(cc, @"rotationDegrees", @"orientation", S.position);
                k.mirrored = [SafeKVC(cc, @"mirroringEnabled") boolValue];
            } else if ([cls containsString:@"Preview"]) {
                k.kind = @"preview";
                k.fmt = kCVPixelFormatType_32BGRA;
                k.w = kSrcW;
                k.h = kSrcH;
                k.rotation = [SafeKVC(cc, @"rotationDegrees") floatValue];
                // override: a fixed angle
                if (getenv(ENV_PREVIEW_ROTATION)) k.rotation = atof(getenv(ENV_PREVIEW_ROTATION));
            } else if ([cls containsString:@"Metadata"]) {
                k.kind = @"metadata";
            } else if ([cls containsString:@"Iris"] || [cls containsString:@"StillImage"]) {
                k.kind = @"photo";
                CMVideoDimensions maxDims = DimsFromValue(SafeKVC(sink, @"maxPhotoDimensions"));
                if (maxDims.width > 0) gIrisMaxDims = maxDims;
            } else if ([cls containsString:@"MovieFile"] || [cls containsString:@"FileSink"]) {
                k.kind = @"movie";
            } else {
                k.kind = @"other";
            }
            if ([NSStringFromClass([cc class]) containsString:@"Audio"] ||
                [SafeKVC(cc, @"mediaType") unsignedIntValue] == 'soun') {
                S.wantsMic = YES;
                if ([k.kind isEqualToString:@"movie"]) {
                    [S.audioMovieSinks addObject:sinkID];
                    LOG(@"audio connection -> movie sink %@", sinkID);
                    continue;
                }
                if ([cls containsString:@"AudioData"]) {
                    k.kind = @"ado";   // AVCaptureAudioDataOutput
                } else {
                    LOG(@"audio connection -> %@ sink ignored", cls);
                    continue;
                }
            }
            LOG(@"sink %@ kind=%@ id=%@ %dx%d fmt=%u rot=%.0f connRot=%@ conn=%@", cls, k.kind, sinkID, k.w, k.h,
                (unsigned)k.fmt, k.rotation, SafeKVC(cc, @"rotationDegrees"), NSStringFromClass([cc class]));
            if ([k.kind isEqualToString:@"vdo"] || [k.kind isEqualToString:@"metadata"] ||
                [k.kind isEqualToString:@"ado"]) {
                CFTypeRef q = NULL;
                OSStatus e = pFigLocalQueueCreate(NULL, 0, &q);
                if (!e && q) {
                    k.queue = q;
                    SessPost(S.obj, @"LocalQueueUpdated", @{@"SectionID": sinkID, @"LocalQueue": (__bridge id)q});
                }
            }
            [S.sinks addObject:k];
        }
    } @catch (NSException *ex) { LOG(@"EXC wiring sinks: %@", ex); }
}
static void PumpStart(SimcamSession *S) {
    double fps = S.fps > 0 ? S.fps : 30;
    if (S.timer && gPumpFps == fps) return;   // restart at the active format's frame rate
    if (S.timer) {
        dispatch_source_cancel(S.timer);
        S.timer = nil;
    }
    gPumpFps = fps;
    S.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gPumpQ);
    // first tick 10 ms after start: 100/30/10/0 ms measured on 26.3 + 27.0, 32 launches, 0 enqueue errors or dropped
    // frames at any value; 10 ms gives first frames ~100 ms sooner and 0 adds nothing. SIMCAM_FIRST_TICK_MS overrides.
    static long firstMs = -1;
    if (firstMs < 0) {
        const char *f = getenv(ENV_FIRST_TICK_MS);
        firstMs = f ? MAX(0, atol(f)) : 10;
    }
    dispatch_source_set_timer(S.timer, dispatch_time(DISPATCH_TIME_NOW, firstMs * NSEC_PER_MSEC),
                              (uint64_t)(NSEC_PER_SEC / fps), 0);
    __weak SimcamSession *weakSess = S;
    dispatch_source_set_event_handler(S.timer, ^{
        SimcamSession *sess = weakSess;
        if (sess && sess.running) {
            @autoreleasepool { PumpTick(sess); }
        }
    });
    dispatch_resume(S.timer);
}

static OSStatus sess_SetConfiguration(CMBaseObjectRef session, id config) {
    int op = SCOpBegin("session", 2.0);
    SimcamSession *sess = SessLookup(session);
    long long cid = [SafeKVC(config, @"configurationID") longLongValue];
    LOG(@"session %p SetConfiguration id=%lld\n%@", session, cid, DescribeShort(config, 4000));
    dispatch_sync(gPumpQ, ^{
        sess.config = config;
        SessPost(session, @"ConfigurationCommitted", @{@"ConfigurationID": @(cid)});
        if (sess.running) {
            PumpWireSinks(sess);
            MicUpdate(sess);
            PumpStart(sess);
            SessPost(session, @"ConfigurationDidBecomeLive", @{@"ConfigurationID": @(cid)});
        }
    });
    SCOpEnd(op);
    return 0;
}
static OSStatus sess_StartRunning(CMBaseObjectRef session) {
    // A stop of this session still waiting for photo callbacks (sess_StopRunning) is reported
    // first, so AVF never gets this DidStartRunning before that DidStopRunning
    if (StopWaitFlush(session))
        LOG(@"session %p StartRunning: a deferred stop was outstanding; reported it first", session);
    int op = SCOpBegin("session", 2.0);
    SimcamSession *sess = SessLookup(session);
    long long cid = [SafeKVC(sess.config, @"configurationID") longLongValue];
    LOG(@"session %p StartRunning (cid=%lld)", session, cid);
    dispatch_sync(gPumpQ, ^{
        // a start during an interruption ends it here: this DidStartRunning is the one AVF waits for, and a second one
        // from InterruptEnd would restart a session AVF thinks is running (then it forwards none of its stops).
        // SIMCAM_OFF_INTERRUPTION_RESET keeps the old behaviour (ledger L52).
        if (sess.interrupted && !getenv(ENV_OFF_INTERRUPTION_RESET)) {
            LOG(@"session %p started while interrupted: interruption over", session);
            sess.interrupted = NO;
        }
        sess.running = YES;
        SessPost(session, @"ConfigurationDidBecomeLive", @{@"ConfigurationID": @(cid)});
        SessPost(session, @"DidStartRunning", @{@"ConfigurationID": @(cid)});
        PumpWireSinks(sess);
        MicUpdate(sess);
        PumpStart(sess);
    });
    SCOpEnd(op);
    TRRecord(TR_SESSION_START, -1, cid);
    // design §5.4: at most one runtime error per 5 s, so an app that restarts on runtime errors can't spin
    // (process-wide, and SessInjectRuntimeError stops every running session: under a session latch none may run anyway)
    if (SCLatched("session")) {
        LOG(@"session latched: stopping it (runtime error %d unless one was posted in the last 5 s)", SC_LATCH_STATUS);
        SessInjectRuntimeError(SC_LATCH_AUTO, NO);
    }
    return 0;
}
// The pump-queue half of a stop: the session stops running (mic, pump timer) and its previews report stopped.
static void StopSessionOnPump(CMBaseObjectRef session, SimcamSession *sess) {
    // a stop during an interruption ends it for this session: AVF posts InterruptionEnded on the DidStopRunning that
    // follows, and InterruptEnd must not restart a session the app stopped (otherwise AVF takes the unasked
    // DidStartRunning and never forwards the app's later start or stop, so the engine pumps until teardown; 6 of 6 runs
    // on 26.3 and 27.0). SIMCAM_OFF_INTERRUPTION_RESET keeps the old behaviour (ledger L52).
    if (sess.interrupted && !getenv(ENV_OFF_INTERRUPTION_RESET)) {
        LOG(@"session %p stopped while interrupted: not restarted when the interruption ends", session);
        sess.interrupted = NO;
    }
    sess.running = NO;
    MicUpdate(sess);
    if (sess.timer) {
        dispatch_source_cancel(sess.timer);
        sess.timer = nil;
    }
    for (SimcamSink *k in sess.sinks)
        if ([k.kind isEqualToString:@"preview"]) SessPost(session, @"DidStopPreviewing", @{@"SectionID": k.sinkID});
}
// Test fault SIMCAM_TEST_FLUSH_RACE (ledger L73): a start's flush lands between StopWaitBeginStop and the waiter's
// dispatch; it must return only after this stop's DidStopRunning is queued. Blocks the stopping thread 50 ms.
static void TestFaultFlushRace(CMBaseObjectRef session) {
    long posted0 = atomic_load(&gStopWaitStopsPosted);
    CFRetain(session);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL outstanding = StopWaitFlush(session);
        LOG(@"SIMCAM_TEST_FLUSH_RACE: flush in the window returned (outstanding=%d); this stop's DidStopRunning %s",
            outstanding,
            atomic_load(&gStopWaitStopsPosted) > posted0 ? "was queued first (in order)"
                                                         : "NOT queued yet (out of order)");
        dispatch_async(gPumpQ, ^{ CFRelease(session); });
    });
    usleep(50000);
}
// DidStopRunning once the photos `sids` (stop stopId) have had their didFinishCapture, or after kStopWaitBoundS, or
// when a start flushes the stop: waited for on the session's private stop queue (StopWaitQueue), not on the stopping
// thread.
static void ReportStopAfterPhotoCallbacks(CMBaseObjectRef session, long long stopId, NSArray<NSNumber *> *sids) {
    CFRetain(session);
    long lateMs = getenv(ENV_TEST_LATE_WAITER_MS) ? atol(getenv(ENV_TEST_LATE_WAITER_MS)) : 0;
    dispatch_async(StopWaitQueue(session), ^{
        // test fault SIMCAM_TEST_LATE_WAITER_MS (ledger L74): the waiter runs <ms> late, as if starved of CPU, past a
        // start's kStopWaitBoundS flush bound
        if (lateMs > 0) {
            usleep((useconds_t)lateMs * 1000);
            LOG(@"SIMCAM_TEST_LATE_WAITER_MS: session %p stop waiter ran %ld ms late", session, lateMs);
        }
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        BOOL flushed = NO;
        NSUInteger left = StopWaitForPhotos(stopId, sids, kStopWaitBoundS, &flushed);
        double ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e6;
        if (flushed)
            LOG(@"session %p StopRunning: flushed by a start after %.1f ms (%lu photo(s) still due)", session, ms,
                (unsigned long)left);
        else if (left)
            LOG(@"session %p StopRunning: %lu photo(s) without didFinishCapture after %.1f s; stop reported anyway",
                session, (unsigned long)left, kStopWaitBoundS);
        else
            LOG(@"session %p StopRunning: in-flight photo callbacks done after %.1f ms", session, ms);
        SessPost(session, @"DidStopRunning", nil);
        StopWaitEndStop(stopId);   // after the post: a start's flush waits for this
        dispatch_async(gPumpQ, ^{ CFRelease(session); });   // a last release finalizes, which syncs to the pump queue
    });
}
static OSStatus sess_StopRunning(CMBaseObjectRef session) {
    TRRecord(TR_SESSION_STOP, -1, 0);
    int op = SCOpBegin("session", 2.0);
    SimcamSession *sess = SessLookup(session);
    LOG(@"session %p StopRunning", session);
    dispatch_sync(gPumpQ, ^{ StopSessionOnPump(session, sess); });
    SCOpEnd(op);
    // photos requested before the stop complete before DidStopRunning. Outside the session op, so a slow encode isn't
    // counted as a session hang. No deadlock: photo blocks only post asynchronously.
    BOOL drain = !getenv(ENV_OFF_PHOTO_DRAIN);   // off-switch: negative control for the canary's photo-then-stop (L51)
    if (drain) dispatch_sync(PhotoQueue(), ^{});
    // ledger L66 (fig/session_state.inc): and after AVF has run their delegates' didFinishCapture. From a private
    // queue, so AVF's stopRunning thread waits on its own condition, holding none of its locks, while the delegate
    // queue catches up. Not on a photo delegate queue, main included (StopWaitOnDelegateQueue): the callbacks it would
    // wait for run there after it. SIMCAM_OFF_STOP_WAIT reports the stop at once (the old behaviour, ledger L66).
    if (drain && !getenv(ENV_OFF_STOP_WAIT)) {
        NSArray<NSNumber *> *sids = nil;
        long long stopId = 0;
        if (StopWaitOnDelegateQueue()) {
            if (StopWaitHasPending(session))
                LOG(@"session %p StopRunning on a photo delegate queue (%s): not waiting for its callbacks", session,
                    pthread_main_np() ? "main thread" : "marked queue");
        } else {
            sids = StopWaitBeginStop(session, &stopId);
        }
        if (sids && getenv(ENV_TEST_FLUSH_RACE)) TestFaultFlushRace(session);
        if (sids) {
            ReportStopAfterPhotoCallbacks(session, stopId, sids);
            return 0;
        }
    }
    SessPost(session, @"DidStopRunning", nil);
    return 0;
}

// slot 1: SetSectionProperty(session, sectionID, key, value)
static OSStatus sess_SetSectionProperty(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                        CFTypeRef value) {
    SimcamSession *sess = SessLookup(session);
    LOG(@"SetSectionProperty %@ %@ = %@", section, key, DescribeShort((__bridge id)value, 200));
    // a rotation change applies to every preview sink, whatever the section
    if ([(__bridge NSString *)key containsString:@"Rotation"])
        for (SimcamSink *k in sess.sinks)
            if ([k.kind isEqualToString:@"preview"]) k.rotation = [(__bridge id)value floatValue];
    return ((OSStatus (*)(CMBaseObjectRef, CFStringRef, CFStringRef, CFTypeRef))gOrigDerived[1])(session, section, key,
                                                                                                 value);
}
// slot 2: CopySectionProperty(session, sectionID, key, allocator, out)
static OSStatus sess_CopySectionProperty(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                         CFAllocatorRef a, CFTypeRef *out) {
    SimcamSession *sess = SessLookup(session);
    SimcamRecording *r;
    @synchronized (sess.recs) { r = sess.recs[(__bridge NSString *)section]; }
    NSString *name = (__bridge NSString *)key;
    if ([name hasSuffix:@"RecordedDuration"]) {
        CMTime d = r && r.started ? CMTimeSubtract(r.last, r.start) : kCMTimeZero;
        *out = CMTimeCopyAsDictionary(d, a);
        return 0;
    }
    if ([name hasSuffix:@"RecordedSize"]) {
        NSNumber *n = r ? [[NSFileManager defaultManager] attributesOfItemAtPath:r.url.path error:nil][NSFileSize] ?: @0
                        : @0;
        *out = CFBridgingRetain(@(n.longLongValue));
        return 0;
    }
    static NSMutableSet *seen;
    if (!seen) seen = [NSMutableSet set];
    if (![seen containsObject:name]) {
        [seen addObject:name];
        LOG(@"CopySectionProperty %@ %@ -> original", section, name);
    }
    return ((OSStatus (*)(CMBaseObjectRef, CFStringRef, CFStringRef, CFAllocatorRef, CFTypeRef *))gOrigDerived[2])(
        session, section, key, a, out);
}

// A CoreMedia failure status for a movie that couldn't be written; AVF reports it to the app as AVErrorUnknown
// (-11800).
enum { kMovieFailedStatus = -12780 };
// slot 6: FileSinkStartRecording(session, sinkID, settings)
static OSStatus MovieStop(CMBaseObjectRef session, CFStringRef sinkID, int stopStatus);   // below
static OSStatus sess_FileStart(CMBaseObjectRef session, CFStringRef sinkID, id settings) {
    SimcamSession *sess = SessLookup(session);
    LOG(@"FileSinkStartRecording %@ settings=%@", sinkID, DescribeShort(settings, 1500));
    DiagDumpProps(settings);
    SimcamRecording *r = [SimcamRecording new];
    r.sinkID = (__bridge NSString *)sinkID;
    r.settingsID = [SafeKVC(settings, @"settingsID") longLongValue];
    r.url = SafeKVC(settings, @"outputURL");
    NSString *fileType = SafeKVC(settings, @"outputFileType") ?: AVFileTypeQuickTimeMovie;
    // movie in the portrait-device orientation: landscape buffer + rotation transform (like a real device)
    float rot = SettingsRotationDeg(settings, @"videoRotationDegrees", @"videoOrientation", sess.position);
    r.rot = (int)rot;
    // mirrored only with the virtual orientation on
    r.mirror = OrientVirtualOn() && [SafeKVC(settings, @"videoMirrored") boolValue];
    r.w = sess.fmtW ?: kSrcW;
    r.h = sess.fmtH ?: kSrcH;
    if (r.w != kSrcW || r.h != kSrcH) {
        SimcamSink *conv = [SimcamSink new];
        conv.w = r.w;
        conv.h = r.h;
        conv.fmt = '420v';
        r.conv = conv;
    }
    NSDictionary *videoSettings = SafeKVC(settings, @"videoSettings");
    NSString *codec = videoSettings[AVVideoCodecKey];
    if (![codec isEqual:AVVideoCodecTypeHEVC] || getenv(ENV_OFF_ACTIVE_FORMAT)) codec = AVVideoCodecTypeH264;
    NSError *err = nil;
    [[NSFileManager defaultManager] removeItemAtURL:r.url error:nil];
    r.writer = [AVAssetWriter assetWriterWithURL:r.url fileType:fileType error:&err];
    NSDictionary *vs = @{AVVideoCodecKey: codec, AVVideoWidthKey: @(r.w), AVVideoHeightKey: @(r.h)};
    AVAssetWriterInput *in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:vs];
    in.expectsMediaDataInRealTime = YES;
    if (rot) in.transform = CGAffineTransformMakeRotation(rot * M_PI / 180.0);
    [r.writer addInput:in];
    r.adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
                                                                                 sourcePixelBufferAttributes:nil];
    if ([sess.audioMovieSinks containsObject:r.sinkID] && r.writer && !getenv(ENV_OFF_AUDIO)) {
        double rate = gMicASBD.mSampleRate > 0 ? gMicASBD.mSampleRate : 48000;
        // AVF's audioSettings, with rate/channels matched to our source (tone/mic) and no channel layout
        NSMutableDictionary *as = [SafeKVC(settings, @"audioSettings") mutableCopy];
        if (as) {
            as[AVSampleRateKey] = @(rate);
            as[AVNumberOfChannelsKey] = @1;
            [as removeObjectForKey:AVChannelLayoutKey];
        }
        NSDictionary *def = @{AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @(rate),
                              AVNumberOfChannelsKey: @1, AVEncoderBitRateKey: @(64000)};
        AVAssetWriterInput *ain = nil;
        for (NSDictionary *cand in as ? @[as, def] : @[def]) {
            @try {
                if ([r.writer canApplyOutputSettings:cand forMediaType:AVMediaTypeAudio]) {
                    ain = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:cand];
                    LOG(@"movie audio input settings %@%@", cand == def ? @"(default) " : @"(from AVF) ",
                        DescribeShort(cand, 300));
                    break;
                }
            }
            @catch (NSException *e) { LOG(@"movie audio settings rejected %@", e.reason); }
        }
        ain.expectsMediaDataInRealTime = YES;
        if (ain && [r.writer canAddInput:ain]) {
            [r.writer addInput:ain];
            r.audioIn = ain;
        } else {
            LOG(@"movie: no audio input (as=%@)", as);
        }
    }
    LOG(@"movie writer %dx%d %@ @%.0f fps audio=%@", r.w, r.h, codec, sess.fps, r.audioIn ? @"yes" : @"no");
    if (!r.writer) {
        LOG(@"movie writer failed %@", err);
        TRRecord(TR_MOVIE_STOP, r.settingsID, kMovieFailedStatus);
        SessPost(session, @"DidStopRecording",
                   @{@"SectionID": r.sinkID, @"SettingsID": @(r.settingsID), @"ErrorStatus": @(kMovieFailedStatus)});
        return 0;
    }
    @synchronized (sess.recs) { sess.recs[r.sinkID] = r; }
    SessPost(session, @"DidStartRecording",
               @{@"SectionID": r.sinkID, @"SettingsID": @(r.settingsID), @"MovieWidth": @(r.w),
                 @"MovieHeight": @(r.h)});
    TRRecord(TR_MOVIE_START, r.settingsID, 0);
    if (SCLatched("movie")) {
        LOG(@"movie latched: recording stops with %d", SC_LATCH_STATUS);
        MovieStop(session, sinkID, SC_LATCH_STATUS);
    }
    return 0;
}
// slot 7: FileSinkStopRecording(session, sinkID)
// test probe: SIMCAM_TEST_MOVIE_ERR=<status> makes every app-requested stop report that status (maps Fig statuses to
// AVF errors)
static OSStatus sess_FileStop(CMBaseObjectRef session, CFStringRef sinkID) {
    static int me;   // the test status (0: none)
    static dispatch_once_t o;
    dispatch_once(&o, ^{ me = getenv(ENV_TEST_MOVIE_ERR) ? atoi(getenv(ENV_TEST_MOVIE_ERR)) : 0; });
    return MovieStop(session, sinkID, me);
}
// The recording's file ends at endT and is finished; done gets 0 when the writer completed, else kMovieFailedStatus.
static void MovieFinish(SimcamRecording *r, CMTime endT, void (^done)(int)) {
    [r.writer endSessionAtSourceTime:endT];
    [r.writer finishWritingWithCompletionHandler:^{
        LOG(@"movie finished status=%ld err=%@ url=%@", (long)r.writer.status, r.writer.error, r.url.path);
        done(r.writer.status == AVAssetWriterStatusCompleted ? 0 : kMovieFailedStatus);
    }];
}
// stopStatus != 0 = stopped by an interruption: the file is finished normally but reported with that status (device:
// AVErrorSessionWasInterrupted + AVErrorRecordingSuccessfullyFinishedKey)
static OSStatus MovieStop(CMBaseObjectRef session, CFStringRef sinkID, int stopStatus) {
    SimcamSession *sess = SessLookup(session);
    SimcamRecording *r;
    @synchronized (sess.recs) { r = sess.recs[(__bridge NSString *)sinkID]; }
    LOG(@"FileSinkStopRecording %@ rec=%@", sinkID, r);
    if (!r) return 0;
    CFRetain(session);
    dispatch_async(gPumpQ, ^{
        // one stop per recording: a latch, a movie-error event or an interruption can race the app's own stop, and a
        // second markAsFinished/finishWriting (and a second DidStopRecording) must not happen
        BOOL already;
        @synchronized (r) {
            already = r.stopRequested;
            r.stopRequested = YES;
            r.stopping = YES;
        }
        if (already) {
            LOG(@"FileSinkStopRecording %@: already stopping", r.sinkID);
            CFRelease(session);
            return;
        }
        void (^done)(int) = ^(int status) {
            CMTime d = r.started ? CMTimeSubtract(r.last, r.start) : kCMTimeZero;
            SessPost(session, @"DidStopRecording", @{
                @"SectionID": r.sinkID, @"SettingsID": @(r.settingsID),
                @"ErrorStatus": @(status ? status : stopStatus), @"RecordingSucceeded": @(status == 0),
                @"MovieDuration": (__bridge_transfer id)CMTimeCopyAsDictionary(d, NULL)});
            TRRecord(TR_MOVIE_STOP, r.settingsID, status ? status : stopStatus);
            @synchronized (sess.recs) { [sess.recs removeObjectForKey:r.sinkID]; }
            CFRelease(session);
        };
        if (!r.started) {   // a latched or failed stop before the first frame keeps its status
            done(stopStatus ?: kMovieFailedStatus);
            return;
        }
        CMTime endT = CMTimeAdd(r.last, CMTimeMake(1, 30));
        [r.adaptor.assetWriterInput markAsFinished];
        if (r.audioIn) {   // let the mic's in-flight buffers (~20 ms buffer + ~20 ms lag) reach the movie end
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), gPumpQ, ^{
                r.audioDone = YES;
                [r.audioIn markAsFinished];
                LOG(@"movie audio: %d buffers, first at %+.1f ms", r.audioBufs,
                    r.audioBufs ? CMTimeGetSeconds(CMTimeSubtract(r.audioFirst, r.start)) * 1000 : 0);
                MovieFinish(r, endT, done);
            });
            return;
        }
        MovieFinish(r, endT, done);
    });
    return 0;
}
static OSStatus sess_FilePause(CMBaseObjectRef session, CFStringRef sinkID) {
    LOG(@"FileSinkPause %@ (not implemented)", sinkID);
    return 0;
}
static OSStatus sess_FileResume(CMBaseObjectRef session, CFStringRef sinkID) {
    LOG(@"FileSinkResume %@ (not implemented)", sinkID);
    return 0;
}
