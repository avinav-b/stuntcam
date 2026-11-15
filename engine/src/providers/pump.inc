// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): frame pump, movie file sinks.
// ---- pump ---------------------------------------------------------------------
static void M5Tick(M5Sess *S) {
    CVPixelBufferRef src = M12Scene2On() ? M12Render(S) : M5Render();
    if (gLatest) CVPixelBufferRelease(gLatest); gLatest = CVPixelBufferRetain(src);
    CMTime pts = CMClockGetTime(CMClockGetHostTimeClock());
    for (M5Sink *k in S.sinks) {
        if ([k.kind isEqualToString:@"vdo"] && k.queue) {
            CVPixelBufferRef pb = M5Convert(k, src);
            CMSampleBufferRef sb = M5SBuf(pb, pts);
            M2LQMsg m = { 3, sb, {0, 0} }; OSStatus e = pFigLocalQueueEnqueue(k.queue, &m, NULL);
            if (k.frames++ < 2 || e) LOG(@"vdo %@ frame %d %dx%d '%.4s' -> %d", k.sinkID, k.frames, k.w, k.h, (char *)&(OSType){CFSwapInt32HostToBig(k.fmt)}, (int)e);
            CFRelease(sb); CVPixelBufferRelease(pb);
        } else if ([k.kind isEqualToString:@"preview"]) {
            CVPixelBufferRef pb = M5Convert(k, src);
            NSArray *note = M12OrientOn() ? M12PreviewNote(k.sinkID) : nil;   // mirrored preview (front camera), as on a device
            if ([note[1] boolValue]) { VTPixelRotationSessionRef fs = k.flipSess; int fk = k.flipKind; CVPixelBufferRef f = M12Flip(pb, [note[0] intValue], &fs, &fk); k.flipSess = fs; k.flipKind = fk; CVPixelBufferRelease(pb); pb = f; }
            M5PreviewPush(S.obj, k, pb);
            CVPixelBufferRelease(pb);
        } else if ([k.kind isEqualToString:@"metadata"] && k.queue && (gFrameNo % 5 == 0)) {
            if (M12Scene2On()) M12DetectBarcodes(src); else M5DetectBarcodes(src);
            CMSampleBufferRef sb = M5SBuf(src, pts);
            NSMutableArray *objs = [NSMutableArray array];
            for (NSDictionary *d in gBarcodeDicts) { NSMutableDictionary *m = [d mutableCopy]; m[@"Timestamp"] = (__bridge_transfer id)CMTimeCopyAsDictionary(pts, NULL); [objs addObject:m]; }
            CMSetAttachment(sb, CFSTR("BarcodeMetadataObjectsCount"), (__bridge CFTypeRef)@(objs.count), kCMAttachmentMode_ShouldPropagate);
            CMSetAttachment(sb, CFSTR("BarcodeMetadataObjectsArray"), (__bridge CFTypeRef)objs, kCMAttachmentMode_ShouldPropagate);
            M2LQMsg m = { 3, sb, {0, 0} }; OSStatus e = pFigLocalQueueEnqueue(k.queue, &m, NULL);
            if (k.frames++ < 2 || e) LOG(@"metadata %@ sbuf %lu codes -> %d", k.sinkID, (unsigned long)objs.count, (int)e);
            CFRelease(sb);
        }
    }
    NSArray *recs; @synchronized (S.recs) { recs = S.recs.allValues; }
    for (M5Rec *r in recs) @synchronized (r) { M5RecAppend(r, src, pts); }   // per-recording lock (stop races the pump)
    CVPixelBufferRelease(src);
}

static void M5ReadActiveFormat(M5Sess *S) {
    // the fig source configuration carries no format in the simulator ("/0x0, ?-?"); read the genuine AVCaptureDevice behind the
    // connection instead. sourceID is the string "<AVCaptureFigVideoDevice 0x...>" — matched against discovered devices (no pointer deref).
    S.fmtW = kSrcW; S.fmtH = kSrcH; S.fps = 30;
    if (getenv("M8_NO_FORMATDIMS")) return;
    for (id cc in M5KVC(S.config, @"connectionConfigurations")) {
        if ([M5KVC(cc, @"mediaType") unsignedIntValue] == 'soun') continue;
        id sid = M5KVC(M5KVC(cc, @"sourceConfiguration"), @"sourceID");
        AVCaptureDevice *dd = [sid isKindOfClass:[AVCaptureDevice class]] ? sid : nil;
        if (!dd && [sid isKindOfClass:[NSString class]]) {
            NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera, AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInDualCamera, AVCaptureDeviceTypeBuiltInDualWideCamera, AVCaptureDeviceTypeBuiltInTripleCamera, AVCaptureDeviceTypeBuiltInTrueDepthCamera, AVCaptureDeviceTypeBuiltInLiDARDepthCamera];
            static NSArray *all; static dispatch_once_t o; dispatch_once(&o, ^{ all = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices; });
            for (AVCaptureDevice *d in all) if ([sid isEqualToString:[NSString stringWithFormat:@"<%@ %p>", NSStringFromClass([d class]), d]]) dd = d;
            if (!dd) LOG(@"m8 sourceID %@ matched no device", sid);
        }
        if (!dd) continue;
        CMVideoDimensions dm = CMVideoFormatDescriptionGetDimensions(dd.activeFormat.formatDescription);
        if (dm.width > 0) { S.fmtW = dm.width; S.fmtH = dm.height; }
        S.position = dd.position;   // front/back decides the buffer content rotation
        CMTime mfd = dd.activeVideoMinFrameDuration; if (CMTIME_IS_VALID(mfd) && mfd.value > 0) S.fps = MIN(60, round(1.0 / CMTimeGetSeconds(mfd)));
        LOG(@"m8 format read: device=%@ %dx%d minFD=%.4f", dd.localizedName, S.fmtW, S.fmtH, CMTimeGetSeconds(mfd));
        break;
    }
}
static void M5WireSinks(M5Sess *S) {
    for (M5Sink *k in S.sinks) if (k.queue) CFRelease(k.queue);
    S.sinks = [NSMutableArray array];
    S.audioMovieSinks = [NSMutableSet set]; S.wantsMic = NO;
    M5ReadActiveFormat(S);
    @try {
        for (id cc in M5KVC(S.config, @"connectionConfigurations")) {
            id sink = M5KVC(cc, @"sinkConfiguration");
            NSString *sinkID = M5KVC(sink, @"sinkID");
            NSString *cls = NSStringFromClass([sink class]);
            M5DumpProps(cc); M5DumpProps(sink);
            M5Sink *k = [M5Sink new]; k.sinkID = sinkID; k.w = S.fmtW; k.h = S.fmtH; k.fmt = kCVPixelFormatType_32BGRA;
            id ow = M5KVC(cc, @"outputWidth"), oh = M5KVC(cc, @"outputHeight"), of = M5KVC(cc, @"outputFormat");
            if ([ow intValue] > 0 && [oh intValue] > 0) { k.w = [ow intValue]; k.h = [oh intValue]; }
            if ([of unsignedIntValue]) k.fmt = [of unsignedIntValue];
            // 17.5 carries the connection angle as `orientation` (enum), 26/27 as `rotationDegrees`; M6Deg reads both
            if ([cls containsString:@"VideoData"]) { k.kind = @"vdo"; k.vdoRot = M6Deg(cc, @"rotationDegrees", @"orientation", S.position); k.mirrored = [M5KVC(cc, @"mirroringEnabled") boolValue]; }
            else if ([cls containsString:@"Preview"]) { k.kind = @"preview"; k.fmt = kCVPixelFormatType_32BGRA; k.w = kSrcW; k.h = kSrcH; k.rotation = [M5KVC(cc, @"rotationDegrees") floatValue]; if (getenv("M5_PREVIEW_ROT")) k.rotation = atof(getenv("M5_PREVIEW_ROT")); }
            else if ([cls containsString:@"Metadata"]) k.kind = @"metadata";
            else if ([cls containsString:@"Iris"] || [cls containsString:@"StillImage"]) { k.kind = @"photo"; CMVideoDimensions md = M8Dims(M5KVC(sink, @"maxPhotoDimensions")); if (md.width > 0) gIrisMaxDims = md; }
            else if ([cls containsString:@"MovieFile"] || [cls containsString:@"FileSink"]) k.kind = @"movie";
            else k.kind = @"other";
            if ([NSStringFromClass([cc class]) containsString:@"Audio"] || [M5KVC(cc, @"mediaType") unsignedIntValue] == 'soun') {
                S.wantsMic = YES;
                if ([k.kind isEqualToString:@"movie"]) { [S.audioMovieSinks addObject:sinkID]; LOG(@"audio connection -> movie sink %@", sinkID); continue; }
                if ([cls containsString:@"AudioData"]) k.kind = @"ado"; // AVCaptureAudioDataOutput
                else { LOG(@"audio connection -> %@ sink ignored", cls); continue; }
            }
            LOG(@"sink %@ kind=%@ id=%@ %dx%d fmt=%u rot=%.0f connRot=%@ conn=%@", cls, k.kind, sinkID, k.w, k.h, (unsigned)k.fmt, k.rotation, M5KVC(cc, @"rotationDegrees"), NSStringFromClass([cc class]));
            if ([k.kind isEqualToString:@"vdo"] || [k.kind isEqualToString:@"metadata"] || [k.kind isEqualToString:@"ado"]) {
                CFTypeRef q = NULL; OSStatus e = pFigLocalQueueCreate(NULL, 0, &q);
                if (!e && q) { k.queue = q; M5SessPost(S.obj, @"LocalQueueUpdated", @{@"SectionID": sinkID, @"LocalQueue": (__bridge id)q}); }
            }
            [S.sinks addObject:k];
        }
    } @catch (NSException *ex) { LOG(@"EXC wiring sinks: %@", ex); }
}
static void M5StartPump(M5Sess *S) {
    double fps = S.fps > 0 ? S.fps : 30;
    if (S.timer && gPumpFps == fps) return;   // restart at the active format's frame rate
    if (S.timer) { dispatch_source_cancel(S.timer); S.timer = nil; }
    gPumpFps = fps;
    S.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gPumpQ);
    // first tick 10 ms after start: 100/30/10/0 ms measured on 26.3 + 27.0, 32 launches, 0 enqueue
    // errors or dropped frames at any value; 10 ms gives first frames ~100 ms sooner and 0 adds nothing. SIMCAM_FIRST_TICK_MS overrides.
    static long firstMs = -1; if (firstMs < 0) { const char *f = getenv("SIMCAM_FIRST_TICK_MS"); firstMs = f ? MAX(0, atol(f)) : 10; }
    dispatch_source_set_timer(S.timer, dispatch_time(DISPATCH_TIME_NOW, firstMs * NSEC_PER_MSEC), (uint64_t)(NSEC_PER_SEC / fps), 0);
    __weak M5Sess *w = S;
    dispatch_source_set_event_handler(S.timer, ^{ M5Sess *s = w; if (s && s.running) { @autoreleasepool { M5Tick(s); } } });
    dispatch_resume(S.timer);
}

static OSStatus sess_SetConfiguration(CMBaseObjectRef s, id config) {
    int op = SCOpBegin("session", 2.0);
    M5Sess *S = M5S(s);
    long long cid = [M5KVC(config, @"configurationID") longLongValue];
    LOG(@"session %p SetConfiguration id=%lld\n%@", s, cid, M5Desc(config, 4000));
    dispatch_sync(gPumpQ, ^{
        S.config = config;
        M5SessPost(s, @"ConfigurationCommitted", @{@"ConfigurationID": @(cid)});
        if (S.running) { M5WireSinks(S); M5UpdateMic(S); M5StartPump(S); M5SessPost(s, @"ConfigurationDidBecomeLive", @{@"ConfigurationID": @(cid)}); }
    });
    SCOpEnd(op);
    return 0;
}
static OSStatus sess_StartRunning(CMBaseObjectRef s) {
    // A stop of this session still waiting for photo callbacks (sess_StopRunning) is reported
    // first, so AVF never gets this DidStartRunning before that DidStopRunning
    if (M40FlushStop(s)) LOG(@"session %p StartRunning: a deferred stop was outstanding; reported it first", s);
    int op = SCOpBegin("session", 2.0);
    M5Sess *S = M5S(s);
    long long cid = [M5KVC(S.config, @"configurationID") longLongValue];
    LOG(@"session %p StartRunning (cid=%lld)", s, cid);
    dispatch_sync(gPumpQ, ^{
        // a start during an interruption ends it here: this DidStartRunning is the one AVF waits for, and a second one from
        // M12EndInterrupt would restart a session AVF thinks is running (then it forwards none of its stops)
        if (S.interrupted && !getenv("M14_OLD_ENDINT")) { LOG(@"session %p started while interrupted: interruption over", s); S.interrupted = NO; }
        S.running = YES;
        M5SessPost(s, @"ConfigurationDidBecomeLive", @{@"ConfigurationID": @(cid)});
        M5SessPost(s, @"DidStartRunning", @{@"ConfigurationID": @(cid)});
        M5WireSinks(S);
        M5UpdateMic(S);
        M5StartPump(S);
    });
    SCOpEnd(op);
    TRRecord(TR_SESSION_START, -1, cid);
    if (SCLatched("session")) {   // design §5.4: at most one runtime error per 5 s, so an app that restarts on runtime errors can't spin
        // (process-wide, and M12RuntimeError stops every running session: under a session latch none may run anyway)
        LOG(@"session latched: stopping it (runtime error %d unless one was posted in the last 5 s)", SC_LATCH_STATUS);
        M12RuntimeError(SC_LATCH_AUTO, NO);
    }
    return 0;
}
static OSStatus sess_StopRunning(CMBaseObjectRef s) {
    TRRecord(TR_SESSION_STOP, -1, 0);
    int op = SCOpBegin("session", 2.0);
    M5Sess *S = M5S(s);
    LOG(@"session %p StopRunning", s);
    dispatch_sync(gPumpQ, ^{
        // a stop during an interruption ends it for this session: AVF posts InterruptionEnded on the DidStopRunning below, and
        // M12EndInterrupt must not restart a session the app stopped (otherwise AVF takes the unasked DidStartRunning and never
        // forwards the app's later start or stop, so the engine pumps until teardown; 6 of 6 runs on 26.3 and 27.0)
        if (S.interrupted && !getenv("M14_OLD_ENDINT")) { LOG(@"session %p stopped while interrupted: not restarted when the interruption ends", s); S.interrupted = NO; }
        S.running = NO;
        M5UpdateMic(S);
        if (S.timer) { dispatch_source_cancel(S.timer); S.timer = nil; }
        for (M5Sink *k in S.sinks) if ([k.kind isEqualToString:@"preview"]) M5SessPost(s, @"DidStopPreviewing", @{@"SectionID": k.sinkID});
    });
    SCOpEnd(op);
    // photos requested before the stop complete before DidStopRunning, as when encoding ran on the pump queue. Outside the
    // session op, so a slow encode isn't counted as a session hang. No deadlock: photo blocks only post asynchronously.
    BOOL drain = !getenv("M13_NO_PHOTODRAIN");   // off-switch: negative control for the canary's photo-then-stop (L51)
    if (drain) dispatch_sync(M5PhotoQ(), ^{});
    // ledger L66 (fig/session_state.inc): and after AVF has run their delegates' didFinishCapture. From a private queue, so
    // AVF's stopRunning thread waits on its own condition, holding none of its locks, while the delegate queue catches up.
    // Not on a photo delegate queue, main included (M40OnDelegateQueue): the callbacks it would wait for run there after it.
    if (drain && !getenv("M40_OLD_STOP")) {
        NSArray<NSNumber *> *sids = nil; long long stopId = 0;
        if (M40OnDelegateQueue()) { if (M40HasPending(s)) LOG(@"session %p StopRunning on a photo delegate queue (%s): not waiting for its callbacks", s, pthread_main_np() ? "main thread" : "marked queue"); }
        else sids = M40BeginStop(s, &stopId);
        if (sids && getenv("M54_FLUSH_RACE")) {
            // test fault: a start's flush lands between M40BeginStop and the waiter's dispatch; it must return only
            // after this stop's DidStopRunning is queued
            long posted0 = atomic_load(&gM40StopsPosted);
            CFRetain(s);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                BOOL f = M40FlushStop(s);
                LOG(@"M54_FLUSH_RACE: flush in the window returned (outstanding=%d); this stop's DidStopRunning %s", f,
                    atomic_load(&gM40StopsPosted) > posted0 ? "was queued first (in order)" : "NOT queued yet (out of order)");
                dispatch_async(gPumpQ, ^{ CFRelease(s); });
            });
            usleep(50000);
        }
        if (sids) {
            CFRetain(s);
            long lateMs = getenv("M59_LATE_WAITER") ? atol(getenv("M59_LATE_WAITER")) : 0;
            dispatch_async(M40StopQ(s), ^{
                // test fault: the waiter runs <ms> late, as if starved of CPU, past a start's kM40WaitS flush bound
                if (lateMs > 0) { usleep((useconds_t)lateMs * 1000); LOG(@"M59_LATE_WAITER: session %p stop waiter ran %ld ms late", s, lateMs); }
                uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
                BOOL flushed = NO;
                NSUInteger left = M40WaitPhotoCallbacks(stopId, sids, kM40WaitS, &flushed);
                double ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e6;
                if (flushed) LOG(@"session %p StopRunning: flushed by a start after %.1f ms (%lu photo(s) still due)", s, ms, (unsigned long)left);
                else if (left) LOG(@"session %p StopRunning: %lu photo(s) without didFinishCapture after %.1f s; stop reported anyway", s, (unsigned long)left, kM40WaitS);
                else LOG(@"session %p StopRunning: in-flight photo callbacks done after %.1f ms", s, ms);
                M5SessPost(s, @"DidStopRunning", nil);
                M40EndStop(stopId);   // after the post: a start's flush waits for this
                dispatch_async(gPumpQ, ^{ CFRelease(s); });   // a last release finalizes, which syncs to the pump queue
            });
            return 0;
        }
    }
    M5SessPost(s, @"DidStopRunning", nil);
    return 0;
}

// slot 1: SetSectionProperty(session, sectionID, key, value)
static OSStatus sess_SetSectionProperty(CMBaseObjectRef s, CFStringRef section, CFStringRef key, CFTypeRef value) {
    M5Sess *S = M5S(s);
    LOG(@"SetSectionProperty %@ %@ = %@", section, key, M5Desc((__bridge id)value, 200));
    if ([(__bridge NSString *)key containsString:@"Rotation"]) for (M5Sink *k in S.sinks) if ([k.sinkID isEqual:(__bridge NSString *)section] || [k.kind isEqualToString:@"preview"]) { if ([k.kind isEqualToString:@"preview"]) k.rotation = [(__bridge id)value floatValue]; }
    return ((OSStatus (*)(CMBaseObjectRef, CFStringRef, CFStringRef, CFTypeRef))gOrigDerived[1])(s, section, key, value);
}
// slot 2: CopySectionProperty(session, sectionID, key, allocator, out)
static OSStatus sess_CopySectionProperty(CMBaseObjectRef s, CFStringRef section, CFStringRef key, CFAllocatorRef a, CFTypeRef *out) {
    M5Sess *S = M5S(s);
    M5Rec *r; @synchronized (S.recs) { r = S.recs[(__bridge NSString *)section]; }
    NSString *k = (__bridge NSString *)key;
    if ([k hasSuffix:@"RecordedDuration"]) { CMTime d = r && r.started ? CMTimeSubtract(r.last, r.start) : kCMTimeZero; *out = CMTimeCopyAsDictionary(d, a); return 0; }
    if ([k hasSuffix:@"RecordedSize"]) { NSNumber *n = r ? [[NSFileManager defaultManager] attributesOfItemAtPath:r.url.path error:nil][NSFileSize] ?: @0 : @0; *out = CFBridgingRetain(@(n.longLongValue)); return 0; }
    static NSMutableSet *seen; if (!seen) seen = [NSMutableSet set];
    if (![seen containsObject:k]) { [seen addObject:k]; LOG(@"CopySectionProperty %@ %@ -> original", section, k); }
    return ((OSStatus (*)(CMBaseObjectRef, CFStringRef, CFStringRef, CFAllocatorRef, CFTypeRef *))gOrigDerived[2])(s, section, key, a, out);
}

// slot 6: FileSinkStartRecording(session, sinkID, settings)
static OSStatus M12StopRec(CMBaseObjectRef s, CFStringRef sinkID, int stopStatus);   // below
static OSStatus sess_FileStart(CMBaseObjectRef s, CFStringRef sinkID, id settings) {
    M5Sess *S = M5S(s);
    LOG(@"FileSinkStartRecording %@ settings=%@", sinkID, M5Desc(settings, 1500));
    M5DumpProps(settings);
    M5Rec *r = [M5Rec new]; r.sinkID = (__bridge NSString *)sinkID; r.settingsID = [M5KVC(settings, @"settingsID") longLongValue];
    r.url = M5KVC(settings, @"outputURL");
    NSString *ft = M5KVC(settings, @"outputFileType") ?: AVFileTypeQuickTimeMovie;
    // movie in the portrait-device orientation: landscape buffer + rotation transform (like a real device)
    float rot = M6Deg(settings, @"videoRotationDegrees", @"videoOrientation", S.position);
    r.rot = (int)rot; r.mirror = M12OrientOn() && [M5KVC(settings, @"videoMirrored") boolValue];   // mirrored only with the virtual orientation on
    r.w = S.fmtW ?: kSrcW; r.h = S.fmtH ?: kSrcH;
    if (r.w != kSrcW || r.h != kSrcH) { M5Sink *cv = [M5Sink new]; cv.w = r.w; cv.h = r.h; cv.fmt = '420v'; r.conv = cv; }
    NSDictionary *vset = M5KVC(settings, @"videoSettings"); NSString *codec = vset[AVVideoCodecKey];
    if (![codec isEqual:AVVideoCodecTypeHEVC] || getenv("M8_NO_FORMATDIMS")) codec = AVVideoCodecTypeH264;
    NSError *err = nil;
    [[NSFileManager defaultManager] removeItemAtURL:r.url error:nil];
    r.writer = [AVAssetWriter assetWriterWithURL:r.url fileType:ft error:&err];
    NSDictionary *vs = @{AVVideoCodecKey: codec, AVVideoWidthKey: @(r.w), AVVideoHeightKey: @(r.h)};
    AVAssetWriterInput *in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:vs];
    in.expectsMediaDataInRealTime = YES;
    if (rot) in.transform = CGAffineTransformMakeRotation(rot * M_PI / 180.0);
    [r.writer addInput:in];
    r.adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in sourcePixelBufferAttributes:nil];
    if ([S.audioMovieSinks containsObject:r.sinkID] && r.writer && !getenv("M6_NO_MIC")) {
        double rate = gMicASBD.mSampleRate > 0 ? gMicASBD.mSampleRate : 48000;
        // AVF's audioSettings, with rate/channels matched to our source (tone/mic) and no channel layout
        NSMutableDictionary *as = [M5KVC(settings, @"audioSettings") mutableCopy];
        if (as) { as[AVSampleRateKey] = @(rate); as[AVNumberOfChannelsKey] = @1; [as removeObjectForKey:AVChannelLayoutKey]; }
        NSDictionary *def = @{AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @(rate), AVNumberOfChannelsKey: @1, AVEncoderBitRateKey: @(64000)};
        AVAssetWriterInput *ain = nil;
        for (NSDictionary *cand in as ? @[as, def] : @[def]) {
            @try { if ([r.writer canApplyOutputSettings:cand forMediaType:AVMediaTypeAudio]) { ain = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:cand]; LOG(@"movie audio input settings %@%@", cand == def ? @"(default) " : @"(from AVF) ", M5Desc(cand, 300)); break; } }
            @catch (NSException *e) { LOG(@"movie audio settings rejected %@", e.reason); }
        }
        ain.expectsMediaDataInRealTime = YES;
        if (ain && [r.writer canAddInput:ain]) { [r.writer addInput:ain]; r.audioIn = ain; } else LOG(@"movie: no audio input (as=%@)", as);
    }
    LOG(@"movie writer %dx%d %@ @%.0f fps audio=%@", r.w, r.h, codec, S.fps, r.audioIn ? @"yes" : @"no");
    if (!r.writer) { LOG(@"movie writer failed %@", err); TRRecord(TR_MOVIE_STOP, r.settingsID, -12780); M5SessPost(s, @"DidStopRecording", @{@"SectionID": r.sinkID, @"SettingsID": @(r.settingsID), @"ErrorStatus": @(-12780)}); return 0; }
    @synchronized (S.recs) { S.recs[r.sinkID] = r; }
    M5SessPost(s, @"DidStartRecording", @{@"SectionID": r.sinkID, @"SettingsID": @(r.settingsID), @"MovieWidth": @(r.w), @"MovieHeight": @(r.h)});
    TRRecord(TR_MOVIE_START, r.settingsID, 0);
    if (SCLatched("movie")) { LOG(@"movie latched: recording stops with %d", SC_LATCH_STATUS); M12StopRec(s, sinkID, SC_LATCH_STATUS); }
    return 0;
}
// slot 7: FileSinkStopRecording(session, sinkID)
static OSStatus M12StopRec(CMBaseObjectRef s, CFStringRef sinkID, int stopStatus);
// test probe: SIMCAM_TEST_MOVIE_ERR=<status> makes every app-requested stop report that status (maps Fig statuses to AVF errors)
static OSStatus sess_FileStop(CMBaseObjectRef s, CFStringRef sinkID) { static int me; static dispatch_once_t o; dispatch_once(&o, ^{ me = getenv("SIMCAM_TEST_MOVIE_ERR") ? atoi(getenv("SIMCAM_TEST_MOVIE_ERR")) : 0; }); return M12StopRec(s, sinkID, me); }
// stopStatus != 0 = stopped by an interruption: the file is finished normally but reported with that status (device: AVErrorSessionWasInterrupted + AVErrorRecordingSuccessfullyFinishedKey)
static OSStatus M12StopRec(CMBaseObjectRef s, CFStringRef sinkID, int stopStatus) {
    M5Sess *S = M5S(s);
    M5Rec *r; @synchronized (S.recs) { r = S.recs[(__bridge NSString *)sinkID]; }
    LOG(@"FileSinkStopRecording %@ rec=%@", sinkID, r);
    if (!r) return 0;
    CFRetain(s);
    dispatch_async(gPumpQ, ^{
        // one stop per recording: a latch, a movie-error event or an interruption can race the app's own stop, and a second
        // markAsFinished/finishWriting (and a second DidStopRecording) must not happen
        BOOL already; @synchronized (r) { already = r.stopRequested; r.stopRequested = YES; r.stopping = YES; }
        if (already) { LOG(@"FileSinkStopRecording %@: already stopping", r.sinkID); CFRelease(s); return; }
        void (^done)(int) = ^(int status) {
            CMTime d = r.started ? CMTimeSubtract(r.last, r.start) : kCMTimeZero;
            M5SessPost(s, @"DidStopRecording", @{@"SectionID": r.sinkID, @"SettingsID": @(r.settingsID), @"ErrorStatus": @(status ? status : stopStatus), @"RecordingSucceeded": @(status == 0), @"MovieDuration": (__bridge_transfer id)CMTimeCopyAsDictionary(d, NULL)});
            TRRecord(TR_MOVIE_STOP, r.settingsID, status ? status : stopStatus);
            @synchronized (S.recs) { [S.recs removeObjectForKey:r.sinkID]; }
            CFRelease(s);
        };
        if (!r.started) { done(stopStatus ?: -12780); return; }   // a latched or failed stop before the first frame keeps its status
        CMTime endT = CMTimeAdd(r.last, CMTimeMake(1, 30));
        [r.adaptor.assetWriterInput markAsFinished];
        if (r.audioIn) {   // let the mic's in-flight buffers (~20 ms buffer + ~20 ms lag) reach the movie end
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), gPumpQ, ^{ r.audioDone = YES; [r.audioIn markAsFinished];
                LOG(@"movie audio: %d buffers, first at %+.1f ms", r.audioBufs, r.audioBufs ? CMTimeGetSeconds(CMTimeSubtract(r.audioFirst, r.start)) * 1000 : 0);
                [r.writer endSessionAtSourceTime:endT];
                [r.writer finishWritingWithCompletionHandler:^{ LOG(@"movie finished status=%ld err=%@ url=%@", (long)r.writer.status, r.writer.error, r.url.path); done(r.writer.status == AVAssetWriterStatusCompleted ? 0 : -12780); }]; });
            return;
        }
        [r.writer endSessionAtSourceTime:endT];
        [r.writer finishWritingWithCompletionHandler:^{ LOG(@"movie finished status=%ld err=%@ url=%@", (long)r.writer.status, r.writer.error, r.url.path); done(r.writer.status == AVAssetWriterStatusCompleted ? 0 : -12780); }];
    });
    return 0;
}
static OSStatus sess_FilePause(CMBaseObjectRef s, CFStringRef sinkID) { LOG(@"FileSinkPause %@ (not implemented)", sinkID); return 0; }
static OSStatus sess_FileResume(CMBaseObjectRef s, CFStringRef sinkID) { LOG(@"FileSinkResume %@ (not implemented)", sinkID); return 0; }

