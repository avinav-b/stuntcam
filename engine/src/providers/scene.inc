// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): scene v2, virtual device orientation, switchable sources.
// ============================================================================
// Scene v2 (orientation watermark + QR + EAN-13), virtual device orientation, switchable sources (scene / still image / video file).
// Included from simcam.m right after the frame-source globals. Off switches: M12_NO_SCENE2 (the plain scene: fixture image + red bar),
// M12_NO_ORIENT (buffer content fixed to the portrait-device layout, coordinator/gravity follow the UI orientation).
// ============================================================================
static double M5AngleFor(long iface, long position);   // defined with the rotation coordinator hooks
static _Atomic long gDevOrientForced;    // 0 = follow the simulator (UIDevice orientation, else UI), 1..4 = UIDeviceOrientation set by env/control file
static _Atomic long gDevOrientSeen = 1;  // last valid UIDevice orientation / UI orientation, refreshed on the main thread
static _Atomic long gUIOrientA = 1;      // atomic mirror of gUIOrient (read from the pump queue)
// the UI orientation for readers off the main thread (coordinator getters, motion ticks): gUIOrient itself is main-only
static long M5UIOrientAny(void) { return atomic_load_explicit(&gUIOrientA, memory_order_relaxed); }
static long M12DevOrient(void) { long f = atomic_load(&gDevOrientForced); return f ? f : atomic_load(&gDevOrientSeen); }
static BOOL M12OrientOn(void) { return getenv("M12_NO_ORIENT") == NULL; }
static BOOL M12Scene2On(void) { return getenv("M12_NO_SCENE2") == NULL; }

// ---- source spec: "scene" | "image:<path>" | "video:<path>" | "webcam[:<uniqueID>]" (env SIMCAM_SOURCE, control file `source=`) ----
static NSString *gSrcSpecPending;        // guarded by @synchronized(gSrcSpecLock)
static NSObject *gSrcSpecLock;
static _Atomic long gSrcGen;             // bumped on every accepted change; the pump re-applies when it differs from its own copy
static _Atomic long gScanGen;            // bumped when the picture changes enough to re-run barcode detection
// pump-queue-only state
static long gSrcGenApplied = -1; static int gSrcKind; static NSString *gSrcPath;   // kind: 0 scene, 1 image, 2 video, 3 webcam
static CGImageRef gSrcStill;             // image source
static AVAssetReader *gVidReader; static AVAssetReaderOutput *gVidOut; static CGImageRef gVidFrame; static double gVidT0, gVidNextPTS, gVidFirstPTS; static long gVidFrames;
static CGImageRef gQRImg; static long gLastOrientForScan = -1;

static void M12SetSource(NSString *spec, const char *why) {
    if (!gSrcSpecLock) return;
    @synchronized (gSrcSpecLock) { if ([gSrcSpecPending isEqualToString:spec]) return; gSrcSpecPending = [spec copy]; }
    atomic_fetch_add(&gSrcGen, 1);
    LOG(@"m12 source -> %@ (%s)", spec, why);
}
static CGImageRef M12LoadImage(NSString *path) {
    CGImageSourceRef is = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL); if (!is) return NULL;
    // apply EXIF orientation so the still is upright as a viewer shows it
    NSDictionary *o = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES, (id)kCGImageSourceCreateThumbnailWithTransform: @YES, (id)kCGImageSourceThumbnailMaxPixelSize: @4096};
    CGImageRef img = CGImageSourceCreateThumbnailAtIndex(is, 0, (__bridge CFDictionaryRef)o);
    CFRelease(is); return img;
}
static void M12VideoClose(void) { [gVidReader cancelReading]; gVidReader = nil; gVidOut = nil; if (gVidFrame) CGImageRelease(gVidFrame); gVidFrame = NULL; }
static BOOL M12VideoOpen(void) {
    M12VideoClose();
    AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:gSrcPath] options:nil];
    NSArray *tracks = [a tracksWithMediaType:AVMediaTypeVideo];
    if (!tracks.count) { LOG(@"m12 video source: no video track in %@", gSrcPath); return NO; }
    NSError *e = nil; AVAssetReader *r = [AVAssetReader assetReaderWithAsset:a error:&e];
    if (!r) { LOG(@"m12 video source: reader failed %@", e); return NO; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    AVVideoComposition *vc = [AVVideoComposition videoCompositionWithPropertiesOfAsset:a];   // applies the track's preferredTransform (upright frames)
#pragma clang diagnostic pop
    AVAssetReaderVideoCompositionOutput *o = [AVAssetReaderVideoCompositionOutput assetReaderVideoCompositionOutputWithVideoTracks:tracks videoSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
    o.videoComposition = vc; o.alwaysCopiesSampleData = NO;
    if (![r canAddOutput:o]) { LOG(@"m12 video source: cannot add output"); return NO; }
    [r addOutput:o];
    if (![r startReading]) { LOG(@"m12 video source: startReading failed %@", r.error); return NO; }
    gVidReader = r; gVidOut = o; gVidT0 = CFAbsoluteTimeGetCurrent(); gVidNextPTS = 0; gVidFirstPTS = -1;
    return YES;
}
// advance the decoded video to "now" (decodes in-process; loops at the end)
static void M12VideoAdvance(void) {
    if (!gVidReader && !M12VideoOpen()) return;
    double now = CFAbsoluteTimeGetCurrent() - gVidT0;
    int guard = 0;
    while (gVidNextPTS <= now && guard++ < 8) {
        CMSampleBufferRef sb = [gVidOut copyNextSampleBuffer];
        if (!sb) {   // end (or error): loop
            if (gVidReader.status == AVAssetReaderStatusFailed) { LOG(@"m12 video source failed %@", gVidReader.error); M12VideoClose(); return; }
            CGImageRef keep = gVidFrame ? CGImageRetain(gVidFrame) : NULL;
            if (!M12VideoOpen()) { if (keep) CGImageRelease(keep); return; }
            gVidFrame = keep; now = 0; continue;
        }
        double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
        if (gVidFirstPTS < 0) gVidFirstPTS = pts;
        gVidNextPTS = pts - gVidFirstPTS;
        CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
        if (ib) { CGImageRef img = NULL; VTCreateCGImageFromCVPixelBuffer(ib, NULL, &img); if (img) { if (gVidFrame) CGImageRelease(gVidFrame); gVidFrame = img; gVidFrames++; } }
        CFRelease(sb);
    }
    if (gVidFrames % 15 == 1) atomic_fetch_add(&gScanGen, 1);   // moving picture: rescan now and then
}
static void M12ApplySource(void) {   // pump queue
    long g = atomic_load(&gSrcGen); if (g == gSrcGenApplied) return; gSrcGenApplied = g;
    NSString *spec; @synchronized (gSrcSpecLock) { spec = gSrcSpecPending ?: @"scene"; }
    if (gSrcStill) { CGImageRelease(gSrcStill); gSrcStill = NULL; } M12VideoClose(); if (gSrcKind == 3) M13WcLeave(); gSrcKind = 0; gSrcPath = nil;
    if ([spec hasPrefix:@"image:"]) { gSrcPath = [spec substringFromIndex:6]; gSrcStill = M12LoadImage(gSrcPath); gSrcKind = gSrcStill ? 1 : 0;
        LOG(@"m12 source applied: image %@ -> %@", gSrcPath, gSrcStill ? [NSString stringWithFormat:@"%zux%zu", CGImageGetWidth(gSrcStill), CGImageGetHeight(gSrcStill)] : @"LOAD FAILED (falling back to scene)"); }
    else if ([spec hasPrefix:@"video:"]) { gSrcPath = [spec substringFromIndex:6]; gSrcKind = 2; BOOL ok = M12VideoOpen(); if (!ok) gSrcKind = 0;
        LOG(@"m12 source applied: video %@ -> %@", gSrcPath, ok ? @"decoding" : @"OPEN FAILED (falling back to scene)"); }
    else if ([spec isEqualToString:@"webcam"] || [spec hasPrefix:@"webcam:"]) { gSrcKind = 3; M13WcApply(spec);
        LOG(@"m12 source applied: webcam %@", spec.length > 7 ? [spec substringFromIndex:7] : @"(default camera)"); }
    else LOG(@"m12 source applied: scene");
    atomic_fetch_add(&gScanGen, 1);
}

// ---- scene drawing (canvas coordinates: origin top-left, y down) ----
static void M12DrawImg(CGContextRef c, CGImageRef img, CGRect r, BOOL crisp) {
    CGContextSaveGState(c); CGContextTranslateCTM(c, r.origin.x, r.origin.y + r.size.height); CGContextScaleCTM(c, 1, -1);
    if (crisp) CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    CGContextDrawImage(c, CGRectMake(0, 0, r.size.width, r.size.height), img); CGContextRestoreGState(c);
}
static void M12DrawFill(CGContextRef c, CGImageRef img, double cw, double ch) {   // aspect-fill
    double iw = CGImageGetWidth(img), ih = CGImageGetHeight(img), s = MAX(cw / iw, ch / ih);
    M12DrawImg(c, img, CGRectMake((cw - iw * s) / 2, (ch - ih * s) / 2, iw * s, ih * s), NO);
}
// The QR is built off the pump queue, started at activation (M12Enqueue). CoreImage's first render in an app compiles Metal
// functions into that app's own cache (Library/Caches/<bundle>/com.apple.metal); with that cache empty (fresh install, new device)
// it took 0.2-0.8 s, more on a busy or just-booted sim, and it ran inside the first pump tick, so it delayed the first frame. Until
// the QR is ready the scene draws its white quiet-zone square without it; barcode detection reruns when it arrives.
static _Atomic(CGImageRef) gQRReady;
static void M12QRWarm(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{ dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        double t0 = CFAbsoluteTimeGetCurrent(); const char *t = getenv("SIMCAM_QR_TEXT");
        CIFilter *f = [CIFilter filterWithName:@"CIQRCodeGenerator"];
        [f setValue:[@(t ?: "https://simcam.dev/m12-qr") dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"]; [f setValue:@"M" forKey:@"inputCorrectionLevel"];
        CIImage *ci = [f.outputImage imageByApplyingTransform:CGAffineTransformMakeScale(10, 10)];
        atomic_store(&gQRReady, [[CIContext contextWithOptions:nil] createCGImage:ci fromRect:ci.extent]);
        LOG(@"TIMING scene QR ready in %.1f ms (%.1f ms after process start)", (CFAbsoluteTimeGetCurrent() - t0) * 1000, (CFAbsoluteTimeGetCurrent() - M6ProcStart()) * 1000);
    } }); });
}
static CGImageRef M12QR(void) {   // pump queue; NULL until ready
    // The rescan is requested here, on the pump queue, when the scene first draws the QR: bumped from the background block it
    // could land between this tick's render (no QR yet) and its barcode scan, which would then cache a scan without the QR.
    if (!gQRImg) { M12QRWarm(); gQRImg = atomic_load(&gQRReady); if (gQRImg) atomic_fetch_add(&gScanGen, 1); }
    return gQRImg;
}
static const char *kEAN = "5901234123457";
static void M12DrawEAN13(CGContextRef c, CGRect r) {
    static const char *L[] = {"0001101","0011001","0010011","0111101","0100011","0110001","0101111","0111011","0110111","0001011"};
    static const char *G[] = {"0100111","0110011","0011011","0100001","0011101","0111001","0000101","0010001","0001001","0010111"};
    static const char *R[] = {"1110010","1100110","1101100","1000010","1011100","1001110","1010000","1000100","1001000","1110100"};
    static const char *P[] = {"LLLLLL","LLGLGG","LLGGLG","LLGGGL","LGLLGG","LGGLLG","LGGGLL","LGLGLG","LGLGGL","LGGLGL"};
    const char *d = getenv("SIMCAM_EAN13") ?: kEAN; if (strlen(d) != 13) d = kEAN;
    char bits[96] = {0}; strcat(bits, "101");
    for (int i = 1; i <= 6; i++) strcat(bits, (P[d[0] - '0'][i - 1] == 'L' ? L : G)[d[i] - '0']);
    strcat(bits, "01010");
    for (int i = 7; i <= 12; i++) strcat(bits, R[d[i] - '0']);
    strcat(bits, "101");
    double mw = floor(r.size.width / (95 + 22)); if (mw < 1) mw = 1;   // integer module width, 11-module quiet zones
    double W = mw * (95 + 22), x0 = r.origin.x + (r.size.width - W) / 2;
    CGContextSetRGBFillColor(c, 1, 1, 1, 1); CGContextFillRect(c, CGRectMake(x0, r.origin.y, W, r.size.height));
    CGContextSetRGBFillColor(c, 0, 0, 0, 1);
    for (int i = 0; i < 95; i++) if (bits[i] == '1') CGContextFillRect(c, CGRectMake(x0 + (11 + i) * mw, r.origin.y + r.size.height * 0.08, mw, r.size.height * 0.84));
}
static void M12DrawScene(CGContextRef c, double cw, double ch) {
    double m = MIN(cw, ch); BOOL land = cw > ch;
    CGContextSetRGBFillColor(c, 0.15, 0.3, 0.45, 1); CGContextFillRect(c, CGRectMake(0, 0, cw, ch));
    // top colour bar, left->right red | green | blue (kept inside the central 70% so aspect-fill previews don't crop it)
    double bx = cw * 0.15, bw = cw * 0.70, by = ch * 0.12, bh = MAX(30, m * 0.05);
    CGContextSetRGBFillColor(c, 1, 0, 0, 1); CGContextFillRect(c, CGRectMake(bx, by, bw / 3, bh));
    CGContextSetRGBFillColor(c, 0, 1, 0, 1); CGContextFillRect(c, CGRectMake(bx + bw / 3, by, bw / 3, bh));
    CGContextSetRGBFillColor(c, 0, 0, 1, 1); CGContextFillRect(c, CGRectMake(bx + 2 * bw / 3, by, bw / 3, bh));
    // "F" in the top-left (asymmetric under every rotation and mirror)
    double fx = cw * 0.17, fy = by + bh + m * 0.04, fh = m * 0.16, fs = fh * 0.18;
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextFillRect(c, CGRectMake(fx, fy, fs, fh)); CGContextFillRect(c, CGRectMake(fx, fy, fh * 0.7, fs)); CGContextFillRect(c, CGRectMake(fx, fy + fh * 0.42, fh * 0.5, fs));
    // QR + EAN-13
    double q = m * (land ? 0.42 : 0.46);
    CGRect qr = land ? CGRectMake(cw * 0.36 - q / 2, ch * 0.58 - q / 2, q, q) : CGRectMake(cw / 2 - q / 2, ch * 0.44 - q / 2, q, q);
    CGContextSetRGBFillColor(c, 1, 1, 1, 1); CGContextFillRect(c, CGRectInset(qr, -q * 0.06, -q * 0.06));
    CGImageRef qi = M12QR(); if (qi) M12DrawImg(c, qi, qr, YES);
    CGRect ean = land ? CGRectMake(cw * 0.56, ch * 0.50, cw * 0.32, m * 0.20) : CGRectMake(cw * 0.18, ch * 0.66, cw * 0.64, m * 0.20);
    M12DrawEAN13(c, ean);
}
static CVPixelBufferRef M12Render(M5Sess *S) {
    if (!gSrcPool) {
        NSDictionary *a = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferWidthKey: @(kSrcW), (id)kCVPixelBufferHeightKey: @(kSrcH), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)a, &gSrcPool);
    }
    M12ApplySource();
    if (gSrcKind != 3) M13WcRestorePump(S);   // no-op unless the webcam source had locked this pump
    if (gSrcKind == 2) M12VideoAdvance();
    CGImageRef wcImg = NULL; NSString *wcBand = nil; CVPixelBufferRef wcPB = NULL;
    if (gSrcKind == 3) { BOOL rescan = NO; wcImg = M13WcAdvance(S, &wcBand, &rescan, &wcPB); if (rescan) atomic_fetch_add(&gScanGen, 1); }
    long pos = S && S.position ? S.position : 1;
    long dev = M12OrientOn() ? M12DevOrient() : 1;
    double ang = M5AngleFor(dev, pos);
    if (dev != gLastOrientForScan) { gLastOrientForScan = dev; atomic_fetch_add(&gScanGen, 1); }
    if (wcPB) {   // webcam frame path: the canvas comes from the ring frame (webcam_path.inc); NULL: draw it below
        CVPixelBufferRef c = M13WcCanvas(S, wcPB, (int)ang);
        if (c) { gFrameNo++; return c; }
        wcImg = gWcFrame;
    }
    BOOL portraitCanvas = ((int)ang % 180) != 0;
    double cw = portraitCanvas ? kSrcH : kSrcW, ch = portraitCanvas ? kSrcW : kSrcH;
    CVPixelBufferRef pb = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, gSrcPool, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pb), kSrcW, kSrcH, 8, CVPixelBufferGetBytesPerRow(pb), cs, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1); CGContextFillRect(ctx, CGRectMake(0, 0, kSrcW, kSrcH));
    // content is upright for the camera's capture angle at the current (virtual) device orientation: the app's rotation (coordinator
    // capture angle, clockwise) brings it upright, exactly like a device's sensor buffer
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, kSrcW / 2.0, kSrcH / 2.0); CGContextRotateCTM(ctx, ang * M_PI / 180.0);
    CGContextTranslateCTM(ctx, -cw / 2, ch / 2); CGContextScaleCTM(ctx, 1, -1);   // canvas: origin top-left, y down
    if (gSrcKind == 1 && gSrcStill) M12DrawFill(ctx, gSrcStill, cw, ch);
    else if (gSrcKind == 2) { if (gVidFrame) M12DrawFill(ctx, gVidFrame, cw, ch); }
    else if (gSrcKind == 3 && wcImg) M12DrawFill(ctx, wcImg, cw, ch);
    else M12DrawScene(ctx, cw, ch);   // also the webcam source while offline, under its band
    if (wcBand) M13WcDrawBand(ctx, cw, ch, wcBand);
    CGContextRestoreGState(ctx);
    if (gSrcKind == 0) { double x = (gFrameNo * 12) % kSrcW; CGContextSetRGBFillColor(ctx, 1, 1, 0.2, 1); CGContextFillRect(ctx, CGRectMake(x, 0, 24, 40)); } // moving bar (sensor bottom edge)
    CGContextRelease(ctx); CGColorSpaceRelease(cs);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    gFrameNo++;
    return pb;
}

// ---- mirroring: flip so that (rotate by `angle`, then this flip) == (mirror after rotating) ; angle 0/180 -> horizontal, 90/270 -> vertical
static CVPixelBufferRef M12Flip(CVPixelBufferRef src, int angle, VTPixelRotationSessionRef *sess, int *sessAngle) {
    BOOL vert = ((angle % 180) + 180) % 180 == 90;
    if (!*sess || *sessAngle != (vert ? 1 : 2)) {
        if (*sess) { VTPixelRotationSessionInvalidate(*sess); CFRelease(*sess); }
        VTPixelRotationSessionCreate(NULL, sess); *sessAngle = vert ? 1 : 2;
        VTSessionSetProperty(*sess, vert ? kVTPixelRotationPropertyKey_FlipVerticalOrientation : kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
    }
    CVPixelBufferRef dst = NULL;
    CVPixelBufferCreate(NULL, CVPixelBufferGetWidth(src), CVPixelBufferGetHeight(src), CVPixelBufferGetPixelFormatType(src), (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &dst);
    if (VTPixelRotationSessionRotateImage(*sess, src, dst)) { CVPixelBufferRelease(dst); return CVPixelBufferRetain(src); }
    return dst;
}
static NSMutableDictionary *gPrevNotes;   // "<Class: 0xptr" -> @[angle, mirrored], written by the preview layer hook
static NSArray *M12PreviewNote(NSString *sinkID) {
    if (!gPrevNotes) return nil;
    @synchronized (gSrcSpecLock) { for (NSString *k in gPrevNotes) if ([sinkID hasPrefix:k]) return gPrevNotes[k]; }
    return nil;
}

// ---- barcodes from the rendered buffer (Vision), re-run when the picture changes: QR + EAN-13 (+ the other 1D/2D types Vision finds) ----
static NSString *(*pMRCDecode)(CFTypeRef);
static CFTypeRef (*pMRCCreate)(CFDictionaryRef);
static NSString *M12MRCString(NSDictionary *attrs) {
    if (!pMRCCreate) { pMRCCreate = dlsym(RTLD_DEFAULT, "MRCDescriptorCreateWithAttributes"); pMRCDecode = dlsym(RTLD_DEFAULT, "MRCDescriptorDecodePayload"); }
    if (!pMRCCreate || !pMRCDecode) return nil;
    CFTypeRef d = pMRCCreate((__bridge CFDictionaryRef)attrs); if (!d) return nil;
    NSString *s = pMRCDecode(d); CFRelease(d); return s;
}
static NSString *M12Sym(const char *name) { CFStringRef *p = dlsym(RTLD_DEFAULT, name); return p ? (__bridge NSString *)*p : nil; }
static NSDictionary *M12OneDAttrs(NSString *mrcSym, NSString *payload) {
    // Quagga's MRC wants 1D payload digits as byte values 0..9 (ASCII bytes decode as digit-2 mod 10, measured on 26.3). Verified once per
    // symbology by decoding it back; never pass an NSString as BarcodeRawData (hung the pump in the probe).
    NSMutableData *dv = [NSMutableData data];
    for (NSUInteger i = 0; i < payload.length; i++) { unichar c = [payload characterAtIndex:i]; uint8_t b = (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : (uint8_t)c; [dv appendBytes:&b length:1]; }
    NSDictionary *d = @{@"BarcodeType": mrcSym, @"BarcodeRawData": dv};
    static NSMutableSet *checked; if (!checked) checked = [NSMutableSet set];
    if (![checked containsObject:mrcSym]) { [checked addObject:mrcSym]; LOG(@"m12 MRC check %@ digits-as-values -> %@ (want %@)", mrcSym, M12MRCString(d), payload); }
    return d;
}
static long gScanGenDone = -1;
static NSString *M12VNSym(const char *n) { CFStringRef *p = dlsym(RTLD_DEFAULT, n); return p ? (__bridge NSString *)*p : @""; }
static CGPoint M12Pt(id o, NSString *k) { NSValue *v = M5KVC(o, k); CGPoint p = CGPointZero; if (v) [v getValue:&p size:sizeof p]; return p; }
static void M7VisionInstall(void);   // client/vision.inc
static void M12DetectBarcodes(CVPixelBufferRef pb) {
    long g = atomic_load(&gScanGen); if (g == gScanGenDone && gBarcodeDicts) return; gScanGenDone = g;
    static Class RQ, HD; static dispatch_once_t o1;
    dispatch_once(&o1, ^{ dlopen("/System/Library/Frameworks/Vision.framework/Vision", RTLD_NOW); RQ = objc_getClass("VNDetectBarcodesRequest"); HD = objc_getClass("VNImageRequestHandler");
        // On 17.5 the add_image callback in client/vision.inc fires before Vision's classes are registered, so when we are the
        // first to load Vision the revision pin would never be installed and detection would find 0 codes. Install it here too.
        if (RQ && !getenv("M7_NO_VISIONFIX")) M7VisionInstall(); });
    if (!RQ || !HD) { LOG(@"m12 barcode detect: Vision unavailable"); gBarcodeDicts = @[]; return; }
    id rq = [[RQ alloc] init];
    NSError *e = nil; id h = ((id (*)(id, SEL, CVPixelBufferRef, id))objc_msgSend)([HD alloc], sel_registerName("initWithCVPixelBuffer:options:"), pb, @{});
    ((BOOL (*)(id, SEL, id, NSError **))objc_msgSend)(h, sel_registerName("performRequests:error:"), @[rq], &e);
    NSArray *res = M5KVC(rq, @"results");
    NSString *sQR = M12VNSym("VNBarcodeSymbologyQR"), *sE13 = M12VNSym("VNBarcodeSymbologyEAN13"), *sE8 = M12VNSym("VNBarcodeSymbologyEAN8"), *sC128 = M12VNSym("VNBarcodeSymbologyCode128");
    NSMutableArray *out = [NSMutableArray array], *names = [NSMutableArray array];
    for (id o in res) {
        NSString *sym = M5KVC(o, @"symbology"), *payload = M5KVC(o, @"payloadStringValue"); id desc = M5KVC(o, @"barcodeDescriptor");
        [names addObject:[NSString stringWithFormat:@"%@=%@", sym, payload]];
        CGRect b = CGRectZero; [(NSValue *)M5KVC(o, @"boundingBox") getValue:&b size:sizeof b];   // normalized, origin bottom-left
        CGRect n = CGRectMake(b.origin.x, 1 - b.origin.y - b.size.height, b.size.width, b.size.height);
        CGPoint tl = M12Pt(o, @"topLeft"), bl = M12Pt(o, @"bottomLeft"), br = M12Pt(o, @"bottomRight"), tr = M12Pt(o, @"topRight");
        NSArray *corners = @[ (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(tl.x, 1 - tl.y)), (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(bl.x, 1 - bl.y)),
                              (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(br.x, 1 - br.y)), (__bridge_transfer id)CGPointCreateDictionaryRepresentation(CGPointMake(tr.x, 1 - tr.y)) ];
        NSMutableDictionary *mrc = nil, *top = nil;
        if ([sym isEqualToString:sQR] && [desc isKindOfClass:[CIQRCodeDescriptor class]]) {
            CIQRCodeDescriptor *qd = (CIQRCodeDescriptor *)desc; long ecl = qd.errorCorrectionLevel;
            mrc = [@{@"BarcodeType": @"QR", @"BarcodeRawData": qd.errorCorrectedPayload, @"SymbolVersion": @(qd.symbolVersion), @"QRMASK": @(qd.maskPattern), @"ErrorCorrectionLevel": @(ecl == 'L' ? 0 : ecl == 'M' ? 1 : ecl == 'Q' ? 2 : 3)} mutableCopy];
            top = [mrc mutableCopy]; top[@"ErrorCorrectionLevel"] = @(ecl);
        } else if (payload) {
            NSString *ms = [sym isEqualToString:sE13] ? M12Sym("MRCSymbologyEAN13") : [sym isEqualToString:sE8] ? M12Sym("MRCSymbologyEAN8") : [sym isEqualToString:sC128] ? M12Sym("MRCSymbologyCode128") : nil;
            if (!ms) continue;
            mrc = [M12OneDAttrs(ms, payload) mutableCopy]; top = [mrc mutableCopy];
        } else continue;
        [top addEntriesFromDictionary:@{@"BarcodeDecodeData": mrc, @"BarcodeRect": (__bridge_transfer id)CGRectCreateDictionaryRepresentation(n), @"BarcodeCorners": corners}];
        [out addObject:top];
    }
    gBarcodeDicts = out;
    static int emptyRuns; if (!out.count && ++emptyRuns <= 5) gScanGenDone = -1;   // the scene always has codes; retry before caching none
    LOG(@"m12 barcode detect (gen %ld): %lu codes %@ err=%@", g, (unsigned long)out.count, names, e);
}
