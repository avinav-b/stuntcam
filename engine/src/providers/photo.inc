// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): photo encode and still-image slots.
// ---- photo ----------------------------------------------------------------------
// Ledger L72: the EXIF/TIFF capture times are `when`, the wall clock at the frame grab (the instant
// the photo's timestamp is, L68), with milliseconds (SubsecTime*) and the UTC offset (OffsetTime*), as a device writes
// them; the encode's time would be 100-480 ms later. SIMCAM_OFF_GRAB_EXIF_TIME=1 stamps them at the encode (with the
// subseconds).
static NSData *PhotoEncode(CVPixelBufferRef src, int w, int h, NSString *uti, int exifOrientation,
                             NSDictionary **metaOut, NSDate *when) {
    CIImage *ci = [CIImage imageWithCVPixelBuffer:src];
    // the source is the 1920x1080 canvas, or the webcam canvas at the ring's size
    if (w > 0 && h > 0)
        ci = [ci imageByApplyingTransform:CGAffineTransformMakeScale((double)w / CVPixelBufferGetWidth(src),
                                                                     (double)h / CVPixelBufferGetHeight(src))];
    static CIContext *ctx;
    if (!ctx) ctx = [CIContext contextWithOptions:nil];
    CGImageRef cgImage = [ctx createCGImage:ci fromRect:ci.extent];
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data,
                                                                 (__bridge CFStringRef)uti, 1, NULL);
    if (!dst && ![uti isEqualToString:@"public.jpeg"]) {
        LOG(@"photo: no encoder for %@, using JPEG", uti);
        dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.jpeg"), 1, NULL);
    }
    if (!when || getenv(ENV_OFF_GRAB_EXIF_TIME)) when = [NSDate date];
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.dateFormat = @"yyyy:MM:dd HH:mm:ss";
    NSString *now = [formatter stringFromDate:when];
    formatter.dateFormat = @"SSS";
    NSString *sub = [formatter stringFromDate:when];
    formatter.dateFormat = @"xxx";
    NSString *ofs = [formatter stringFromDate:when];
    NSDictionary *exif = @{
        (id)kCGImagePropertyExifDateTimeOriginal: now, (id)kCGImagePropertyExifDateTimeDigitized: now,
        (id)kCGImagePropertyExifSubsecTimeOriginal: sub, (id)kCGImagePropertyExifSubsecTimeDigitized: sub,
        (id)kCGImagePropertyExifOffsetTimeOriginal: ofs, (id)kCGImagePropertyExifOffsetTimeDigitized: ofs,
        (id)kCGImagePropertyExifSubsecTime: sub, (id)kCGImagePropertyExifOffsetTime: ofs,
        (id)kCGImagePropertyExifPixelXDimension: @(CGImageGetWidth(cgImage)),
        (id)kCGImagePropertyExifPixelYDimension: @(CGImageGetHeight(cgImage)),
        (id)kCGImagePropertyExifLensMake: @"Apple", (id)kCGImagePropertyExifLensModel: @"simcam virtual back camera",
        (id)kCGImagePropertyExifFocalLength: @6.765, (id)kCGImagePropertyExifFNumber: @1.78,
        (id)kCGImagePropertyExifExposureTime: @(1.0 / 60), (id)kCGImagePropertyExifISOSpeedRatings: @[@100]};
    NSDictionary *meta = @{
        (id)kCGImagePropertyOrientation: @(exifOrientation), (id)kCGImagePropertyExifDictionary: exif,
        (id)kCGImagePropertyTIFFDictionary: @{(id)kCGImagePropertyTIFFMake: @"Apple",
                                              (id)kCGImagePropertyTIFFModel: @"iPhone Simulator (simcam)",
                                              (id)kCGImagePropertyTIFFOrientation: @(exifOrientation),
                                              (id)kCGImagePropertyTIFFDateTime: now}};
    CGImageDestinationAddImage(dst, cgImage, (__bridge CFDictionaryRef)meta);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(cgImage);
    if (metaOut) *metaOut = meta;
    return data;
}
static IOSurfaceRef PhotoSurfaceFromBytes(NSData *d) {
    NSDictionary *props = @{(id)kIOSurfaceWidth: @(d.length), (id)kIOSurfaceHeight: @1,
                            (id)kIOSurfaceBytesPerElement: @1, (id)kIOSurfacePixelFormat: @(0),
                            (id)kIOSurfaceAllocSize: @(d.length)};
    IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    IOSurfaceLock(surf, 0, NULL);
    memcpy(IOSurfaceGetBaseAddress(surf), d.bytes, d.length);
    IOSurfaceUnlock(surf, 0, NULL);
    return surf;
}
// slot 13: IrisStillImageSinkPrepareToCapture(session, sinkID, settings)
static OSStatus sess_IrisPrepare(CMBaseObjectRef s, CFStringRef sinkID, id settings) {
    TRRecord(TR_PREPARE, [SafeKVC(settings, @"settingsID") longLongValue], 0);
    LOG(@"IrisPrepareToCapture %@ %@", sinkID, DescribeShort(settings, 600));
    SessPost(s, @"IrisPreparationComplete", @{@"SectionID": (__bridge NSString *)sinkID,
                                                @"SettingsID": @([SafeKVC(settings, @"settingsID") longLongValue]),
                                                @"ErrorStatus": @0});
    return 0;
}
// slot 12: IrisStillImageSinkCaptureImage(session, sinkID, settings)
// Control-file event photo-error:<status> (core/control.inc) makes the next photo complete with that ErrorStatus and no
// image, to map Fig statuses to the NSError apps get (input for mid-session error completion, design §5.4).
static _Atomic int gPhotoErrNext;
// The photo's pixel size: the settings' output size, else its outputDimensions, else the format's maximum photo
// dimensions (4032x3024 for 4:3), else 4032 (SIMCAM_PHOTO_WIDTH) wide at the 16:9 canvas's aspect.
// SIMCAM_OFF_PHOTO_DIMS: outputDimensions and the format's maximum are ignored (the output size, else the old 16:9
// size).
static void PhotoOutputSize(id settings, int *wOut, int *hOut) {
    int w = [SafeKVC(settings, @"outputWidth") intValue], h = [SafeKVC(settings, @"outputHeight") intValue];
    CMVideoDimensions outDims = DimsFromValue(SafeKVC(settings, @"outputDimensions"));
    if ((w <= 0 || h <= 0) && outDims.width > 0 && !getenv(ENV_OFF_PHOTO_DIMS)) {
        w = outDims.width;
        h = outDims.height;
    }
    if ((w <= 0 || h <= 0) && gIrisMaxDims.width > 0 && !getenv(ENV_OFF_PHOTO_DIMS)) {
        w = gIrisMaxDims.width;
        h = gIrisMaxDims.height;
    }
    if (w <= 0 || h <= 0) {
        w = getenv(ENV_PHOTO_WIDTH) ? atoi(getenv(ENV_PHOTO_WIDTH)) : 4032;
        h = w * kSrcH / kSrcW;
    }
    *wOut = w;
    *hOut = h;
}
// SIMCAM_TEST_FIRST_PHOTO_STALL_MS=<ms> (test fault): the process's first photo stalls the frame pump for that long, as
// a lazy encoder setup on the pump queue would; later photos don't. The canary's in-process retry of the frame-gap
// check passes such a regression, so the CLI must report it as a retried pass, never a plain cached one. Pump queue.
static void TestFaultFirstPhotoStall(void) {
    static _Atomic int done;
    const char *ms = getenv(ENV_TEST_FIRST_PHOTO_STALL_MS);
    if (ms && atoi(ms) > 0 && atomic_exchange(&done, 1) == 0) {
        LOG(@"SIMCAM_TEST_FIRST_PHOTO_STALL_MS: first-photo stall %d ms", atoi(ms));
        usleep((useconds_t)atoi(ms) * 1000);
    }
}
// IrisWillBeginCapture's payload: the resolved sizes (previewW/H 0: no preview image), no thumbnail, flash, HDR or
// stabilization, and the settings' photo manifest when there is one
static NSDictionary *PhotoWillBeginPayload(NSDictionary *base, id settings, int w, int h, int previewW, int previewH) {
    NSMutableDictionary *willBegin = [base mutableCopy];
    [willBegin addEntriesFromDictionary:@{
        @"StillWidth": @(w), @"StillHeight": @(h), @"PreviewWidth": @(previewW), @"PreviewHeight": @(previewH),
        @"ThumbnailWidth": @0, @"ThumbnailHeight": @0, @"FlashActive": @NO, @"HDRActive": @NO, @"SISActive": @NO,
        @"ErrorStatus": @0}];
    id manifest = SafeKVC(settings, @"photoManifest");
    if (manifest) willBegin[@"PhotoManifest"] = manifest;
    return willBegin;
}
// A photo that completes with errStatus and no image (photo latch or the photo-error test hook): ends the photo op,
// posts the completion and releases the session. On the photo queue (the pump queue with SIMCAM_OFF_PHOTO_QUEUE).
static void PhotoCompleteWithError(CMBaseObjectRef session, NSDictionary *base, long long sid, int errStatus,
                                   BOOL latched, int op) {
    NSMutableDictionary *fail = [base mutableCopy];
    fail[@"ErrorStatus"] = @(errStatus);
    LOG(@"photo: completing with ErrorStatus %d (%s)", errStatus, latched ? "photo latch" : "test hook");
    SCOpEnd(op);
    SessPost(session, @"IrisStillImageComplete", fail);
    TRRecord(TR_STILL_COMPLETE, sid, errStatus);
    // SIMCAM_OFF_RESPONSIVE_NOTE: no ReadyForResponsiveRequest (AVF then never runs didFinishCapture)
    if (!getenv(ENV_OFF_RESPONSIVE_NOTE)) SessPost(session, @"ReadyForResponsiveRequest", base);
    // never wait on the pump queue from the photo queue (sess_IrisCapture)
    dispatch_async(gPumpQ, ^{ CFRelease(session); });
}
// img mirrored for a photo taken at rotDeg (consumes img, returns +1), with a flip session of its own
static CVPixelBufferRef PhotoMirrored(CVPixelBufferRef img, int rotDeg) {
    VTPixelRotationSessionRef flipSess = NULL;
    int flipKind = 0;
    CVPixelBufferRef flipped = MirrorFlip(img, rotDeg, &flipSess, &flipKind);
    if (flipSess) {
        VTPixelRotationSessionInvalidate(flipSess);
        CFRelease(flipSess);
    }
    CVPixelBufferRelease(img);
    LOG(@"photo mirrored (rot %d)", rotDeg);
    return flipped;
}
// The preview image (onPreviewImageAvailable / previewCGImageRepresentation): img at w x h in fmt (+1)
static CVPixelBufferRef PhotoPreviewImage(CVPixelBufferRef img, int w, int h, OSType fmt) {
    SimcamSink *conv = [SimcamSink new];   // conv frees its pool/transfer session (SimcamSink dealloc, L75)
    conv.w = w;
    conv.h = h;
    conv.fmt = fmt;
    return SinkConvert(conv, img);
}
// IrisStillImageComplete's payload for an encoded photo: the bytes' surface, metadata and timestamp, codec and file
// type when given, and the preview image's surface when there is one
static NSDictionary *PhotoCompletePayload(NSDictionary *base, IOSurfaceRef surf, NSUInteger size, NSDictionary *meta,
                                          CMTime pts, unsigned codec, unsigned ftype, CVPixelBufferRef previewPB) {
    NSMutableDictionary *complete = [base mutableCopy];
    [complete addEntriesFromDictionary:@{
        @"ErrorStatus": @0, @"Surface": (__bridge id)surf, @"SurfaceSize": @(size), @"Metadata": meta,
        @"PresentationTimestamp": (__bridge_transfer id)CMTimeCopyAsDictionary(pts, NULL)}];
    if (codec) complete[@"PhotoCodec"] = @(codec);
    if (ftype) complete[@"PhotoFileType"] = @(ftype);
    if (previewPB && CVPixelBufferGetIOSurface(previewPB)) {
        IOSurfaceRef previewSurf = CVPixelBufferGetIOSurface(previewPB);
        complete[@"PreviewSurface"] = (__bridge id)previewSurf;
        complete[@"PreviewSurfaceSize"] = @(IOSurfaceGetAllocSize(previewSurf));
    }
    return complete;
}
static OSStatus sess_IrisCapture(CMBaseObjectRef session, CFStringRef sinkIDRef, id settings) {
    NSString *sinkID = (__bridge NSString *)sinkIDRef;
    long long sid = [SafeKVC(settings, @"settingsID") longLongValue];
    TRRecord(TR_SLOT12, sid, 0);   // the slot-12 mark (design §9), before the (long) log line
    LOG(@"IrisCaptureImage %@ settings=%@", sinkID, DescribeShort(settings, 2500));
    DiagDumpProps(settings);
    int w, h;
    PhotoOutputSize(settings, &w, &h);
    // SIMCAM_OFF_PHOTO_PREVIEW: no preview image with the photo
    BOOL previewOn = [SafeKVC(settings, @"previewEnabled") boolValue] && !getenv(ENV_OFF_PHOTO_PREVIEW);
    int previewW = [SafeKVC(settings, @"previewWidth") intValue],
        previewH = [SafeKVC(settings, @"previewHeight") intValue];
    OSType previewFmt = [SafeKVC(settings, @"previewFormat") unsignedIntValue] ?: '420f';
    int rotDeg = SettingsRotationDeg(settings, @"outputRotationDegrees", @"outputOrientation",
                                     SessLookup(session).position);
    int exifOrientation = rotDeg == 90 ? 6 : rotDeg == 180 ? 3 : rotDeg == 270 ? 8 : 1;
    unsigned codec = [SafeKVC(settings, @"outputFormat") unsignedIntValue];
    unsigned ftype = [SafeKVC(settings, @"outputFileType") unsignedIntValue];
    BOOL hevc = codec == 'hvc1';
    NSString *uti = hevc ? @"public.heic" : @"public.jpeg";
    // pending until AVF has run the delegate's didFinishCapture (a stop waits for that)
    StopWaitPhotoBegin(session, sid);
    CFRetain(session);
    dispatch_async(gPumpQ, ^{
        NSMutableDictionary *base = [@{@"SectionID": sinkID, @"SettingsID": @(sid)} mutableCopy];
        NSDictionary *willBegin = PhotoWillBeginPayload(base, settings, w, h, previewOn ? previewW : 0,
                                                        previewOn ? previewH : 0);
        SessPost(session, @"IrisWillBeginCaptureBeforeResolvingSettings", base);
        SessPost(session, @"IrisWillBeginCapture", willBegin);
        SessPost(session, @"IrisWillCaptureStillImage", base);
        TRRecord(TR_WILL_CAPTURE, sid, 0);
        CVPixelBufferRef src = gLatest ? CVPixelBufferRetain(gLatest)
                                       : (SceneGeneratedOn() ? SceneRenderSource(SessLookup(session))
                                                             : SceneRenderFixture());
        // Ledger L68: the photo's timestamp (PresentationTimestamp -> AVCapturePhoto.timestamp) is this instant, when
        // its source frame is taken, as a device's is the sensor frame's: within a few ms of the slot-12 mark (design
        // §9 asks <= 50 ms). After the encode it would be 120-160 ms later for a 12 MP JPEG;
        // SIMCAM_OFF_GRAB_TIMESTAMP=1 stamps it there.
        BOOL oldPts = getenv(ENV_OFF_GRAB_TIMESTAMP) != NULL;
        // the wall clock too, the EXIF capture time (L72): one value, back-computed from grabMach, for both the
        // timestamp op and the EXIF, so a preemption between two clock reads can't put them apart
        uint64_t grabMach = mach_absolute_time(), grabWall = TRWallAt(grabMach);
        if (!oldPts)
            TRRecordAtWall(TR_TIMESTAMP, sid,
                           CMTimeConvertScale(CMClockMakeHostTimeFromSystemUnits(grabMach), 1000000000,
                                              kCMTimeRoundingMethod_Default).value,
                           grabMach, grabWall);
        TestFaultFirstPhotoStall();
        SessPost(session, @"IrisDidCaptureStillImage", base);
        TRRecord(TR_DID_CAPTURE, sid, 0);
        // encode, mirroring, the preview image and the completion run on their own serial queue, so a slow encode
        // (7.6 s once on a starved hosted runner) doesn't stall preview/VDO frames on the pump queue. Photos still
        // complete in request order; the early notifications above stay on the pump queue, in order with the frames.
        // Each pending photo holds one retained source frame (~8 MB BGRA at 1080p) until its encode; a burst of n holds
        // n of them. SIMCAM_OFF_PHOTO_QUEUE: encode on the pump queue instead, a negative control for the canary's
        // frame-gap check
        dispatch_async(getenv(ENV_OFF_PHOTO_QUEUE) ? gPumpQ : PhotoQueue(), ^{
            // the op starts when this photo's encode starts, so in a burst the k-th photo's 10 s doesn't include the
            // k-1 encodes before it; it ends before the still-complete is posted (our work only, not the app's
            // delegate)
            int op = SCOpBegin("photo", 10.0);
            TRRecord(TR_ENCODE_START, sid, 0);
            CVPixelBufferRef img = src;   // taken on the pump queue above; released below
            // decided per photo, when its encode starts: once photo is latched every photo still to encode
            // error-completes (including ones requested before the latch); otherwise the test hook's one-shot status,
            // if set
            BOOL latched = SCLatched("photo");
            int errStatus = latched ? SC_LATCH_STATUS : atomic_exchange(&gPhotoErrNext, 0);
            if (latched) LOG(@"photo latched: this capture error-completes (%d)", SC_LATCH_STATUS);
            if (errStatus) {
                CVPixelBufferRelease(img);
                PhotoCompleteWithError(session, base, sid, errStatus, latched, op);
                return;
            }
            NSDictionary *meta = nil;
            // device-like: landscape sensor pixels + EXIF orientation 6 (rotate 90° CW for portrait display)
            if (OrientVirtualOn() && [SafeKVC(settings, @"outputMirroring") boolValue])
                img = PhotoMirrored(img, rotDeg);
            // SIMCAM_PHOTO_EXIF_ORIENTATION: a fixed EXIF orientation
            NSData *encoded = PhotoEncode(img, w, h, uti,
                                            getenv(ENV_PHOTO_EXIF_ORIENTATION)
                                                ? atoi(getenv(ENV_PHOTO_EXIF_ORIENTATION)) : exifOrientation,
                                            &meta, [NSDate dateWithTimeIntervalSince1970:grabWall / 1e9]);
            // preview image (onPreviewImageAvailable / previewCGImageRepresentation)
            CVPixelBufferRef previewPB = NULL;
            if (previewOn && previewW > 0 && previewH > 0)
                previewPB = PhotoPreviewImage(img, previewW, previewH, previewFmt);
            CVPixelBufferRelease(img);
            IOSurfaceRef surf = PhotoSurfaceFromBytes(encoded);
            TRRecord(TR_ENCODED, sid, (int64_t)encoded.length);
            // the frame grab (above); SIMCAM_OFF_GRAB_TIMESTAMP: now, after the encode
            uint64_t ptsMach = oldPts ? mach_absolute_time() : grabMach;
            CMTime pts = CMClockMakeHostTimeFromSystemUnits(ptsMach);
            NSDictionary *complete = PhotoCompletePayload(base, surf, encoded.length, meta, pts, codec, ftype,
                                                          previewPB);
            LOG(@"photo encoded %lu bytes as %@ (codec=%.4s ftype=%.4s) %dx%d", (unsigned long)encoded.length, uti,
                (char *)&(unsigned){CFSwapInt32HostToBig(codec)}, (char *)&(unsigned){CFSwapInt32HostToBig(ftype)},
                w, h);
            SCOpEnd(op);
            if (oldPts)
                TRRecordAt(TR_TIMESTAMP, sid, CMTimeConvertScale(pts, 1000000000, kCMTimeRoundingMethod_Default).value,
                           ptsMach);
            SessPost(session, @"IrisStillImageComplete", complete);
            TRRecord(TR_STILL_COMPLETE, sid, 0);
            // AVF fires captureOutput:didFinishCaptureForResolvedSettings: only after ReadyForResponsiveRequest (sets
            // the 0x8000 callback flag, then -_handlePotentiallyFinalPhotoRequestCallbackWithPayload:). Also drives
            // readyForResponsiveRequestAfterResolvedSettings: / readiness.
            if (!getenv(ENV_OFF_RESPONSIVE_NOTE)) SessPost(session, @"ReadyForResponsiveRequest", base);
            CFRelease(surf);
            if (previewPB) CVPixelBufferRelease(previewPB);
            // Never wait on the pump queue from this queue: an interruption waits on this queue from inside a
            // pump-queue block (core/control.inc). A last release here would run the session's finalize, which syncs to
            // the pump queue; release on the pump queue instead, where finalize runs inline (SessForget).
            dispatch_async(gPumpQ, ^{ CFRelease(session); });
        });
    });
    return 0;
}

