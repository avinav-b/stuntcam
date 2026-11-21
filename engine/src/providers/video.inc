// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): frame source, per-sink conversion, preview frames.
// ---- frame source ------------------------------------------------------------
static CGImageRef gSrcImage;
static CVPixelBufferPoolRef gSrcPool;
static CVPixelBufferRef gLatest;          // latest BGRA source frame (retained)
static const int kSrcW = 1920, kSrcH = 1080;
static long gFrameNo;
static double gPumpFps;   // the pump's current frame rate
// the pump's frame rate, 30 before the first start
static double PumpFps(void) { return gPumpFps > 0 ? gPumpFps : 30; }
static CVPixelBufferRef SceneRenderFixture(void) {
    if (!gSrcPool) {
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id)kCVPixelBufferWidthKey: @(kSrcW), (id)kCVPixelBufferHeightKey: @(kSrcH),
                                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &gSrcPool);
        // the fixture image drawn into the plain scene
        const char *img = EnvConfig(ENV_FIXTURE_IMAGE, ENV_LEGACY_FIXTURE_IMAGE);
        if (img) {
            CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(img)],
                                                                 NULL);
            if (source) {
                gSrcImage = CGImageSourceCreateImageAtIndex(source, 0, NULL);
                CFRelease(source);
            }
        }
    }
    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, gSrcPool, &pb);
    if (!pb) return NULL;   // no buffer from the pool: no frame this tick
    CVPixelBufferLockBaseAddress(pb, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pb), kSrcW, kSrcH, 8,
                                             CVPixelBufferGetBytesPerRow(pb), cs,
                                             kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGContextSetRGBFillColor(ctx, 0.15, 0.3, 0.45, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, kSrcW, kSrcH));
    // sensor-like: landscape buffer whose content is upright when rotated 90° clockwise (portrait device)
    CGContextSaveGState(ctx);
    // Rotating by -pi/2 (SIMCAM_OFF_SENSOR_LAYOUT) puts the scene's top on the buffer's RIGHT (photos with EXIF 6 and
    // movies with the 90 deg track transform come out upside down). A device's native back-camera buffer has the
    // scene's top on the LEFT. SIMCAM_OFF_SENSOR_LAYOUT keeps that old layout.
    CGContextTranslateCTM(ctx, kSrcW / 2.0, kSrcH / 2.0);
    CGContextRotateCTM(ctx, getenv(ENV_OFF_SENSOR_LAYOUT) ? -M_PI_2 : M_PI_2);
    // upright canvas is kSrcH wide x kSrcW tall (portrait); aspect-fit the fixture into it
    if (gSrcImage) {
        double drawW = kSrcH * 0.95, drawH = drawW * CGImageGetHeight(gSrcImage) / CGImageGetWidth(gSrcImage);
        CGContextDrawImage(ctx, CGRectMake(-drawW / 2, -drawH / 2, drawW, drawH), gSrcImage);
    }
    // "up" marker: red bar near the top of the upright picture
    CGContextSetRGBFillColor(ctx, 0.9, 0.1, 0.1, 1);
    CGContextFillRect(ctx, CGRectMake(-kSrcH * 0.4, kSrcW * 0.38, kSrcH * 0.8, 40));
    CGContextRestoreGState(ctx);
    // moving bar
    double x = (gFrameNo * 12) % kSrcW;
    CGContextSetRGBFillColor(ctx, 1, 1, 0.2, 1);
    CGContextFillRect(ctx, CGRectMake(x, 0, 24, 60));
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    gFrameNo++;
    return pb;
}

static CVPixelBufferRef SinkConvertScale(SimcamSink *k, CVPixelBufferRef src);
// Apply the connection's rotationDegrees (clockwise, like the device's ISP) and mirroring before scaling/format
// conversion. SIMCAM_OFF_DATA_OUTPUT_ROTATION turns this off (the buffer as the sensor gives it).
static CVPixelBufferRef SinkRotate(SimcamSink *k, CVPixelBufferRef src) {
    if (getenv(ENV_OFF_DATA_OUTPUT_ROTATION) || (k.vdoRot % 360 == 0 && !k.mirrored)) return CVPixelBufferRetain(src);
    int deg = ((k.vdoRot % 360) + 360) % 360;
    BOOL swap = (deg == 90 || deg == 270);
    size_t w = CVPixelBufferGetWidth(src), h = CVPixelBufferGetHeight(src);
    if (!k.rotSess) {
        VTPixelRotationSessionRef rotSess = NULL;
        VTPixelRotationSessionCreate(NULL, &rotSess);
        // a quarter turn is clockwise (SIMCAM_OFF_SENSOR_LAYOUT, the old sensor layout: the quarter turns swapped, 90
        // counter-clockwise and 270 clockwise)
        VTSessionSetProperty(rotSess, kVTPixelRotationPropertyKey_Rotation,
                             ((deg == 90) != !!getenv(ENV_OFF_SENSOR_LAYOUT) && (deg == 90 || deg == 270))
                                 ? kVTRotation_CW90
                             : deg == 180                ? kVTRotation_180
                             : (deg == 90 || deg == 270) ? kVTRotation_CCW90
                                                         : kVTRotation_0);
        if (k.mirrored)
            VTSessionSetProperty(rotSess, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
        k.rotSess = rotSess;
        CVPixelBufferPoolRef pool = NULL;
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id)kCVPixelBufferWidthKey: @(swap ? h : w),
                                (id)kCVPixelBufferHeightKey: @(swap ? w : h),
                                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &pool);
        k.rotPool = pool;
        LOG(@"vdo %@ rotating %d deg cw mirrored=%d -> %zux%zu then %dx%d", k.sinkID, deg, k.mirrored, swap ? h : w,
            swap ? w : h, k.w, k.h);
    }
    CVPixelBufferRef dst = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, k.rotPool, &dst);
    OSStatus e = VTPixelRotationSessionRotateImage(k.rotSess, src, dst);
    // On a failed rotation the sink still gets dst (its pixels not rotated), so it keeps its frame rate; the error is
    // logged once per process, not per frame.
    if (e) {
        static int once;
        if (!once++) LOG(@"VTPixelRotation -> %d", (int)e);
    }
    return dst;
}
static CVPixelBufferRef SinkConvert(SimcamSink *k, CVPixelBufferRef src0) {
    CVPixelBufferRef src = [k.kind isEqualToString:@"vdo"] ? SinkRotate(k, src0) : CVPixelBufferRetain(src0);
    if (k.fmt == kCVPixelFormatType_32BGRA && k.w == (int)CVPixelBufferGetWidth(src) &&
        k.h == (int)CVPixelBufferGetHeight(src))
        return src;
    CVPixelBufferRef out = SinkConvertScale(k, src);
    CVPixelBufferRelease(src);
    return out;
}
static CVPixelBufferRef SinkConvertScale(SimcamSink *k, CVPixelBufferRef src) {
    if (!k.pool) {
        CVPixelBufferPoolRef pool = NULL;
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(k.fmt), (id)kCVPixelBufferWidthKey: @(k.w),
                                (id)kCVPixelBufferHeightKey: @(k.h), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &pool);
        k.pool = pool;
        VTPixelTransferSessionRef xfer = NULL;
        VTPixelTransferSessionCreate(NULL, &xfer);
        k.xfer = xfer;
    }
    CVPixelBufferRef dst = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, k.pool, &dst);
    OSStatus e = VTPixelTransferSessionTransferImage(k.xfer, src, dst);
    // As in SinkRotate: on a failed transfer the sink still gets dst (its pixels not converted); logged once
    // per process.
    if (e) {
        static int once;
        if (!once++) LOG(@"VTPixelTransfer -> %d", (int)e);
    }
    return dst;
}
// The type field of an LocalQueueMsg (FigLocalQueueEnqueue): the message carries a sample buffer.
enum { kLocalQueueMsgSampleBuffer = 3 };
static CMSampleBufferRef SinkSampleBuffer(CVPixelBufferRef pb, CMTime pts) {
    CMVideoFormatDescriptionRef fd = NULL;
    CMVideoFormatDescriptionCreateForImageBuffer(NULL, pb, &fd);
    CMSampleTimingInfo timing = { CMTimeMake(1, (int32_t)PumpFps()), pts, kCMTimeInvalid };
    CMSampleBufferRef sb = NULL;
    CMSampleBufferCreateReadyWithImageBuffer(NULL, pb, fd, &timing, &sb);
    CFRelease(fd);
    return sb;
}

// ---- preview ----------------------------------------------------------------
static CFTypeRef (*pCAImageQueueCreate)(uint32_t, uint32_t, uint32_t);
static uint64_t (*pCAImageQueueRegisterIOSurfaceBuffer)(CFTypeRef, IOSurfaceRef, uint32_t);
static bool (*pCAImageQueueInsertImage)(CFTypeRef, double, uint32_t, uint64_t, uint32_t, void *, void *);
static void PreviewPush(CMBaseObjectRef s, SimcamSink *k, CVPixelBufferRef pb) {
    IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
    // SIMCAM_PREVIEW_IMAGE_QUEUE: the CAImageQueue path instead (its arguments are unresolved: it shows nothing)
    BOOL surfaceMode = getenv(ENV_PREVIEW_IMAGE_QUEUE) == NULL || !pCAImageQueueCreate;
    if (surfaceMode) {   // post the IOSurface itself as "ImageQueue" (CALayer.contents accepts IOSurface)
        gQuietPost = YES;
        NSDictionary *info = @{@"SectionID": k.sinkID, @"ImageQueue": (__bridge id)surf, @"ImageQueueWidth": @(k.w),
                               @"ImageQueueHeight": @(k.h), @"RotationDegrees": @(k.rotation)};
        pCMNotificationCenterPostNotification(pCMNotificationCenterGetDefaultLocalCenter(), CFSTR("ImageQueueUpdated"),
                                              s, (__bridge CFDictionaryRef)info, 0);
        if (k.frames == 0) {
            static int firstEver;
            if (!firstEver++)
                LOG(@"TIMING first preview frame %.1f ms after process start",
                    (CFAbsoluteTimeGetCurrent() - ActProcessStartTime()) * 1000);
            LOG(@"preview(surface) first ImageQueueUpdated %@", k.sinkID);
            SessPost(s, @"DidStartPreviewing", @{@"SectionID": k.sinkID});
            SessPost(s, @"DidDisplayFirstPreviewFrame", @{@"SectionID": k.sinkID});
        }
        k.frames++;
        return;
    }
    if (!k.imageQueue) {
        k.imageQueue = pCAImageQueueCreate(k.w, k.h, 8);
        k.regIDs = [NSMutableDictionary dictionary];
        LOG(@"preview CAImageQueueCreate(%d,%d) -> %p", k.w, k.h, k.imageQueue);
        SessPost(s, @"ImageQueueUpdated", @{@"SectionID": k.sinkID, @"ImageQueue": (__bridge id)k.imageQueue,
                                              @"ImageQueueWidth": @(k.w), @"ImageQueueHeight": @(k.h),
                                              @"RotationDegrees": @(k.rotation), @"ImageQueueSlot": @0});
        SessPost(s, @"DidStartPreviewing", @{@"SectionID": k.sinkID});
    }
    NSNumber *surfID = @(IOSurfaceGetID(surf));
    NSNumber *regID = k.regIDs[surfID];
    if (!regID) {
        regID = @(pCAImageQueueRegisterIOSurfaceBuffer(k.imageQueue, surf, 0));
        k.regIDs[surfID] = regID;
        CFRetain(surf);   // keep registered surfaces alive
    }
    // SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_A / SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_B: the two unknown integer arguments of
    // CAImageQueueInsertImage (probing)
    uint32_t argA = getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_A) ? atoi(getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_A)) : 0,
             argB = getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_B) ? atoi(getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_B)) : 0;
    bool ok = pCAImageQueueInsertImage(k.imageQueue, -1.0, argA, regID.unsignedLongLongValue, argB, NULL, NULL);
    if (k.frames < 3 || !ok) LOG(@"preview insert frame %d id=%@ -> %d", k.frames, regID, ok);
    if (k.frames == 1) SessPost(s, @"DidDisplayFirstPreviewFrame", @{@"SectionID": k.sinkID});
    k.frames++;
}
