// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): scene v2, virtual device orientation, switchable
// sources.
// ============================================================================
// Scene v2 (orientation watermark + QR + EAN-13), virtual device orientation, switchable sources (scene / still image /
// video file).
// Off switches: SIMCAM_OFF_GENERATED_SCENE (the plain scene: fixture image + red bar),
// SIMCAM_OFF_VIRTUAL_ORIENTATION (buffer content fixed to the portrait-device layout, coordinator/gravity follow the UI
// orientation).
// ============================================================================
static double CoordAngleFor(long iface, long position);   // defined with the rotation coordinator hooks
// 0 = follow the simulator (UIDevice orientation, else UI), 1..4 = UIDeviceOrientation set by env/control file
static _Atomic long gDevOrientForced;
// last valid UIDevice orientation / UI orientation, refreshed on the main thread
static _Atomic long gDevOrientSeen = 1;
static _Atomic long gUIOrientA = 1;      // atomic mirror of gUIOrient (read from the pump queue)
// the UI orientation for readers off the main thread (coordinator getters, motion ticks): gUIOrient itself is main-only
static long OrientUIAny(void) { return atomic_load_explicit(&gUIOrientA, memory_order_relaxed); }
static long OrientDevice(void) {
    long forced = atomic_load(&gDevOrientForced);
    return forced ? forced : atomic_load(&gDevOrientSeen);
}
static BOOL OrientVirtualOn(void) { return getenv(ENV_OFF_VIRTUAL_ORIENTATION) == NULL; }
static BOOL SceneGeneratedOn(void) { return getenv(ENV_OFF_GENERATED_SCENE) == NULL; }

// ---- source spec: "scene" | "image:<path>" | "video:<path>" | "webcam[:<uniqueID>]" (env SIMCAM_SOURCE, control file
// `source=`) ----
// gSrcSpecLock (made by core/control.inc) guards gSrcSpecPending and the preview notes (gPrevNotes, mirror.inc).
static NSString *gSrcSpecPending;        // guarded by @synchronized(gSrcSpecLock)
static NSObject *gSrcSpecLock;
// bumped on every accepted change; the pump re-applies when it differs from its own copy
static _Atomic long gSrcGen;
static _Atomic long gScanGen;            // bumped when the picture changes enough to re-run barcode detection
// the values of gSrcKind: the source the pump draws
enum { kSourceScene = 0, kSourceImage = 1, kSourceVideo = 2, kSourceWebcam = 3 };
// pump-queue-only state
static long gSrcGenApplied = -1;
static int gSrcKind;                     // kSourceScene, kSourceImage, kSourceVideo or kSourceWebcam
static NSString *gSrcPath;
static CGImageRef gSrcStill;             // image source
static AVAssetReader *gVidReader;        // video source: the reader, its output, the newest frame and the timing
static AVAssetReaderOutput *gVidOut;
static CGImageRef gVidFrame;
static double gVidT0, gVidNextPTS, gVidFirstPTS;
static long gVidFrames;
static CGImageRef gQRImg;
static long gLastOrientForScan = -1;

static void SceneSetSource(NSString *spec, const char *why) {
    if (!gSrcSpecLock) return;
    @synchronized (gSrcSpecLock) {
        if ([gSrcSpecPending isEqualToString:spec]) return;
        gSrcSpecPending = [spec copy];
    }
    atomic_fetch_add(&gSrcGen, 1);
    LOG(@"source -> %@ (%s)", spec, why);
}
static CGImageRef SceneLoadImage(NSString *path) {
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!source) return NULL;
    // apply EXIF orientation so the still is upright as a viewer shows it
    NSDictionary *opts = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                           (id)kCGImageSourceCreateThumbnailWithTransform: @YES,
                           (id)kCGImageSourceThumbnailMaxPixelSize: @4096};
    CGImageRef img = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)opts);
    CFRelease(source);
    return img;
}
static void SceneVideoClose(void) {
    [gVidReader cancelReading];
    gVidReader = nil;
    gVidOut = nil;
    if (gVidFrame) CGImageRelease(gVidFrame);
    gVidFrame = NULL;
}
static BOOL SceneVideoOpen(void) {
    SceneVideoClose();
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:gSrcPath] options:nil];
    NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    if (!tracks.count) {
        LOG(@"video source: no video track in %@", gSrcPath);
        return NO;
    }
    NSError *e = nil;
    AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&e];
    if (!reader) {
        LOG(@"video source: reader failed %@", e);
        return NO;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // applies the track's preferredTransform (upright frames)
    AVVideoComposition *vc = [AVVideoComposition videoCompositionWithPropertiesOfAsset:asset];
#pragma clang diagnostic pop
    AVAssetReaderVideoCompositionOutput *output = [AVAssetReaderVideoCompositionOutput
        assetReaderVideoCompositionOutputWithVideoTracks:tracks
                                           videoSettings:@{
                                               (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
    output.videoComposition = vc;
    output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:output]) {
        LOG(@"video source: cannot add output");
        return NO;
    }
    [reader addOutput:output];
    if (![reader startReading]) {
        LOG(@"video source: startReading failed %@", reader.error);
        return NO;
    }
    gVidReader = reader;
    gVidOut = output;
    gVidT0 = CFAbsoluteTimeGetCurrent();
    gVidNextPTS = 0;
    gVidFirstPTS = -1;
    return YES;
}
// advance the decoded video to "now" (decodes in-process; loops at the end)
static void SceneVideoAdvance(void) {
    if (!gVidReader && !SceneVideoOpen()) return;
    double now = CFAbsoluteTimeGetCurrent() - gVidT0;
    int guard = 0;
    while (gVidNextPTS <= now && guard++ < 8) {
        CMSampleBufferRef sb = [gVidOut copyNextSampleBuffer];
        if (!sb) {   // end (or error): loop
            if (gVidReader.status == AVAssetReaderStatusFailed) {
                LOG(@"video source failed %@", gVidReader.error);
                SceneVideoClose();
                return;
            }
            CGImageRef keep = gVidFrame ? CGImageRetain(gVidFrame) : NULL;
            if (!SceneVideoOpen()) {
                if (keep) CGImageRelease(keep);
                return;
            }
            gVidFrame = keep;
            now = 0;
            continue;
        }
        double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
        if (gVidFirstPTS < 0) gVidFirstPTS = pts;
        gVidNextPTS = pts - gVidFirstPTS;
        CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
        if (ib) {
            CGImageRef img = NULL;
            VTCreateCGImageFromCVPixelBuffer(ib, NULL, &img);
            if (img) {
                if (gVidFrame) CGImageRelease(gVidFrame);
                gVidFrame = img;
                gVidFrames++;
            }
        }
        CFRelease(sb);
    }
    if (gVidFrames % 15 == 1) atomic_fetch_add(&gScanGen, 1);   // moving picture: rescan now and then
}
static void SceneApplySource(void) {   // pump queue
    long gen = atomic_load(&gSrcGen);
    if (gen == gSrcGenApplied) return;
    gSrcGenApplied = gen;
    NSString *spec;
    @synchronized (gSrcSpecLock) { spec = gSrcSpecPending ?: @"scene"; }
    if (gSrcStill) {
        CGImageRelease(gSrcStill);
        gSrcStill = NULL;
    }
    SceneVideoClose();
    if (gSrcKind == kSourceWebcam) WcLeave();
    gSrcKind = kSourceScene;
    gSrcPath = nil;
    if ([spec hasPrefix:@"image:"]) {
        gSrcPath = [spec substringFromIndex:6];
        gSrcStill = SceneLoadImage(gSrcPath);
        gSrcKind = gSrcStill ? kSourceImage : kSourceScene;
        LOG(@"source applied: image %@ -> %@", gSrcPath,
            gSrcStill ? [NSString stringWithFormat:@"%zux%zu", CGImageGetWidth(gSrcStill), CGImageGetHeight(gSrcStill)]
                      : @"LOAD FAILED (falling back to scene)");
    } else if ([spec hasPrefix:@"video:"]) {
        gSrcPath = [spec substringFromIndex:6];
        gSrcKind = kSourceVideo;
        BOOL ok = SceneVideoOpen();
        if (!ok) gSrcKind = kSourceScene;
        LOG(@"source applied: video %@ -> %@", gSrcPath, ok ? @"decoding" : @"OPEN FAILED (falling back to scene)");
    } else if ([spec isEqualToString:@"webcam"] || [spec hasPrefix:@"webcam:"]) {
        gSrcKind = kSourceWebcam;
        WcApply(spec);
        LOG(@"source applied: webcam %@", spec.length > 7 ? [spec substringFromIndex:7] : @"(default camera)");
    } else {
        LOG(@"source applied: scene");
    }
    atomic_fetch_add(&gScanGen, 1);
}

// ---- scene drawing (canvas coordinates: origin top-left, y down) ----
static void SceneDrawImage(CGContextRef c, CGImageRef img, CGRect r, BOOL crisp) {
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, r.origin.x, r.origin.y + r.size.height);
    CGContextScaleCTM(c, 1, -1);
    if (crisp) CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    CGContextDrawImage(c, CGRectMake(0, 0, r.size.width, r.size.height), img);
    CGContextRestoreGState(c);
}
static void SceneDrawFill(CGContextRef c, CGImageRef img, double cw, double ch) {   // aspect-fill
    double iw = CGImageGetWidth(img), ih = CGImageGetHeight(img), scale = MAX(cw / iw, ch / ih);
    SceneDrawImage(c, img, CGRectMake((cw - iw * scale) / 2, (ch - ih * scale) / 2, iw * scale, ih * scale), NO);
}
// The QR is built off the pump queue, started at activation (CtlEnqueue). CoreImage's first render in an app compiles
// Metal functions into that app's own cache (Library/Caches/<bundle>/com.apple.metal); with that cache empty (fresh
// install, new device) it takes 0.2-0.8 s, more on a busy or just-booted sim; inside the first pump tick it would delay
// the first frame. Until the QR is ready the scene draws its white quiet-zone square without it; barcode detection
// reruns when it arrives.
static _Atomic(CGImageRef) gQRReady;
static void SceneQRWarm(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                double t0 = CFAbsoluteTimeGetCurrent();
                const char *text = getenv(ENV_QR_TEXT);   // the QR's text (default: a fixed URL)
                CIFilter *f = [CIFilter filterWithName:@"CIQRCodeGenerator"];
                [f setValue:[@(text ?: "https://simcam.dev/m12-qr") dataUsingEncoding:NSUTF8StringEncoding]
                     forKey:@"inputMessage"];
                [f setValue:@"M" forKey:@"inputCorrectionLevel"];
                CIImage *ci = [f.outputImage imageByApplyingTransform:CGAffineTransformMakeScale(10, 10)];
                atomic_store(&gQRReady, [[CIContext contextWithOptions:nil] createCGImage:ci fromRect:ci.extent]);
                LOG(@"TIMING scene QR ready in %.1f ms (%.1f ms after process start)",
                    (CFAbsoluteTimeGetCurrent() - t0) * 1000,
                    (CFAbsoluteTimeGetCurrent() - ActProcessStartTime()) * 1000);
            }
        });
    });
}
static CGImageRef SceneQRImage(void) {   // pump queue; NULL until ready
    // The rescan is requested here, on the pump queue, when the scene first draws the QR: bumped from the background
    // block it could land between this tick's render (no QR yet) and its barcode scan, which would then cache a scan
    // without the QR.
    if (!gQRImg) {
        SceneQRWarm();
        gQRImg = atomic_load(&gQRReady);
        if (gQRImg) atomic_fetch_add(&gScanGen, 1);
    }
    return gQRImg;
}
static const char *kEAN = "5901234123457";
// EAN-13: 95 modules (start, 6 left digits, centre, 6 right digits, end) between 11-module quiet zones
static void SceneDrawEAN13(CGContextRef c, CGRect r) {
    static const char *L[] = {"0001101", "0011001", "0010011", "0111101", "0100011",
                              "0110001", "0101111", "0111011", "0110111", "0001011"};
    static const char *G[] = {"0100111", "0110011", "0011011", "0100001", "0011101",
                              "0111001", "0000101", "0010001", "0001001", "0010111"};
    static const char *R[] = {"1110010", "1100110", "1101100", "1000010", "1011100",
                              "1001110", "1010000", "1000100", "1001000", "1110100"};
    static const char *P[] = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
                              "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};
    const char *digits = getenv(ENV_EAN13) ?: kEAN;   // the barcode's 13 digits (anything else: the default)
    if (strlen(digits) != 13) digits = kEAN;
    char bits[96] = {0};
    strcat(bits, "101");
    for (int i = 1; i <= 6; i++) strcat(bits, (P[digits[0] - '0'][i - 1] == 'L' ? L : G)[digits[i] - '0']);
    strcat(bits, "01010");
    for (int i = 7; i <= 12; i++) strcat(bits, R[digits[i] - '0']);
    strcat(bits, "101");
    double mw = floor(r.size.width / (95 + 22));   // integer module width, 11-module quiet zones
    if (mw < 1) mw = 1;
    double W = mw * (95 + 22), x0 = r.origin.x + (r.size.width - W) / 2;
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextFillRect(c, CGRectMake(x0, r.origin.y, W, r.size.height));
    CGContextSetRGBFillColor(c, 0, 0, 0, 1);
    for (int i = 0; i < 95; i++)
        if (bits[i] == '1')
            CGContextFillRect(c, CGRectMake(x0 + (11 + i) * mw, r.origin.y + r.size.height * 0.08, mw,
                                            r.size.height * 0.84));
}
static void SceneDrawGenerated(CGContextRef c, double cw, double ch) {
    double m = MIN(cw, ch);
    BOOL land = cw > ch;
    CGContextSetRGBFillColor(c, 0.15, 0.3, 0.45, 1);
    CGContextFillRect(c, CGRectMake(0, 0, cw, ch));
    // top colour bar, left->right red | green | blue (kept inside the central 70% so aspect-fill previews don't crop
    // it)
    double bx = cw * 0.15, bw = cw * 0.70, by = ch * 0.12, bh = MAX(30, m * 0.05);
    CGContextSetRGBFillColor(c, 1, 0, 0, 1);
    CGContextFillRect(c, CGRectMake(bx, by, bw / 3, bh));
    CGContextSetRGBFillColor(c, 0, 1, 0, 1);
    CGContextFillRect(c, CGRectMake(bx + bw / 3, by, bw / 3, bh));
    CGContextSetRGBFillColor(c, 0, 0, 1, 1);
    CGContextFillRect(c, CGRectMake(bx + 2 * bw / 3, by, bw / 3, bh));
    // "F" in the top-left (asymmetric under every rotation and mirror)
    double fx = cw * 0.17, fy = by + bh + m * 0.04, fh = m * 0.16, fs = fh * 0.18;
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextFillRect(c, CGRectMake(fx, fy, fs, fh));
    CGContextFillRect(c, CGRectMake(fx, fy, fh * 0.7, fs));
    CGContextFillRect(c, CGRectMake(fx, fy + fh * 0.42, fh * 0.5, fs));
    // QR + EAN-13
    double q = m * (land ? 0.42 : 0.46);
    CGRect qr = land ? CGRectMake(cw * 0.36 - q / 2, ch * 0.58 - q / 2, q, q)
                     : CGRectMake(cw / 2 - q / 2, ch * 0.44 - q / 2, q, q);
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextFillRect(c, CGRectInset(qr, -q * 0.06, -q * 0.06));
    CGImageRef qrImage = SceneQRImage();
    if (qrImage) SceneDrawImage(c, qrImage, qr, YES);
    CGRect ean = land ? CGRectMake(cw * 0.56, ch * 0.50, cw * 0.32, m * 0.20)
                      : CGRectMake(cw * 0.18, ch * 0.66, cw * 0.64, m * 0.20);
    SceneDrawEAN13(c, ean);
}
static CVPixelBufferRef SceneRenderSource(SimcamSession *S) {
    if (!gSrcPool) {
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id)kCVPixelBufferWidthKey: @(kSrcW), (id)kCVPixelBufferHeightKey: @(kSrcH),
                                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &gSrcPool);
    }
    SceneApplySource();
    if (gSrcKind != kSourceWebcam) WcRestorePump(S);   // no-op unless the webcam source had locked this pump
    if (gSrcKind == kSourceVideo) SceneVideoAdvance();
    CGImageRef wcImg = NULL;
    NSString *wcBand = nil;
    CVPixelBufferRef wcPB = NULL;
    if (gSrcKind == kSourceWebcam) {
        BOOL rescan = NO;
        wcImg = WcAdvance(S, &wcBand, &rescan, &wcPB);
        if (rescan) atomic_fetch_add(&gScanGen, 1);
    }
    long pos = S && S.position ? S.position : 1;
    long dev = OrientVirtualOn() ? OrientDevice() : 1;
    double ang = CoordAngleFor(dev, pos);
    if (dev != gLastOrientForScan) {
        gLastOrientForScan = dev;
        atomic_fetch_add(&gScanGen, 1);
    }
    if (wcPB) {   // webcam frame path: the canvas comes from the ring frame (webcam_path.inc); NULL: draw it below
        CVPixelBufferRef canvas = WcCanvas(S, wcPB, (int)ang);
        if (canvas) {
            gFrameNo++;
            return canvas;
        }
        wcImg = gWcFrame;
    }
    BOOL portraitCanvas = ((int)ang % 180) != 0;
    double cw = portraitCanvas ? kSrcH : kSrcW, ch = portraitCanvas ? kSrcW : kSrcH;
    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, gSrcPool, &pb);
    if (!pb) return NULL;   // no buffer from the pool: no frame this tick
    CVPixelBufferLockBaseAddress(pb, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pb), kSrcW, kSrcH, 8,
                                             CVPixelBufferGetBytesPerRow(pb), cs,
                                             kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, kSrcW, kSrcH));
    // content is upright for the camera's capture angle at the current (virtual) device orientation: the app's rotation
    // (coordinator capture angle, clockwise) brings it upright, exactly like a device's sensor buffer
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, kSrcW / 2.0, kSrcH / 2.0);
    CGContextRotateCTM(ctx, ang * M_PI / 180.0);
    CGContextTranslateCTM(ctx, -cw / 2, ch / 2);   // canvas: origin top-left, y down
    CGContextScaleCTM(ctx, 1, -1);
    if (gSrcKind == kSourceImage && gSrcStill) {
        SceneDrawFill(ctx, gSrcStill, cw, ch);
    } else if (gSrcKind == kSourceVideo) {
        if (gVidFrame) SceneDrawFill(ctx, gVidFrame, cw, ch);
    } else if (gSrcKind == kSourceWebcam && wcImg) {
        SceneDrawFill(ctx, wcImg, cw, ch);
    } else {
        SceneDrawGenerated(ctx, cw, ch);   // also the webcam source while offline, under its band
    }
    if (wcBand) WcDrawBand(ctx, cw, ch, wcBand);
    CGContextRestoreGState(ctx);
    if (gSrcKind == kSourceScene) {   // moving bar (sensor bottom edge)
        double x = (gFrameNo * 12) % kSrcW;
        CGContextSetRGBFillColor(ctx, 1, 1, 0.2, 1);
        CGContextFillRect(ctx, CGRectMake(x, 0, 24, 40));
    }
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    gFrameNo++;
    return pb;
}
