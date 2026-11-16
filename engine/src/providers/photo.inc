// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): photo encode and still-image slots.
// ---- photo ----------------------------------------------------------------------
// Ledger L72: the EXIF/TIFF capture times are `when`, the wall clock at the frame grab (the instant
// the photo's timestamp is, L68), with milliseconds (SubsecTime*) and the UTC offset (OffsetTime*), as a device writes them;
// the encode's time would be 100-480 ms later. M54_OLD_EXIF=1 stamps them at the encode (with the subseconds).
static NSData *M5EncodePhoto(CVPixelBufferRef src, int w, int h, NSString *uti, int exifOrientation, NSDictionary **metaOut, NSDate *when) {
    CIImage *ci = [CIImage imageWithCVPixelBuffer:src];
    // the source is the 1920x1080 canvas, or the webcam canvas at the ring's size
    if (w > 0 && h > 0) ci = [ci imageByApplyingTransform:CGAffineTransformMakeScale((double)w / CVPixelBufferGetWidth(src), (double)h / CVPixelBufferGetHeight(src))];
    static CIContext *ctx; if (!ctx) ctx = [CIContext contextWithOptions:nil];
    CGImageRef cg = [ctx createCGImage:ci fromRect:ci.extent];
    NSMutableData *d = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)d, (__bridge CFStringRef)uti, 1, NULL);
    if (!dst && ![uti isEqualToString:@"public.jpeg"]) { LOG(@"photo: no encoder for %@, using JPEG", uti); dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)d, CFSTR("public.jpeg"), 1, NULL); }
    if (!when || getenv("M54_OLD_EXIF")) when = [NSDate date];
    NSDateFormatter *f = [NSDateFormatter new]; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    f.dateFormat = @"yyyy:MM:dd HH:mm:ss"; NSString *now = [f stringFromDate:when];
    f.dateFormat = @"SSS"; NSString *sub = [f stringFromDate:when];
    f.dateFormat = @"xxx"; NSString *ofs = [f stringFromDate:when];
    NSDictionary *exif = @{(id)kCGImagePropertyExifDateTimeOriginal: now, (id)kCGImagePropertyExifDateTimeDigitized: now, (id)kCGImagePropertyExifSubsecTimeOriginal: sub, (id)kCGImagePropertyExifSubsecTimeDigitized: sub,
                           (id)kCGImagePropertyExifOffsetTimeOriginal: ofs, (id)kCGImagePropertyExifOffsetTimeDigitized: ofs, (id)kCGImagePropertyExifSubsecTime: sub, (id)kCGImagePropertyExifOffsetTime: ofs,
                           (id)kCGImagePropertyExifPixelXDimension: @(CGImageGetWidth(cg)), (id)kCGImagePropertyExifPixelYDimension: @(CGImageGetHeight(cg)),
                           (id)kCGImagePropertyExifLensMake: @"Apple", (id)kCGImagePropertyExifLensModel: @"simcam virtual back camera", (id)kCGImagePropertyExifFocalLength: @6.765, (id)kCGImagePropertyExifFNumber: @1.78,
                           (id)kCGImagePropertyExifExposureTime: @(1.0 / 60), (id)kCGImagePropertyExifISOSpeedRatings: @[@100]};
    NSDictionary *meta = @{(id)kCGImagePropertyOrientation: @(exifOrientation), (id)kCGImagePropertyExifDictionary: exif,
                           (id)kCGImagePropertyTIFFDictionary: @{(id)kCGImagePropertyTIFFMake: @"Apple", (id)kCGImagePropertyTIFFModel: @"iPhone Simulator (simcam)", (id)kCGImagePropertyTIFFOrientation: @(exifOrientation), (id)kCGImagePropertyTIFFDateTime: now}};
    CGImageDestinationAddImage(dst, cg, (__bridge CFDictionaryRef)meta);
    CGImageDestinationFinalize(dst); CFRelease(dst); CGImageRelease(cg);
    if (metaOut) *metaOut = meta;
    return d;
}
static IOSurfaceRef M5BytesSurface(NSData *d) {
    NSDictionary *p = @{(id)kIOSurfaceWidth: @(d.length), (id)kIOSurfaceHeight: @1, (id)kIOSurfaceBytesPerElement: @1, (id)kIOSurfacePixelFormat: @(0), (id)kIOSurfaceAllocSize: @(d.length)};
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    IOSurfaceLock(s, 0, NULL); memcpy(IOSurfaceGetBaseAddress(s), d.bytes, d.length); IOSurfaceUnlock(s, 0, NULL);
    return s;
}
// slot 13: IrisStillImageSinkPrepareToCapture(session, sinkID, settings)
static OSStatus sess_IrisPrepare(CMBaseObjectRef s, CFStringRef sinkID, id settings) {
    TRRecord(TR_PREPARE, [M5KVC(settings, @"settingsID") longLongValue], 0);
    LOG(@"IrisPrepareToCapture %@ %@", sinkID, M5Desc(settings, 600));
    M5SessPost(s, @"IrisPreparationComplete", @{@"SectionID": (__bridge NSString *)sinkID, @"SettingsID": @([M5KVC(settings, @"settingsID") longLongValue]), @"ErrorStatus": @0});
    return 0;
}
// slot 12: IrisStillImageSinkCaptureImage(session, sinkID, settings)
// Control-file event photo-error:<status> (core/control.inc) makes the next photo complete with that ErrorStatus and no image,
// to map Fig statuses to the NSError apps get (input for mid-session error completion, design §5.4).
static _Atomic int gPhotoErrNext;
static OSStatus sess_IrisCapture(CMBaseObjectRef s, CFStringRef sinkIDr, id settings) {
    NSString *sinkID = (__bridge NSString *)sinkIDr;
    long long sid = [M5KVC(settings, @"settingsID") longLongValue];
    TRRecord(TR_SLOT12, sid, 0);   // the slot-12 mark (design §9), before the (long) log line
    LOG(@"IrisCaptureImage %@ settings=%@", sinkID, M5Desc(settings, 2500));
    M5DumpProps(settings);
    int w = [M5KVC(settings, @"outputWidth") intValue], h = [M5KVC(settings, @"outputHeight") intValue];
    CMVideoDimensions od = M8Dims(M5KVC(settings, @"outputDimensions"));
    if ((w <= 0 || h <= 0) && od.width > 0 && !getenv("M8_NO_PHOTODIMS")) { w = od.width; h = od.height; }
    if ((w <= 0 || h <= 0) && gIrisMaxDims.width > 0 && !getenv("M8_NO_PHOTODIMS")) { w = gIrisMaxDims.width; h = gIrisMaxDims.height; }   // 4032x3024 for 4:3
    if (w <= 0 || h <= 0) { w = getenv("M5_PHOTO_W") ? atoi(getenv("M5_PHOTO_W")) : 4032; h = w * kSrcH / kSrcW; }
    BOOL pvOn = [M5KVC(settings, @"previewEnabled") boolValue] && !getenv("M8_NO_PREVIEWSURFACE");
    int pvW = [M5KVC(settings, @"previewWidth") intValue], pvH = [M5KVC(settings, @"previewHeight") intValue]; OSType pvF = [M5KVC(settings, @"previewFormat") unsignedIntValue] ?: '420f';
    int rotDeg = M6Deg(settings, @"outputRotationDegrees", @"outputOrientation", M5S(s).position);
    int exifO = rotDeg == 90 ? 6 : rotDeg == 180 ? 3 : rotDeg == 270 ? 8 : 1;
    unsigned codec = [M5KVC(settings, @"outputFormat") unsignedIntValue];
    unsigned ftype = [M5KVC(settings, @"outputFileType") unsignedIntValue];
    BOOL hevc = codec == 'hvc1';
    NSString *uti = hevc ? @"public.heic" : @"public.jpeg";
    M40PhotoBegin(s, sid);   // pending until AVF has run the delegate's didFinishCapture (a stop waits for that)
    CFRetain(s);
    dispatch_async(gPumpQ, ^{
        NSMutableDictionary *base = [@{@"SectionID": sinkID, @"SettingsID": @(sid)} mutableCopy];
        NSMutableDictionary *wb = [base mutableCopy];
        [wb addEntriesFromDictionary:@{@"StillWidth": @(w), @"StillHeight": @(h), @"PreviewWidth": @(pvOn ? pvW : 0), @"PreviewHeight": @(pvOn ? pvH : 0), @"ThumbnailWidth": @0, @"ThumbnailHeight": @0, @"FlashActive": @NO, @"HDRActive": @NO, @"SISActive": @NO, @"ErrorStatus": @0}];
        id manifest = M5KVC(settings, @"photoManifest"); if (manifest) wb[@"PhotoManifest"] = manifest;
        M5SessPost(s, @"IrisWillBeginCaptureBeforeResolvingSettings", base);
        M5SessPost(s, @"IrisWillBeginCapture", wb);
        M5SessPost(s, @"IrisWillCaptureStillImage", base);
        TRRecord(TR_WILL_CAPTURE, sid, 0);
        CVPixelBufferRef src = gLatest ? CVPixelBufferRetain(gLatest) : (M12Scene2On() ? M12Render(M5S(s)) : M5Render());
        // Ledger L68: the photo's timestamp (PresentationTimestamp -> AVCapturePhoto.timestamp) is this instant, when
        // its source frame is taken, as a device's is the sensor frame's: within a few ms of the slot-12 mark (design §9 asks
        // <= 50 ms). After the encode it would be 120-160 ms later for a 12 MP JPEG; M48_OLD_PTS=1 stamps it there.
        BOOL oldPts = getenv("M48_OLD_PTS") != NULL;
        // the wall clock too, the EXIF capture time (L72): one value, back-computed from grabMach, for both the timestamp op and
        // the EXIF, so a preemption between two clock reads can't put them apart
        uint64_t grabMach = mach_absolute_time(), grabWall = TRWallAt(grabMach);
        if (!oldPts) TRRecordAtWall(TR_TIMESTAMP, sid, CMTimeConvertScale(CMClockMakeHostTimeFromSystemUnits(grabMach), 1000000000, kCMTimeRoundingMethod_Default).value, grabMach, grabWall);
        // M38_FIRST_PHOTO_STALL_MS=<ms> (test fault): the process's first photo stalls the frame pump for that long, as
        // a lazy encoder setup on the pump queue would; later photos don't. The canary's in-process retry of the frame-gap
        // check passes such a regression, so the CLI must report it as a retried pass, never a plain cached one.
        { static _Atomic int done; const char *ms = getenv("M38_FIRST_PHOTO_STALL_MS");
          if (ms && atoi(ms) > 0 && atomic_exchange(&done, 1) == 0) { LOG(@"M38 first-photo stall %d ms", atoi(ms)); usleep((useconds_t)atoi(ms) * 1000); } }
        M5SessPost(s, @"IrisDidCaptureStillImage", base);
        TRRecord(TR_DID_CAPTURE, sid, 0);
        // encode, mirroring, the preview image and the completion run on their own serial queue, so a slow encode
        // (7.6 s once on a starved hosted runner) doesn't stall preview/VDO frames on the pump queue. Photos still complete
        // in request order; the early notifications above stay on the pump queue, in order with the frames. Each pending photo
        // holds one retained source frame (~8 MB BGRA at 1080p) until its encode; a burst of n holds n of them.
        // M13_PHOTO_ON_PUMP: the old placement (encode on the pump queue), a negative control for the canary's frame-gap check
        dispatch_async(getenv("M13_PHOTO_ON_PUMP") ? gPumpQ : M5PhotoQ(), ^{
            // the op starts when this photo's encode starts, so in a burst the k-th photo's 10 s doesn't include the k-1
            // encodes before it; it ends before the still-complete is posted (our work only, not the app's delegate)
            int op = SCOpBegin("photo", 10.0);
            TRRecord(TR_ENCODE_START, sid, 0);
            CVPixelBufferRef img = src;   // taken on the pump queue above; released below
            // decided per photo, when its encode starts: once photo is latched every photo still to encode error-completes
            // (including ones requested before the latch); otherwise the test hook's one-shot status, if set
            BOOL latched = SCLatched("photo");
            int perr = latched ? SC_LATCH_STATUS : atomic_exchange(&gPhotoErrNext, 0);
            if (latched) LOG(@"photo latched: this capture error-completes (%d)", SC_LATCH_STATUS);
            if (perr) {
                CVPixelBufferRelease(img);
                NSMutableDictionary *fail = [base mutableCopy]; fail[@"ErrorStatus"] = @(perr);
                LOG(@"photo: completing with ErrorStatus %d (%s)", perr, latched ? "photo latch" : "test hook");
                SCOpEnd(op);
                M5SessPost(s, @"IrisStillImageComplete", fail);
                TRRecord(TR_STILL_COMPLETE, sid, perr);
                if (!getenv("M10_NO_RESPONSIVE")) M5SessPost(s, @"ReadyForResponsiveRequest", base);
                dispatch_async(gPumpQ, ^{ CFRelease(s); }); return;   // never wait on the pump queue from here (see below)
            }
            NSDictionary *meta = nil;
            // device-like: landscape sensor pixels + EXIF orientation 6 (rotate 90° CW for portrait display)
            if (M12OrientOn() && [M5KVC(settings, @"outputMirroring") boolValue]) { VTPixelRotationSessionRef fs = NULL; int fk = 0; CVPixelBufferRef f = M12Flip(img, rotDeg, &fs, &fk); if (fs) { VTPixelRotationSessionInvalidate(fs); CFRelease(fs); } CVPixelBufferRelease(img); img = f; LOG(@"m12 photo mirrored (rot %d)", rotDeg); }
            NSData *enc = M5EncodePhoto(img, w, h, uti, getenv("M5_PHOTO_ORIENT") ? atoi(getenv("M5_PHOTO_ORIENT")) : exifO, &meta, [NSDate dateWithTimeIntervalSince1970:grabWall / 1e9]);
            CVPixelBufferRef pvpb = NULL;   // preview image (onPreviewImageAvailable / previewCGImageRepresentation)
            if (pvOn && pvW > 0 && pvH > 0) { M5Sink *cv = [M5Sink new]; cv.w = pvW; cv.h = pvH; cv.fmt = pvF; pvpb = M5Convert(cv, img); }   // cv frees its pool/transfer session (M5Sink dealloc, L75)
            CVPixelBufferRelease(img);
            IOSurfaceRef surf = M5BytesSurface(enc);
            TRRecord(TR_ENCODED, sid, (int64_t)enc.length);
            uint64_t ptsMach = oldPts ? mach_absolute_time() : grabMach;   // the frame grab (above); M48_OLD_PTS: now, after the encode
            CMTime pts = CMClockMakeHostTimeFromSystemUnits(ptsMach);
            NSMutableDictionary *done = [base mutableCopy];
            [done addEntriesFromDictionary:@{@"ErrorStatus": @0, @"Surface": (__bridge id)surf, @"SurfaceSize": @(enc.length), @"Metadata": meta,
                                             @"PresentationTimestamp": (__bridge_transfer id)CMTimeCopyAsDictionary(pts, NULL)}];
            if (codec) done[@"PhotoCodec"] = @(codec);
            if (ftype) done[@"PhotoFileType"] = @(ftype);
            if (pvpb && CVPixelBufferGetIOSurface(pvpb)) { IOSurfaceRef ps = CVPixelBufferGetIOSurface(pvpb); done[@"PreviewSurface"] = (__bridge id)ps; done[@"PreviewSurfaceSize"] = @(IOSurfaceGetAllocSize(ps)); }
            LOG(@"photo encoded %lu bytes as %@ (codec=%.4s ftype=%.4s) %dx%d", (unsigned long)enc.length, uti, (char *)&(unsigned){CFSwapInt32HostToBig(codec)}, (char *)&(unsigned){CFSwapInt32HostToBig(ftype)}, w, h);
            SCOpEnd(op);
            if (oldPts) TRRecordAt(TR_TIMESTAMP, sid, CMTimeConvertScale(pts, 1000000000, kCMTimeRoundingMethod_Default).value, ptsMach);
            M5SessPost(s, @"IrisStillImageComplete", done);
            TRRecord(TR_STILL_COMPLETE, sid, 0);
            // AVF fires captureOutput:didFinishCaptureForResolvedSettings: only after ReadyForResponsiveRequest (sets the 0x8000 callback flag,
            // then -_handlePotentiallyFinalPhotoRequestCallbackWithPayload:). Also drives readyForResponsiveRequestAfterResolvedSettings: / readiness.
            if (!getenv("M10_NO_RESPONSIVE")) M5SessPost(s, @"ReadyForResponsiveRequest", base);
            CFRelease(surf); if (pvpb) CVPixelBufferRelease(pvpb);
            // Never wait on the pump queue from this queue: an interruption waits on this queue from inside a pump-queue
            // block (core/control.inc). A last release here would run the session's finalize, which syncs to the pump queue;
            // release on the pump queue instead, where finalize runs inline (M10ForgetSession).
            dispatch_async(gPumpQ, ^{ CFRelease(s); });
        });
    });
    return 0;
}

