// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): frame source, preview, barcode fixture.
// ---- frame source ------------------------------------------------------------
static CGImageRef gSrcImage;
static CVPixelBufferPoolRef gSrcPool;
static CVPixelBufferRef gLatest;          // latest BGRA source frame (retained)
static const int kSrcW = 1920, kSrcH = 1080;
static long gFrameNo;
static double gPumpFps;   // m8
static CVPixelBufferRef M5Render(void) {
    if (!gSrcPool) {
        NSDictionary *a = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferWidthKey: @(kSrcW), (id)kCVPixelBufferHeightKey: @(kSrcH), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)a, &gSrcPool);
        const char *img = getenv("M5_IMAGE");
        if (img) { CGImageSourceRef is = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(img)], NULL); if (is) { gSrcImage = CGImageSourceCreateImageAtIndex(is, 0, NULL); CFRelease(is); } }
    }
    CVPixelBufferRef pb = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, gSrcPool, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pb), kSrcW, kSrcH, 8, CVPixelBufferGetBytesPerRow(pb), cs, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGContextSetRGBFillColor(ctx, 0.15, 0.3, 0.45, 1); CGContextFillRect(ctx, CGRectMake(0, 0, kSrcW, kSrcH));
    // sensor-like: landscape buffer whose content is upright when rotated 90° clockwise (portrait device)
    CGContextSaveGState(ctx);
    // m7: m5 rotated by -pi/2, which put the scene's top on the buffer's RIGHT (photos with EXIF 6 and movies with the 90 deg
    // track transform came out upside down). A device's native back-camera buffer has the scene's top on the LEFT.
    CGContextTranslateCTM(ctx, kSrcW / 2.0, kSrcH / 2.0); CGContextRotateCTM(ctx, getenv("M7_SRC_LEGACY") ? -M_PI_2 : M_PI_2);
    // upright canvas is kSrcH wide x kSrcW tall (portrait); aspect-fit the fixture into it
    if (gSrcImage) { double W = kSrcH * 0.95, H = W * CGImageGetHeight(gSrcImage) / CGImageGetWidth(gSrcImage); CGContextDrawImage(ctx, CGRectMake(-W / 2, -H / 2, W, H), gSrcImage); }
    // "up" marker: red bar near the top of the upright picture
    CGContextSetRGBFillColor(ctx, 0.9, 0.1, 0.1, 1); CGContextFillRect(ctx, CGRectMake(-kSrcH * 0.4, kSrcW * 0.38, kSrcH * 0.8, 40));
    CGContextRestoreGState(ctx);
    double x = (gFrameNo * 12) % kSrcW; CGContextSetRGBFillColor(ctx, 1, 1, 0.2, 1); CGContextFillRect(ctx, CGRectMake(x, 0, 24, 60)); // moving bar
    CGContextRelease(ctx); CGColorSpaceRelease(cs);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    gFrameNo++;
    return pb;
}

static CVPixelBufferRef M5ConvertScale(M5Sink *k, CVPixelBufferRef src);
// m7: apply the connection's rotationDegrees (clockwise, like the device's ISP) and mirroring before scaling/format conversion.
static CVPixelBufferRef M7Rotate(M5Sink *k, CVPixelBufferRef src) {
    if (getenv("M7_NO_VDOROT") || (k.vdoRot % 360 == 0 && !k.mirrored)) return CVPixelBufferRetain(src);
    int r = ((k.vdoRot % 360) + 360) % 360; BOOL swap = (r == 90 || r == 270);
    size_t w = CVPixelBufferGetWidth(src), h = CVPixelBufferGetHeight(src);
    if (!k.rotSess) {
        VTPixelRotationSessionRef rs = NULL; VTPixelRotationSessionCreate(NULL, &rs);
        VTSessionSetProperty(rs, kVTPixelRotationPropertyKey_Rotation, ((r == 90) != !!getenv("M7_SRC_LEGACY") && (r == 90 || r == 270) ? kVTRotation_CW90 : r == 180 ? kVTRotation_180 : (r == 90 || r == 270) ? kVTRotation_CCW90 : kVTRotation_0));
        if (k.mirrored) VTSessionSetProperty(rs, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
        k.rotSess = rs;
        CVPixelBufferPoolRef p = NULL;
        NSDictionary *a = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferWidthKey: @(swap ? h : w), (id)kCVPixelBufferHeightKey: @(swap ? w : h), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)a, &p); k.rotPool = p;
        LOG(@"vdo %@ rotating %d deg cw mirrored=%d -> %zux%zu then %dx%d", k.sinkID, r, k.mirrored, swap ? h : w, swap ? w : h, k.w, k.h);
    }
    CVPixelBufferRef dst = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, k.rotPool, &dst);
    OSStatus e = VTPixelRotationSessionRotateImage(k.rotSess, src, dst);
    if (e) { static int once; if (!once++) LOG(@"VTPixelRotation -> %d", (int)e); }
    return dst;
}
static CVPixelBufferRef M5Convert(M5Sink *k, CVPixelBufferRef src0) {
    CVPixelBufferRef src = [k.kind isEqualToString:@"vdo"] ? M7Rotate(k, src0) : CVPixelBufferRetain(src0);
    if (k.fmt == kCVPixelFormatType_32BGRA && k.w == (int)CVPixelBufferGetWidth(src) && k.h == (int)CVPixelBufferGetHeight(src)) return src;
    CVPixelBufferRef out = NULL;
    { CVPixelBufferRef r = M5ConvertScale(k, src); out = r; }
    CVPixelBufferRelease(src);
    return out;
}
static CVPixelBufferRef M5ConvertScale(M5Sink *k, CVPixelBufferRef src) {
    if (!k.pool) {
        CVPixelBufferPoolRef p = NULL;
        NSDictionary *a = @{(id)kCVPixelBufferPixelFormatTypeKey: @(k.fmt), (id)kCVPixelBufferWidthKey: @(k.w), (id)kCVPixelBufferHeightKey: @(k.h), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)a, &p); k.pool = p;
        VTPixelTransferSessionRef x = NULL; VTPixelTransferSessionCreate(NULL, &x); k.xfer = x;
    }
    CVPixelBufferRef dst = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, k.pool, &dst);
    OSStatus e = VTPixelTransferSessionTransferImage(k.xfer, src, dst);
    if (e) { static int once; if (!once++) LOG(@"VTPixelTransfer -> %d", (int)e); }
    return dst;
}
static CMSampleBufferRef M5SBuf(CVPixelBufferRef pb, CMTime pts) {
    CMVideoFormatDescriptionRef fd = NULL; CMVideoFormatDescriptionCreateForImageBuffer(NULL, pb, &fd);
    CMSampleTimingInfo ti = { CMTimeMake(1, (int32_t)(gPumpFps > 0 ? gPumpFps : 30)), pts, kCMTimeInvalid };
    CMSampleBufferRef sb = NULL; CMSampleBufferCreateReadyWithImageBuffer(NULL, pb, fd, &ti, &sb);
    CFRelease(fd); return sb;
}

// ---- preview ----------------------------------------------------------------
static CFTypeRef (*pCAImageQueueCreate)(uint32_t, uint32_t, uint32_t);
static uint64_t (*pCAImageQueueRegisterIOSurfaceBuffer)(CFTypeRef, IOSurfaceRef, uint32_t);
static bool (*pCAImageQueueInsertImage)(CFTypeRef, double, uint32_t, uint64_t, uint32_t, void *, void *);
static void M5PreviewPush(CMBaseObjectRef s, M5Sink *k, CVPixelBufferRef pb) {
    IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
    BOOL surfaceMode = getenv("M5_PREVIEW_IQ") == NULL || !pCAImageQueueCreate; // CAImageQueue path: args unresolved, shows nothing
    if (surfaceMode) {   // post the IOSurface itself as "ImageQueue" (CALayer.contents accepts IOSurface)
        gQuietPost = YES;
        NSDictionary *p = @{@"SectionID": k.sinkID, @"ImageQueue": (__bridge id)surf, @"ImageQueueWidth": @(k.w), @"ImageQueueHeight": @(k.h), @"RotationDegrees": @(k.rotation)};
        pCMNotificationCenterPostNotification(pCMNotificationCenterGetDefaultLocalCenter(), CFSTR("ImageQueueUpdated"), s, (__bridge CFDictionaryRef)p, 0);
        if (k.frames == 0) { static int firstEver; if (!firstEver++) LOG(@"TIMING first preview frame %.1f ms after process start", (CFAbsoluteTimeGetCurrent() - M6ProcStart()) * 1000);
            LOG(@"preview(surface) first ImageQueueUpdated %@", k.sinkID); M5SessPost(s, @"DidStartPreviewing", @{@"SectionID": k.sinkID}); M5SessPost(s, @"DidDisplayFirstPreviewFrame", @{@"SectionID": k.sinkID}); }
        k.frames++;
        return;
    }
    if (!k.imageQueue) {
        k.imageQueue = pCAImageQueueCreate(k.w, k.h, 8);
        k.regIDs = [NSMutableDictionary dictionary];
        LOG(@"preview CAImageQueueCreate(%d,%d) -> %p", k.w, k.h, k.imageQueue);
        M5SessPost(s, @"ImageQueueUpdated", @{@"SectionID": k.sinkID, @"ImageQueue": (__bridge id)k.imageQueue, @"ImageQueueWidth": @(k.w), @"ImageQueueHeight": @(k.h), @"RotationDegrees": @(k.rotation), @"ImageQueueSlot": @0});
        M5SessPost(s, @"DidStartPreviewing", @{@"SectionID": k.sinkID});
    }
    NSNumber *sid = @(IOSurfaceGetID(surf));
    NSNumber *rid = k.regIDs[sid];
    if (!rid) { rid = @(pCAImageQueueRegisterIOSurfaceBuffer(k.imageQueue, surf, 0)); k.regIDs[sid] = rid; CFRetain(surf); /* keep registered surfaces alive */ }
    uint32_t a = getenv("M5_IQ_A") ? atoi(getenv("M5_IQ_A")) : 0, b = getenv("M5_IQ_B") ? atoi(getenv("M5_IQ_B")) : 0;
    bool ok = pCAImageQueueInsertImage(k.imageQueue, -1.0, a, rid.unsignedLongLongValue, b, NULL, NULL);
    if (k.frames < 3 || !ok) LOG(@"preview insert frame %d id=%@ -> %d", k.frames, rid, ok);
    if (k.frames == 1) M5SessPost(s, @"DidDisplayFirstPreviewFrame", @{@"SectionID": k.sinkID});
    k.frames++;
}

// ---- barcodes (static fixture: detect once with CIDetector) --------------------
static NSArray *gBarcodeDicts;
static void M5DetectBarcodes(CVPixelBufferRef pb) {
    if (gBarcodeDicts) return;
    CIDetector *det = [CIDetector detectorOfType:CIDetectorTypeQRCode context:nil options:@{CIDetectorAccuracy: CIDetectorAccuracyHigh}];
    NSArray *fs = [det featuresInImage:[CIImage imageWithCVPixelBuffer:pb]];
    NSMutableArray *out = [NSMutableArray array];
    for (CIQRCodeFeature *f in fs) {
        // AVF metadata rects are normalized, in the landscape sensor buffer (0..1, origin top-left)
        CGRect b = f.bounds; double W = kSrcW, H = kSrcH;
        CGRect n = CGRectMake(b.origin.x / W, 1 - (b.origin.y + b.size.height) / H, b.size.width / W, b.size.height / H);
        NSArray *corners = @[ (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(f.topLeft.x / W, 1 - f.topLeft.y / H)),
                              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(f.bottomLeft.x / W, 1 - f.bottomLeft.y / H)),
                              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(f.bottomRight.x / W, 1 - f.bottomRight.y / H)),
                              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(f.topRight.x / W, 1 - f.topRight.y / H)) ];
        // BarcodeDecodeData = Quagga MRCDescriptor attributes (AVF: stringValue = MRCDescriptorDecodePayload(MRCDescriptorCreateWithAttributes(it)))
        CIQRCodeDescriptor *qd = (CIQRCodeDescriptor *)f.symbolDescriptor;
        NSMutableDictionary *mrc = [@{@"BarcodeType": @"QR"} mutableCopy];   // MRCDescriptorAttributeSymbology = MRCSymbologyQR
        if (qd) { mrc[@"BarcodeRawData"] = qd.errorCorrectedPayload; mrc[@"SymbolVersion"] = @(qd.symbolVersion); mrc[@"QRMASK"] = @(qd.maskPattern);
                  // MRC wants its own ECL enum (CI's ASCII 'L'/'M'/'Q'/'H' makes MRCDescriptorDecodePayload fail -3001)
                  long ecl = qd.errorCorrectionLevel; mrc[@"ErrorCorrectionLevel"] = @(ecl == 'L' ? 0 : ecl == 'M' ? 1 : ecl == 'Q' ? 2 : 3); }
        NSMutableDictionary *top = [mrc mutableCopy];
        if (qd) top[@"ErrorCorrectionLevel"] = @(qd.errorCorrectionLevel); // top level -> CIQRCodeDescriptor (CI enum)
        [top addEntriesFromDictionary:@{@"BarcodeDecodeData": mrc, @"BarcodeRect": (__bridge_transfer id)CGRectCreateDictionaryRepresentation(n), @"BarcodeCorners": corners}];
        [out addObject:top];
    }
    gBarcodeDicts = out;
    LOG(@"barcode detect: %lu codes %@", (unsigned long)out.count, out.count ? out[0][@"BarcodeDecodeData"] : @"");
}

