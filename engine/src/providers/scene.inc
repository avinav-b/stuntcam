// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): scene v2, virtual device orientation, switchable
// sources.
//
// Provides: SceneRenderSource (providers/pump.inc, providers/photo.inc)
//           SceneSetSource, SceneQRWarm (core/control.inc)
//           OrientDevice, OrientUIAny (client/motion.inc, client/rotation.inc)
// Owns:     gScene and gOrientation, defined in providers/scene.h with which queue owns what
//           gScene.request is made and written by core/control.inc; providers/barcodes.inc reads applied and scan
//           gOrientation: written by client/motion.inc and core/control.inc
// Uses:     core/init.inc, fig/session_state.inc, providers/video.inc, providers/webcam.inc,
//           providers/webcam_path.inc, providers/qr.c, client/rotation.inc
//
// ============================================================================
// The generated scene (an orientation watermark, a QR code and an EAN-13 barcode), the virtual device orientation,
// and the switchable sources: the scene, a still image, a video file or a generated QR code.
// Off switches:
//   SIMCAM_OFF_GENERATED_SCENE: the plain scene instead, a fixture image and a red bar.
//   SIMCAM_OFF_VIRTUAL_ORIENTATION: the buffer content stays in the portrait-device layout, and the rotation
//   coordinator and gravity follow the UI orientation.
// ============================================================================
#include "scene.h"
#include "../fig/session.h"
#include "pump.h"
#include "webcam.h"
#include "../client/rotation.h"
#include "../core/init.h"

// the UI orientation for readers off the main thread (coordinator getters, motion ticks): gOrientTracker.uiOrientation
// itself is main-only
static long OrientUIAny(void) { return atomic_load_explicit(&gOrientation.ui, memory_order_relaxed); }

static long OrientDevice(void) {
    long forced = atomic_load(&gOrientation.deviceForced);
    return forced ? forced : atomic_load(&gOrientation.deviceSeen);
}

// ---- source spec: "scene" | "image:<path>" | "video:<path>" | "webcam[:<uniqueID>]" | "qr:<text, %XX-encoded>" (env
// SIMCAM_SOURCE, control file `source=`; the qr: text encoding is in providers/qr.c) ----

// the spec prefixes of the image, video and qr sources (the webcam's is kWcSpecPrefixLen)
#define kSceneImagePrefixLen 6           // strlen("image:")
#define kSceneVideoPrefixLen 6           // strlen("video:")
#define kSceneQRPrefixLen 3              // strlen("qr:")
// the video source decodes at most this many frames a tick to catch up, and re-runs barcode detection on every
// kSceneVideoRescanEveryFrames-th frame
#define kSceneVideoMaxFramesPerTick 8
#define kSceneVideoRescanEveryFrames 5
#define kSceneQRModulePixels 10          // the scene's QR code is drawn at 10 pixels per module
// the moving bar on the sensor's bottom edge: its step per frame and its size, in pixels
#define kSceneBarStepPx 12
#define kSceneBarWidthPx 24
#define kSceneBarHeightPx 40

static void SceneSetSource(NSString *spec, const char *why) {
    if (!gScene.request.lock) {
        return;
    }
    @synchronized (gScene.request.lock) {
        if ([gScene.request.spec isEqualToString:spec]) {
            return;
        }
        gScene.request.spec = [spec copy];
    }
    atomic_fetch_add(&gScene.request.gen, 1);
    LOG(@"source -> %@ (%s)", spec, why);
}

static CGImageRef SceneLoadImage(NSString *path) {
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!source) {
        return NULL;
    }
    // apply EXIF orientation so the still is upright as a viewer shows it
    NSDictionary *options = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                           (id)kCGImageSourceCreateThumbnailWithTransform: @YES,
                           (id)kCGImageSourceThumbnailMaxPixelSize: @4096};
    CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
    CFRelease(source);
    return image;
}

static void SceneVideoClose(void) {
    [gScene.video.reader cancelReading];
    gScene.video.reader = nil;
    gScene.video.output = nil;
    if (gScene.video.frame) {
        CGImageRelease(gScene.video.frame);
    }
    gScene.video.frame = NULL;
}

static BOOL SceneVideoOpen(void) {
    SceneVideoClose();
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:gScene.applied.path] options:nil];
    NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];

    if (!tracks.count) {
        LOG(@"video source: no video track in %@", gScene.applied.path);
        return NO;
    }

    NSError *error = nil;
    AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&error];

    if (!reader) {
        LOG(@"video source: reader failed %@", error);
        return NO;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

    // applies the track's preferredTransform (upright frames)
    AVVideoComposition *composition = [AVVideoComposition videoCompositionWithPropertiesOfAsset:asset];
#pragma clang diagnostic pop
    AVAssetReaderVideoCompositionOutput *output = [AVAssetReaderVideoCompositionOutput
        assetReaderVideoCompositionOutputWithVideoTracks:tracks
                                           videoSettings:@{
                                               (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
    output.videoComposition = composition;
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

    gScene.video.reader = reader;
    gScene.video.output = output;
    gScene.video.startTime = CFAbsoluteTimeGetCurrent();
    gScene.video.nextPTS = 0;
    gScene.video.firstPTS = -1;
    return YES;
}

// advance the decoded video to "now" (decodes in-process; loops at the end)
static void SceneVideoAdvance(void) {
    if (!gScene.video.reader && !SceneVideoOpen()) {
        return;
    }
    double now = CFAbsoluteTimeGetCurrent() - gScene.video.startTime;
    int loopPasses = 0;

    while (gScene.video.nextPTS <= now && loopPasses++ < kSceneVideoMaxFramesPerTick) {
        CMSampleBufferRef sampleBuffer = [gScene.video.output copyNextSampleBuffer];
        if (!sampleBuffer) {   // end (or error): loop
            if (gScene.video.reader.status == AVAssetReaderStatusFailed) {
                LOG(@"video source failed %@", gScene.video.reader.error);
                SceneVideoClose();
                return;
            }
            CGImageRef kept = gScene.video.frame ? CGImageRetain(gScene.video.frame) : NULL;
            if (!SceneVideoOpen()) {
                if (kept) {
                    CGImageRelease(kept);
                }
                return;
            }
            gScene.video.frame = kept;
            now = 0;
            continue;
        }
        double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
        if (gScene.video.firstPTS < 0) {
            gScene.video.firstPTS = pts;
        }
        gScene.video.nextPTS = pts - gScene.video.firstPTS;
        CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (imageBuffer) {
            CGImageRef image = NULL;
            VTCreateCGImageFromCVPixelBuffer(imageBuffer, NULL, &image);
            if (image) {
                if (gScene.video.frame) {
                    CGImageRelease(gScene.video.frame);
                }
                gScene.video.frame = image;
                gScene.video.frames++;
            }
        }
        CFRelease(sampleBuffer);
    }

    // moving picture: rescan every 5th frame, the metadata sink's rate (a scan still running skips one)
    if (gScene.video.frames % kSceneVideoRescanEveryFrames == 1) {
        atomic_fetch_add(&gScene.scan.gen, 1);
    }
}

// The qr source's code is made off the pump queue, like the scene's QR (SceneQRWarm: a first Core Image render can take
// most of a second) and handed back to the pump queue, where it is kept only if that source is still the one applied.
// Until then the canvas is plain white. Text too long for a QR code (the CLI refuses it first) falls back to the scene.
static void SceneQRSourceMake(NSString *spec, long gen) {   // pump queue
    const char *encoded = spec.UTF8String + kSceneQRPrefixLen;
    size_t encodedLength = strlen(encoded);
    NSMutableData *text = [NSMutableData dataWithLength:encodedLength];
    text.length = QRSpecDecode(encoded, encodedLength, text.mutableBytes);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            double start = CFAbsoluteTimeGetCurrent();
            CIFilter *filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
            [filter setValue:text forKey:@"inputMessage"];
            [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
            CIImage *ciImage = text.length ? filter.outputImage : nil;
            CGImageRef image =
                ciImage ? [[CIContext contextWithOptions:nil] createCGImage:ciImage fromRect:ciImage.extent] : NULL;
            double elapsedMs = (CFAbsoluteTimeGetCurrent() - start) * 1000;
            dispatch_async(gSessionLayer.pumpQueue, ^{
                if (gen != gScene.applied.gen || gScene.applied.kind != kSourceQR) {   // replaced meanwhile
                    if (image) {
                        CGImageRelease(image);
                    }
                    return;
                }
                if (image) {
                    gScene.applied.qrImage = image;
                    LOG(@"source qr: %zux%zu modules (%lu bytes of text) in %.1f ms", CGImageGetWidth(image),
                        CGImageGetHeight(image), (unsigned long)text.length, elapsedMs);
                } else {
                    gScene.applied.kind = kSourceScene;
                    LOG(@"source qr: no code for %lu bytes of text (empty, or too long) -> falling back to scene",
                        (unsigned long)text.length);
                }
                atomic_fetch_add(&gScene.scan.gen, 1);
            });
        }
    });
}

static void SceneApplySource(void) {   // pump queue
    long gen = atomic_load(&gScene.request.gen);
    if (gen == gScene.applied.gen) {
        return;
    }
    gScene.applied.gen = gen;
    NSString *spec;
    @synchronized (gScene.request.lock) {
        spec = gScene.request.spec ?: @"scene";
    }

    if (gScene.applied.stillImage) {
        CGImageRelease(gScene.applied.stillImage);
        gScene.applied.stillImage = NULL;
    }

    if (gScene.applied.qrImage) {
        CGImageRelease(gScene.applied.qrImage);
        gScene.applied.qrImage = NULL;
    }

    SceneVideoClose();
    if (gScene.applied.kind == kSourceWebcam) {
        WebcamLeave();
    }
    gScene.applied.kind = kSourceScene;
    gScene.applied.path = nil;

    if ([spec hasPrefix:@"image:"]) {
        gScene.applied.path = [spec substringFromIndex:kSceneImagePrefixLen];
        gScene.applied.stillImage = SceneLoadImage(gScene.applied.path);
        gScene.applied.kind = gScene.applied.stillImage ? kSourceImage : kSourceScene;
        CGImageRef still = gScene.applied.stillImage;
        LOG(@"source applied: image %@ -> %@", gScene.applied.path,
            still ? [NSString stringWithFormat:@"%zux%zu", CGImageGetWidth(still), CGImageGetHeight(still)]
                  : @"LOAD FAILED (falling back to scene)");
    } else if ([spec hasPrefix:@"video:"]) {
        gScene.applied.path = [spec substringFromIndex:kSceneVideoPrefixLen];
        gScene.applied.kind = kSourceVideo;
        BOOL opened = SceneVideoOpen();
        if (!opened) {
            gScene.applied.kind = kSourceScene;
        }
        LOG(@"source applied: video %@ -> %@", gScene.applied.path,
            opened ? @"decoding" : @"OPEN FAILED (falling back to scene)");
    } else if ([spec isEqualToString:@"webcam"] || [spec hasPrefix:@"webcam:"]) {
        gScene.applied.kind = kSourceWebcam;
        WebcamApply(spec);
        LOG(@"source applied: webcam %@",
            spec.length > kWcSpecPrefixLen ? [spec substringFromIndex:kWcSpecPrefixLen] : @"(default camera)");
    } else if ([spec hasPrefix:@"qr:"]) {
        gScene.applied.kind = kSourceQR;
        SceneQRSourceMake(spec, gen);
        LOG(@"source applied: qr (making the code)");
    } else {
        LOG(@"source applied: scene");
    }

    atomic_fetch_add(&gScene.scan.gen, 1);
}

// ---- scene drawing (canvas coordinates: origin top-left, y down) ----

// The colours the scene is drawn in, as red, green and blue from 0 to 1. All are opaque.
typedef struct {
    CGFloat red;
    CGFloat green;
    CGFloat blue;
} SceneColor;

static const SceneColor kSceneBlack = {0, 0, 0};
static const SceneColor kSceneWhite = {1, 1, 1};
static const SceneColor kSceneBackground = {0.15, 0.3, 0.45};   // a slate blue
static const SceneColor kSceneRed = {1, 0, 0};
static const SceneColor kSceneGreen = {0, 1, 0};
static const SceneColor kSceneBlue = {0, 0, 1};
static const SceneColor kSceneMovingBar = {1, 1, 0.2};          // a light yellow

static void SceneSetFill(CGContextRef context, SceneColor color) {
    CGContextSetRGBFillColor(context, color.red, color.green, color.blue, 1);
}

static void SceneDrawImage(CGContextRef context, CGImageRef image, CGRect rect, BOOL crisp) {
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, rect.origin.x, rect.origin.y + rect.size.height);
    CGContextScaleCTM(context, 1, -1);
    if (crisp) {
        CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    }
    CGContextDrawImage(context, CGRectMake(0, 0, rect.size.width, rect.size.height), image);
    CGContextRestoreGState(context);
}

// the qr source: the code centred on white, at whole pixels per module (QRModuleScale), on a pixel boundary
static void SceneDrawQRSource(CGContextRef context, double width, double height) {
    SceneSetFill(context, kSceneWhite);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    if (!gScene.applied.qrImage) {
        return;
    }
    double modules = CGImageGetWidth(gScene.applied.qrImage);
    double side = modules * QRModuleScale((int)modules, width, height);
    SceneDrawImage(context, gScene.applied.qrImage,
                   CGRectMake(floor((width - side) / 2), floor((height - side) / 2), side, side), YES);
}

static void SceneDrawFill(CGContextRef context, CGImageRef image, double width, double height) {   // aspect-fill
    double imageWidth = CGImageGetWidth(image);
    double imageHeight = CGImageGetHeight(image);
    double scale = MAX(width / imageWidth, height / imageHeight);
    SceneDrawImage(context, image,
                   CGRectMake((width - imageWidth * scale) / 2, (height - imageHeight * scale) / 2, imageWidth * scale,
                              imageHeight * scale),
                   NO);
}

// The scene's QR code is built off the pump queue, starting at activation (ControlEnqueue).
// Core Image's first render in an app compiles Metal functions into that app's own cache
// (Library/Caches/<bundle>/com.apple.metal). With that cache empty, after a fresh install or on a new device, it takes
// 0.2-0.8 s, and more on a busy or just-booted simulator. Inside the first pump tick, it would delay the first frame.
// Until the QR code is ready, the scene draws its white square without it. Barcode detection runs again when it
// arrives.
static void SceneQRWarm(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                double start = CFAbsoluteTimeGetCurrent();
                const char *text = getenv(ENV_QR_TEXT);   // the QR's text (default: a fixed URL)
                CIFilter *filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
                [filter setValue:[@(text ?: "https://simcam.dev/m12-qr") dataUsingEncoding:NSUTF8StringEncoding]
                     forKey:@"inputMessage"];
                [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
                CIImage *ciImage = [filter.outputImage
                    imageByApplyingTransform:CGAffineTransformMakeScale(kSceneQRModulePixels, kSceneQRModulePixels)];
                atomic_store(&gScene.sceneQR.ready,
                             [[CIContext contextWithOptions:nil] createCGImage:ciImage fromRect:ciImage.extent]);
                LOG(@"TIMING scene QR ready in %.1f ms (%.1f ms after process start)",
                    (CFAbsoluteTimeGetCurrent() - start) * 1000,
                    (CFAbsoluteTimeGetCurrent() - ProcessStartTime()) * 1000);
            }
        });
    });
}

static CGImageRef SceneQRImage(void) {   // pump queue; NULL until ready
    // The rescan is requested here, on the pump queue, when the scene first draws the QR: bumped from the background
    // block it could land between this tick's render (no QR yet) and its barcode scan, which would then cache a scan
    // without the QR.
    if (!gScene.sceneQR.image) {
        SceneQRWarm();
        gScene.sceneQR.image = atomic_load(&gScene.sceneQR.ready);
        if (gScene.sceneQR.image) {
            atomic_fetch_add(&gScene.scan.gen, 1);
        }
    }
    return gScene.sceneQR.image;
}

static const char *kSceneDefaultEAN13 = "5901234123457";
// EAN-13: 95 modules (start, 6 left digits, centre, 6 right digits, end) between 11-module quiet zones
#define kEAN13Digits 13
#define kEAN13Modules 95
#define kEAN13QuietModules 11            // each quiet zone
#define kEAN13QuietZoneModules 22        // both quiet zones
#define kEAN13BarTop 0.08                // the bars leave this share of the barcode's height white above them
#define kEAN13BarHeight 0.84             // and are this share of its height

static void SceneDrawEAN13(CGContextRef context, CGRect rect) {
    // The EAN-13 digit codes: L (odd parity) and G (even parity) for the left half, R for the right half, and the
    // first digit's parity pattern for the six left digits (P).
    static const char *L[] = {"0001101", "0011001", "0010011", "0111101", "0100011",
                              "0110001", "0101111", "0111011", "0110111", "0001011"};
    static const char *G[] = {"0100111", "0110011", "0011011", "0100001", "0011101",
                              "0111001", "0000101", "0010001", "0001001", "0010111"};
    static const char *R[] = {"1110010", "1100110", "1101100", "1000010", "1011100",
                              "1001110", "1010000", "1000100", "1001000", "1110100"};
    static const char *P[] = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
                              "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};

    // the barcode's 13 digits (anything else: the default)
    const char *digits = getenv(ENV_EAN13) ?: kSceneDefaultEAN13;
    if (strlen(digits) != kEAN13Digits) {
        digits = kSceneDefaultEAN13;
    }
    char bits[kEAN13Modules + 1] = {0};
    strcat(bits, "101");
    for (int i = 1; i <= 6; i++) {
        strcat(bits, (P[digits[0] - '0'][i - 1] == 'L' ? L : G)[digits[i] - '0']);
    }

    strcat(bits, "01010");
    for (int i = 7; i <= 12; i++) {
        strcat(bits, R[digits[i] - '0']);
    }

    strcat(bits, "101");
    double moduleWidth = floor(rect.size.width / (kEAN13Modules + kEAN13QuietZoneModules));   // whole pixels
    if (moduleWidth < 1) {
        moduleWidth = 1;
    }
    double barcodeWidth = moduleWidth * (kEAN13Modules + kEAN13QuietZoneModules);
    double left = rect.origin.x + (rect.size.width - barcodeWidth) / 2;
    SceneSetFill(context, kSceneWhite);
    CGContextFillRect(context, CGRectMake(left, rect.origin.y, barcodeWidth, rect.size.height));
    SceneSetFill(context, kSceneBlack);

    for (int i = 0; i < kEAN13Modules; i++) {
        if (bits[i] == '1') {
            CGContextFillRect(context, CGRectMake(left + (kEAN13QuietModules + i) * moduleWidth,
                                                  rect.origin.y + rect.size.height * kEAN13BarTop, moduleWidth,
                                                  rect.size.height * kEAN13BarHeight));
        }
    }
}

// Where the generated scene's parts sit. Sizes are shares of the canvas's width, its height or its shorter side.

// The colour bar across the top: red, green and blue, left to right. It stays inside the central 70 % of the width,
// which aspect-fill previews don't crop.
#define kSceneColorBarLeft 0.15          // of the width
#define kSceneColorBarWidth 0.70         // of the width
#define kSceneColorBarTop 0.12           // of the height
#define kSceneColorBarHeight 0.05        // of the shorter side
#define kSceneColorBarMinHeight 30       // pixels

// The "F" glyph below the bar's left end. An F looks different under every rotation and mirror.
#define kSceneGlyphLeft 0.17             // of the width
#define kSceneGlyphGap 0.04              // below the colour bar, of the shorter side
#define kSceneGlyphHeight 0.16           // of the shorter side
#define kSceneGlyphStroke 0.18           // the stroke width, of the glyph's height
#define kSceneGlyphTopArm 0.7            // the top arm's length, of the glyph's height
#define kSceneGlyphMiddleArmTop 0.42     // where the middle arm starts, of the glyph's height
#define kSceneGlyphMiddleArm 0.5         // the middle arm's length, of the glyph's height

// The QR code on a white square, and the EAN-13 barcode. Where they sit depends on the canvas's orientation.
#define kSceneQRMargin 0.06              // the white square's margin around the code, of the code's side
#define kSceneEANHeight 0.20             // of the shorter side

typedef struct {
    double qrSide;                       // of the shorter side
    double qrCentreX;                    // of the width
    double qrCentreY;                    // of the height
    double eanLeft;                      // of the width
    double eanTop;                       // of the height
    double eanWidth;                     // of the width
} SceneCodesLayout;

static const SceneCodesLayout kSceneCodesPortrait = {
    .qrSide = 0.46,
    .qrCentreX = 0.5,
    .qrCentreY = 0.44,
    .eanLeft = 0.18,
    .eanTop = 0.66,
    .eanWidth = 0.64,
};

static const SceneCodesLayout kSceneCodesLandscape = {
    .qrSide = 0.42,
    .qrCentreX = 0.36,
    .qrCentreY = 0.58,
    .eanLeft = 0.56,
    .eanTop = 0.50,
    .eanWidth = 0.32,
};

static void SceneDrawGenerated(CGContextRef context, double width, double height) {
    double shortSide = MIN(width, height);
    BOOL landscape = width > height;
    SceneSetFill(context, kSceneBackground);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));

    // the colour bar
    double barX = width * kSceneColorBarLeft;
    double barWidth = width * kSceneColorBarWidth;
    double barY = height * kSceneColorBarTop;
    double barHeight = MAX(kSceneColorBarMinHeight, shortSide * kSceneColorBarHeight);
    SceneSetFill(context, kSceneRed);
    CGContextFillRect(context, CGRectMake(barX, barY, barWidth / 3, barHeight));
    SceneSetFill(context, kSceneGreen);
    CGContextFillRect(context, CGRectMake(barX + barWidth / 3, barY, barWidth / 3, barHeight));
    SceneSetFill(context, kSceneBlue);
    CGContextFillRect(context, CGRectMake(barX + 2 * barWidth / 3, barY, barWidth / 3, barHeight));

    // the "F": its stem, top arm and middle arm
    double glyphX = width * kSceneGlyphLeft;
    double glyphY = barY + barHeight + shortSide * kSceneGlyphGap;
    double glyphHeight = shortSide * kSceneGlyphHeight;
    double strokeWidth = glyphHeight * kSceneGlyphStroke;
    SceneSetFill(context, kSceneWhite);
    CGContextFillRect(context, CGRectMake(glyphX, glyphY, strokeWidth, glyphHeight));
    CGContextFillRect(context, CGRectMake(glyphX, glyphY, glyphHeight * kSceneGlyphTopArm, strokeWidth));
    CGContextFillRect(context, CGRectMake(glyphX, glyphY + glyphHeight * kSceneGlyphMiddleArmTop,
                                          glyphHeight * kSceneGlyphMiddleArm, strokeWidth));

    // the QR code and the EAN-13 barcode
    const SceneCodesLayout *layout = landscape ? &kSceneCodesLandscape : &kSceneCodesPortrait;
    double qrSide = shortSide * layout->qrSide;
    CGRect qrRect = CGRectMake(width * layout->qrCentreX - qrSide / 2, height * layout->qrCentreY - qrSide / 2, qrSide,
                               qrSide);
    SceneSetFill(context, kSceneWhite);
    CGContextFillRect(context, CGRectInset(qrRect, -qrSide * kSceneQRMargin, -qrSide * kSceneQRMargin));
    CGImageRef qrImage = SceneQRImage();
    if (qrImage) {
        SceneDrawImage(context, qrImage, qrRect, YES);
    }
    CGRect eanRect = CGRectMake(width * layout->eanLeft, height * layout->eanTop, width * layout->eanWidth,
                                shortSide * kSceneEANHeight);
    SceneDrawEAN13(context, eanRect);
}

// The canvas pool, made with the first frame.
static void SceneMakeCanvasPool(void) {
    if (gPump.canvasPool) {
        return;
    }
    NSDictionary *attributes = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferWidthKey: @(kSrcW),
        (id)kCVPixelBufferHeightKey: @(kSrcH),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    // a failed create leaves the pool NULL: this tick has no frame, and the next one tries again
    (void)CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attributes, &gPump.canvasPool);
}

// Moves the video or webcam source on by one tick. It returns the webcam source's frame for this tick (all empty for
// the other sources).
static WebcamTickFrame SceneAdvanceSource(SimcamSession *sess) {
    WebcamTickFrame webcam = {0};
    // a no-op unless the webcam source had locked this pump
    if (gScene.applied.kind != kSourceWebcam) {
        WebcamRestorePump(sess);
    }
    if (gScene.applied.kind == kSourceVideo) {
        SceneVideoAdvance();
    }
    if (gScene.applied.kind == kSourceWebcam) {
        webcam = WebcamAdvance(sess);
        if (webcam.rescan) {
            atomic_fetch_add(&gScene.scan.gen, 1);
        }
    }
    return webcam;
}

// The camera's capture angle at the current (virtual) device orientation. A new orientation bumps the scan generation:
// the codes are found again.
static double SceneCaptureAngle(SimcamSession *sess) {
    long position = sess && sess.position ? sess.position : AVCaptureDevicePositionBack;
    long deviceOrientation = OrientVirtualOn() ? OrientDevice() : UIDeviceOrientationPortrait;
    double captureAngle = CoordAngleFor(deviceOrientation, position);
    if (deviceOrientation != gScene.scan.lastOrientation) {
        gScene.scan.lastOrientation = deviceOrientation;
        atomic_fetch_add(&gScene.scan.gen, 1);
    }
    return captureAngle;
}

// The applied source's picture, filling the canvas (origin top-left, y down).
static void SceneDrawSource(CGContextRef context, double width, double height, CGImageRef webcamImage) {
    if (gScene.applied.kind == kSourceImage && gScene.applied.stillImage) {
        SceneDrawFill(context, gScene.applied.stillImage, width, height);
    } else if (gScene.applied.kind == kSourceVideo) {
        if (gScene.video.frame) {
            SceneDrawFill(context, gScene.video.frame, width, height);
        }
    } else if (gScene.applied.kind == kSourceWebcam && webcamImage) {
        SceneDrawFill(context, webcamImage, width, height);
    } else if (gScene.applied.kind == kSourceQR) {
        SceneDrawQRSource(context, width, height);
    } else {
        // also the webcam source while offline, under its band
        SceneDrawGenerated(context, width, height);
    }
}

// Draws the source into buffer (a kSrcW x kSrcH canvas). The content is upright for the capture angle: the app's
// rotation (the coordinator's capture angle, clockwise) brings it upright, exactly as with a device's sensor buffer.
static void SceneDrawCanvas(CVPixelBufferRef buffer, double captureAngle, WebcamTickFrame webcam) {
    BOOL portraitCanvas = ((int)captureAngle % 180) != 0;
    double canvasWidth = portraitCanvas ? kSrcH : kSrcW;
    double canvasHeight = portraitCanvas ? kSrcW : kSrcH;
    // a pool's buffer always locks; if it didn't, the base address is NULL and CG draws into memory of its own
    (void)CVPixelBufferLockBaseAddress(buffer, 0);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(buffer), kSrcW, kSrcH, 8,
                                                 CVPixelBufferGetBytesPerRow(buffer), colorSpace,
                                                 kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    SceneSetFill(context, kSceneBlack);
    CGContextFillRect(context, CGRectMake(0, 0, kSrcW, kSrcH));

    CGContextSaveGState(context);
    CGContextTranslateCTM(context, kSrcW / 2.0, kSrcH / 2.0);
    CGContextRotateCTM(context, captureAngle * M_PI / 180.0);
    CGContextTranslateCTM(context, -canvasWidth / 2, canvasHeight / 2);   // canvas: origin top-left, y down
    CGContextScaleCTM(context, 1, -1);
    SceneDrawSource(context, canvasWidth, canvasHeight, webcam.image);
    if (webcam.band) {
        WebcamDrawOfflineBand(context, canvasWidth, canvasHeight, webcam.band);
    }
    CGContextRestoreGState(context);

    if (gScene.applied.kind == kSourceScene) {   // moving bar (sensor bottom edge)
        double barX = (gPump.frameNo * kSceneBarStepPx) % kSrcW;
        SceneSetFill(context, kSceneMovingBar);
        CGContextFillRect(context, CGRectMake(barX, 0, kSceneBarWidthPx, kSceneBarHeightPx));
    }

    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);
    CVPixelBufferUnlockBaseAddress(buffer, 0);
}

// The pump's source frame for this tick (+1): the applied source, upright for the capture angle. NULL: no frame.
static CVPixelBufferRef SceneRenderSource(SimcamSession *sess) {
    SceneMakeCanvasPool();
    SceneApplySource();
    WebcamTickFrame webcam = SceneAdvanceSource(sess);
    double captureAngle = SceneCaptureAngle(sess);

    // the webcam frame path: the canvas comes from the ring frame (webcam_path.inc); NULL: draw it below
    if (webcam.buffer) {
        CVPixelBufferRef canvas = WebcamCanvas(sess, webcam.buffer, (int)captureAngle);
        if (canvas) {
            gPump.frameNo++;
            return canvas;
        }
        webcam.image = gWebcam.frame.image;
    }

    CVPixelBufferRef buffer = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, gPump.canvasPool, &buffer);
    if (!buffer) {
        return NULL;   // no buffer from the pool: no frame this tick
    }
    SceneDrawCanvas(buffer, captureAngle, webcam);
    gPump.frameNo++;
    return buffer;
}
