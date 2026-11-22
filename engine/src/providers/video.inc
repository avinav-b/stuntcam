// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): frame source, per-sink conversion, preview frames.
//
// Provides: SceneRenderFixture, SinkConvert (providers/pump.inc, providers/photo.inc; SinkConvert also audio.inc)
//           SinkSampleBuffer, PreviewPush (providers/pump.inc)
//           PumpFps (providers/webcam.inc)
// Owns:     gPump, the pump's shared state, defined in providers/pump.h with the canvas size (pump queue; read by
//           the other providers; providers/pump.inc writes latest and fps)
//           pCAImageQueue* (resolved by fig/session_hook.inc)
// Uses:     fig/sources.inc, core/init.inc, fig/session_state.inc
//
// ---- frame source ------------------------------------------------------------
#include "pump.h"
#include "../fig/session.h"
#include "../core/init.h"
#include "../fig/sources.h"

// the pump's frame rate, kPumpDefaultFps before the first start
static double PumpFps(void) {
    return gPump.fps > 0 ? gPump.fps : kPumpDefaultFps;
}

static CVPixelBufferRef SceneRenderFixture(void) {
    if (!gPump.canvasPool) {
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id)kCVPixelBufferWidthKey: @(kSrcW), (id)kCVPixelBufferHeightKey: @(kSrcH),
                                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVReturn created = CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &gPump.canvasPool);
        if (created != kCVReturnSuccess) {
            gPump.canvasPool = NULL;
            static int once;
            if (!once++) {
                LOG(@"fixture: CVPixelBufferPoolCreate failed; no frame until a pool can be made");
            }
            return NULL;   // no frame this tick; the next tick tries again
        }
        // the fixture image drawn into the plain scene
        const char *fixturePath = EnvConfig(ENV_FIXTURE_IMAGE, ENV_LEGACY_FIXTURE_IMAGE);
        if (fixturePath) {
            CGImageSourceRef source =
                CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(fixturePath)], NULL);
            if (source) {
                gPump.fixtureImage = CGImageSourceCreateImageAtIndex(source, 0, NULL);
                CFRelease(source);
            }
        }
    }

    CVPixelBufferRef frame = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, gPump.canvasPool, &frame);
    if (!frame) {
        return NULL;   // no buffer from the pool: no frame this tick
    }
    // a pool's buffer always locks; if it didn't, the base address is NULL and CG draws into memory of its own
    (void)CVPixelBufferLockBaseAddress(frame, 0);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(frame), kSrcW, kSrcH, 8,
                                             CVPixelBufferGetBytesPerRow(frame), colorSpace,
                                             kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGContextSetRGBFillColor(ctx, 0.15, 0.3, 0.45, 1);   // a slate-blue background
    CGContextFillRect(ctx, CGRectMake(0, 0, kSrcW, kSrcH));

    // sensor-like: a landscape buffer whose content is upright when turned 90° clockwise (a portrait device)
    CGContextSaveGState(ctx);

    // Rotating by -pi/2 (SIMCAM_OFF_SENSOR_LAYOUT) puts the scene's top on the buffer's RIGHT (photos with EXIF 6 and
    // movies with the 90 deg track transform come out upside down). A device's native back-camera buffer has the
    // scene's top on the LEFT. SIMCAM_OFF_SENSOR_LAYOUT keeps that old layout.
    CGContextTranslateCTM(ctx, kSrcW / 2.0, kSrcH / 2.0);
    CGContextRotateCTM(ctx, SensorLayoutOn() ? M_PI_2 : -M_PI_2);

    // the upright canvas is kSrcH wide and kSrcW tall (portrait); the fixture is aspect-fit into 95% of its width
    if (gPump.fixtureImage) {
        double drawW = kSrcH * 0.95;
        double drawH = drawW * CGImageGetHeight(gPump.fixtureImage) / CGImageGetWidth(gPump.fixtureImage);
        CGContextDrawImage(ctx, CGRectMake(-drawW / 2, -drawH / 2, drawW, drawH), gPump.fixtureImage);
    }

    // the "up" marker: a red bar, 40 px high, near the top of the upright picture
    CGContextSetRGBFillColor(ctx, 0.9, 0.1, 0.1, 1);
    CGContextFillRect(ctx, CGRectMake(-kSrcH * 0.4, kSrcW * 0.38, kSrcH * 0.8, 40));
    CGContextRestoreGState(ctx);

    // a moving yellow bar (24 x 60 px), 12 px further along each frame
    double x = (gPump.frameNo * 12) % kSrcW;
    CGContextSetRGBFillColor(ctx, 1, 1, 0.2, 1);
    CGContextFillRect(ctx, CGRectMake(x, 0, 24, 60));
    CGContextRelease(ctx);
    CGColorSpaceRelease(colorSpace);
    CVPixelBufferUnlockBaseAddress(frame, 0);
    gPump.frameNo++;
    return frame;
}

static CVPixelBufferRef SinkConvertScale(SimcamSink *sink, CVPixelBufferRef src);   // below

// The VideoToolbox rotation for a clockwise turn of deg (0, 90, 180 or 270). A quarter turn is clockwise; with
// SIMCAM_OFF_SENSOR_LAYOUT (the old sensor layout) the quarter turns are swapped: 90 turns counter-clockwise and 270
// clockwise.
static CFStringRef SinkRotationFor(int deg) {
    BOOL sensorLayout = SensorLayoutOn();
    BOOL quarterTurn = deg == 90 || deg == 270;
    if ((deg == 90) == sensorLayout && quarterTurn) {
        return kVTRotation_CW90;
    }
    if (deg == 180) {
        return kVTRotation_180;
    }
    if (quarterTurn) {
        return kVTRotation_CCW90;
    }
    return kVTRotation_0;
}

// Applies the connection's rotationDegrees (clockwise, like the device's ISP) and mirroring before the scaling and
// format conversion. SIMCAM_OFF_DATA_OUTPUT_ROTATION turns this off (the buffer as the sensor gives it).
static CVPixelBufferRef SinkRotate(SimcamSink *sink, CVPixelBufferRef src) {
    if (!DataOutputRotationOn() || (sink.dataOutputRotationDeg % 360 == 0 && !sink.mirrored)) {
        return CVPixelBufferRetain(src);
    }
    int deg = ((sink.dataOutputRotationDeg % 360) + 360) % 360;
    BOOL swapsSides = (deg == 90 || deg == 270);
    size_t width = CVPixelBufferGetWidth(src);
    size_t height = CVPixelBufferGetHeight(src);

    if (!sink.rotationSess) {
        VTPixelRotationSessionRef rotSess = NULL;
        CVPixelBufferPoolRef pool = NULL;
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id)kCVPixelBufferWidthKey: @(swapsSides ? height : width),
                                (id)kCVPixelBufferHeightKey: @(swapsSides ? width : height),
                                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        if (VTPixelRotationSessionCreate(NULL, &rotSess) != noErr || !rotSess ||
            CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &pool) != kCVReturnSuccess) {
            if (rotSess) {
                VTPixelRotationSessionInvalidate(rotSess);
                CFRelease(rotSess);
            }
            if (pool) {
                CVPixelBufferPoolRelease(pool);
            }
            static int once;   // once per process, whichever sink fails first (as the VTPixelRotation line below)
            if (!once++) {
                LOG(@"vdo %@: no rotation session or pool; frames go unrotated", sink.sinkID);
            }
            return CVPixelBufferRetain(src);   // as for a failed rotation, the sink keeps its frame rate
        }
        // every rotation session supports these two properties and values, so setting them can't fail
        (void)VTSessionSetProperty(rotSess, kVTPixelRotationPropertyKey_Rotation, SinkRotationFor(deg));
        if (sink.mirrored) {
            (void)VTSessionSetProperty(rotSess, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
        }
        sink.rotationSess = rotSess;
        sink.rotationPool = pool;
        LOG(@"vdo %@ rotating %d deg cw mirrored=%d -> %zux%zu then %dx%d", sink.sinkID, deg, sink.mirrored,
            swapsSides ? height : width, swapsSides ? width : height, sink.width, sink.height);
    }

    CVPixelBufferRef dst = NULL;

    if (CVPixelBufferPoolCreatePixelBuffer(NULL, sink.rotationPool, &dst) != kCVReturnSuccess || !dst) {
        static int once;
        if (!once++) {
            LOG(@"vdo %@: no buffer from the rotation pool; this frame goes unrotated", sink.sinkID);
        }
        return CVPixelBufferRetain(src);
    }

    OSStatus status = VTPixelRotationSessionRotateImage(sink.rotationSess, src, dst);

    // On a failed rotation the sink still gets dst (its pixels not rotated), so it keeps its frame rate; the error is
    // logged once per process, not per frame.
    if (status) {
        static int once;
        if (!once++) {
            LOG(@"VTPixelRotation -> %d", (int)status);
        }
    }

    return dst;
}

// input converted for sink (+1): turned and mirrored for a video data output, then scaled and converted to the
// sink's size and pixel format when they differ
static CVPixelBufferRef SinkConvert(SimcamSink *sink, CVPixelBufferRef input) {
    CVPixelBufferRef src = [sink.kind isEqualToString:@"vdo"] ? SinkRotate(sink, input) : CVPixelBufferRetain(input);
    BOOL alreadyFits = sink.pixelFormat == kCVPixelFormatType_32BGRA && sink.width == (int)CVPixelBufferGetWidth(src) &&
                       sink.height == (int)CVPixelBufferGetHeight(src);
    if (alreadyFits) {
        return src;
    }
    CVPixelBufferRef converted = SinkConvertScale(sink, src);
    CVPixelBufferRelease(src);
    return converted;
}

static CVPixelBufferRef SinkConvertScale(SimcamSink *sink, CVPixelBufferRef src) {
    if (!sink.pool) {
        CVPixelBufferPoolRef pool = NULL;
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(sink.pixelFormat),
                                (id)kCVPixelBufferWidthKey: @(sink.width), (id)kCVPixelBufferHeightKey: @(sink.height),
                                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &pool);
        sink.pool = pool;
        VTPixelTransferSessionRef transferSess = NULL;
        VTPixelTransferSessionCreate(NULL, &transferSess);
        sink.transferSess = transferSess;
    }

    CVPixelBufferRef dst = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, sink.pool, &dst);
    OSStatus status = VTPixelTransferSessionTransferImage(sink.transferSess, src, dst);

    // As in SinkRotate: on a failed transfer the sink still gets dst (its pixels not converted); the error is logged
    // once per process.
    if (status) {
        static int once;
        if (!once++) {
            LOG(@"VTPixelTransfer -> %d", (int)status);
        }
    }

    return dst;
}


// frame as a sample buffer at pts, one pump frame long (+1)
static CMSampleBufferRef SinkSampleBuffer(CVPixelBufferRef frame, CMTime pts) {
    CMVideoFormatDescriptionRef formatDesc = NULL;
    CMVideoFormatDescriptionCreateForImageBuffer(NULL, frame, &formatDesc);
    CMSampleTimingInfo timing = { CMTimeMake(1, (int32_t)PumpFps()), pts, kCMTimeInvalid };
    CMSampleBufferRef sampleBuffer = NULL;
    CMSampleBufferCreateReadyWithImageBuffer(NULL, frame, formatDesc, &timing, &sampleBuffer);
    CFRelease(formatDesc);
    return sampleBuffer;
}

// ---- preview ----------------------------------------------------------------
// the opt-in CAImageQueue path's queue capacity, in images
enum { kPreviewImageQueueCapacity = 8 };

// Shows frame in the preview sink: by default it posts the frame's IOSurface itself; with SIMCAM_PREVIEW_IMAGE_QUEUE
// it inserts it into a CAImageQueue. The first frame also reports the preview started.
static void PreviewPush(CMBaseObjectRef session, SimcamSink *sink, CVPixelBufferRef frame) {
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(frame);

    // SIMCAM_PREVIEW_IMAGE_QUEUE: the CAImageQueue path instead (its arguments are unresolved, so it shows nothing)
    BOOL surfaceMode = getenv(ENV_PREVIEW_IMAGE_QUEUE) == NULL || !pCAImageQueueCreate;

    if (surfaceMode) {   // post the IOSurface itself as "ImageQueue" (CALayer.contents accepts an IOSurface)
        gQuietPost = YES;
        NSDictionary *info = @{@"SectionID": sink.sinkID, @"ImageQueue": (__bridge id)surface,
                               @"ImageQueueWidth": @(sink.width), @"ImageQueueHeight": @(sink.height),
                               @"RotationDegrees": @(sink.rotation)};
        pCMNotificationCenterPostNotification(pCMNotificationCenterGetDefaultLocalCenter(), CFSTR("ImageQueueUpdated"),
                                              session, (__bridge CFDictionaryRef)info, 0);
        if (sink.frames == 0) {
            static int firstEver;
            if (!firstEver++) {
                LOG(@"TIMING first preview frame %.1f ms after process start",
                    (CFAbsoluteTimeGetCurrent() - ProcessStartTime()) * 1000);
            }
            LOG(@"preview(surface) first ImageQueueUpdated %@", sink.sinkID);
            SessPost(session, @"DidStartPreviewing", @{@"SectionID": sink.sinkID});
            SessPost(session, @"DidDisplayFirstPreviewFrame", @{@"SectionID": sink.sinkID});
        }
        sink.frames++;
        return;
    }

    if (!sink.imageQueue) {
        sink.imageQueue = pCAImageQueueCreate(sink.width, sink.height, kPreviewImageQueueCapacity);
        sink.bufferIDsBySurface = [NSMutableDictionary dictionary];
        LOG(@"preview CAImageQueueCreate(%d,%d) -> %p", sink.width, sink.height, sink.imageQueue);
        SessPost(session, @"ImageQueueUpdated",
                 @{@"SectionID": sink.sinkID, @"ImageQueue": (__bridge id)sink.imageQueue,
                   @"ImageQueueWidth": @(sink.width), @"ImageQueueHeight": @(sink.height),
                   @"RotationDegrees": @(sink.rotation), @"ImageQueueSlot": @0});
        SessPost(session, @"DidStartPreviewing", @{@"SectionID": sink.sinkID});
    }

    NSNumber *surfID = @(IOSurfaceGetID(surface));
    NSNumber *regID = sink.bufferIDsBySurface[surfID];

    if (!regID) {
        regID = @(pCAImageQueueRegisterIOSurfaceBuffer(sink.imageQueue, surface, 0));
        sink.bufferIDsBySurface[surfID] = regID;
        CFRetain(surface);   // keep registered surfaces alive
    }

    // SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_A / SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_B: the two unknown integer arguments of
    // CAImageQueueInsertImage (for probing)
    uint32_t argA = getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_A) ? atoi(getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_A)) : 0;
    uint32_t argB = getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_B) ? atoi(getenv(ENV_PREVIEW_IMAGE_QUEUE_ARG_B)) : 0;
    bool ok = pCAImageQueueInsertImage(sink.imageQueue, -1.0, argA, regID.unsignedLongLongValue, argB, NULL, NULL);
    if (sink.frames < 3 || !ok) {
        LOG(@"preview insert frame %d id=%@ -> %d", sink.frames, regID, ok);
    }
    if (sink.frames == 1) {
        SessPost(session, @"DidDisplayFirstPreviewFrame", @{@"SectionID": sink.sinkID});
    }
    sink.frames++;
}
