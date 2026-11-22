// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included by providers/webcam.inc): the webcam source's frame path.
//
// Provides: WebcamCanvas (providers/scene.inc); WebcamSinkOutput (providers/pump.inc)
//           WebcamMovieOutput (providers/audio.inc); WebcamOutputCacheFree (fig/session_state.inc)
//           WebcamPathName, WebcamPathOn, WebcamPathWarm, WebcamCopyFrame, WebcamPathReset, WebcamPathFree
//           (providers/webcam.inc, which includes this)
// Owns:     gWebcamPath, the frame path's state, defined in providers/webcam.h (pump queue)
// Uses:     webcam/ring.c, providers/webcam.inc, providers/mirror.inc,
//           fig/session_state.inc
#include "../fig/session.h"
#include "pump.h"
#include "../webcam/ring.h"
#include "webcam.h"
#include "mirror.h"

// ============================================================================
// While `webcam` is the source and live, each new ring frame is copied once, under the seqlock, into a buffer from a
// ring-sized pool, and every output is made straight from that buffer in one pass: cropped to the output's aspect
// (aspect fill, centred), scaled, turned and mirrored into the output's size and pixel format.
//   canvas   What the consumers of the tick's source buffer take (metadata, photo, movie): the frame cropped to the
//            active format's aspect at the ring's size, turned by the capture angle like the scene's sensor buffer
//            (content upright once the app applies the capture angle). The plain landscape case is the frame itself.
//   video    Video data sinks: the frame turned by (connection rotation - capture angle), mirrored when the
//            connection mirrors, in the sink's size and pixel format (the canvas plus SinkRotate and SinkConvertScale
//            in one pass).
//   preview  The preview sink's size, mirrored as MirrorFlip mirrors the front preview.
//   movie    A recording whose size or format differs from the canvas's (a 1080p ring at a 4K format), when the aspects
//            match: the canvas's picture at the movie's size in its format (what SinkConvert does on the CPU).
// Every output is kept per sink (and per recording) while the same ring frame is shown again (60 fps from a 30 fps
// camera), so the second tick of a frame renders nothing.
// Backends:
//   gpu  The default: Core Image on the Metal device. Warmed up off the pump queue (WebcamPathWarm); ticks before that
//        use cg. The pump queue waits at most 250 ms for a render (sess_SetConfiguration syncs to it from the app's
//        thread; WebcamRenderGpu has the mechanics). A render over 250 ms is dropped (never delivered or cached); the
//        path is cg until it completes, then gpu again if it ended OK: a cold kernel compile on a loaded Mac is one
//        slow render, not a broken GPU. The third late render within 30 s, or a render error, makes the path cg for
//        good. No render starts while one is late, so at most one late render's buffers, task and context are held.
//   vt   Only with SIMCAM_WEBCAM_PATH=vt: VTPixelRotation, then VTPixelTransfer cropping to the clean aperture.
//        In the simulator VideoToolbox scales on the CPU. That costs 5-17 ms per scaled 1080p-4K output, and 42-122 %
//        app CPU in the measured cases. So vt is never a fallback.
//   cg   scene.inc's CG canvas, the scene/image/video path. It cost 25-62 % app CPU in the same cases.
//        Unlike the others it doesn't crop to a 4:3 format. The canvas is 16:9, and the sinks stretch it into 4:3
//        outputs.
// SIMCAM_WEBCAM_PATH=gpu|vt|cg picks a backend. A gpu or vt failure logs once and moves to cg for the rest of the
// process.
// Scene, image and video never reach this file. Everything here runs on the pump queue, except the waits and the
// warm-up.
// Test hook: SIMCAM_TEST_WEBCAM_GPU_STALL=<n>:<ms>[:<count>] delays the completion of the gpu renders from the n-th on
// by <ms>, for all of them or for <count>.
// ============================================================================

#define kWcGpuWaitMs 250                 // the longest the pump queue waits for a gpu render
#define kWcGpuLateWindowNs (30 * NSEC_PER_SEC)   // kWcGpuMaxLate late renders within this window: cg for good
#define kWcSlowRenderMs 30               // a render this slow is logged, the first kWcSlowRenderLogs times
#define kWcSlowRenderLogs 8
#define kWcAspectTolerance 0.005         // a movie and the canvas have the same aspect within 0.5 %

static const char *WebcamPathName(int path) {
    if (path == kWcPathGpu) {
        return "gpu";
    }
    if (path == kWcPathVt) {
        return "vt";
    }
    if (path == kWcPathCg && gWebcamPath.lateRender.running) {
        return "cg-late";
    }
    return "cg";
}

// SIMCAM_TEST_WEBCAM_GPU_STALL=<at>:<ms>[:<n>]: the gpu renders from the <at>-th on complete <ms> ms late (<n> of
// them, or all)
static void WebcamParseGpuStallTestFault(void) {
    const char *stall = getenv(ENV_TEST_WEBCAM_GPU_STALL);
    if (!stall || !strchr(stall, ':')) {
        return;
    }
    const char *secondColon = strchr(strchr(stall, ':') + 1, ':');
    gWebcamPath.testStall.from = atol(stall);
    gWebcamPath.testStall.ms = atol(strchr(stall, ':') + 1);
    gWebcamPath.testStall.count = secondColon ? atol(secondColon + 1) : -1;
    LOG(@"webcam: test hook: gpu renders from #%ld complete %ld ms late (%@)", gWebcamPath.testStall.from,
        gWebcamPath.testStall.ms,
        gWebcamPath.testStall.count < 0 ? @"all"
                                        : [NSString stringWithFormat:@"%ld of them", gWebcamPath.testStall.count]);
}

// The test hook's delay for the gpu render about to start, in ms (0: none). gpu renders are counted only while the
// hook is set.
static long WebcamTestStallMsForNextRender(void) {
    if (gWebcamPath.testStall.from < 0) {
        return 0;
    }
    gWebcamPath.testStall.renders++;
    if (gWebcamPath.testStall.renders < gWebcamPath.testStall.from) {
        return 0;
    }
    BOOL stallAll = gWebcamPath.testStall.count < 0;
    BOOL pastStalledRenders = gWebcamPath.testStall.renders >= gWebcamPath.testStall.from + gWebcamPath.testStall.count;
    if (!stallAll && pastStalledRenders) {
        return 0;
    }
    return gWebcamPath.testStall.ms;
}

static void WebcamPathChoose(void) {
    if (gWebcamPath.backend >= 0) {
        return;
    }
    const char *env = getenv(ENV_WEBCAM_PATH);
    if (env && !strcmp(env, "cg")) {
        gWebcamPath.backend = kWcPathCg;
    } else if (env && !strcmp(env, "vt")) {
        gWebcamPath.backend = kWcPathVt;
    } else {
        gWebcamPath.backend = kWcPathGpu;
    }
    WebcamParseGpuStallTestFault();
    if (gWebcamPath.backend < kWcPathGpu) {
        LOG(@"webcam: frame path %s%s", WebcamPathName(gWebcamPath.backend), env ? " (SIMCAM_WEBCAM_PATH)" : "");
    }
}

static BOOL WebcamPathOn(void) {
    WebcamPathChoose();
    return gWebcamPath.backend == kWcPathVt || (gWebcamPath.backend == kWcPathGpu && gWebcamPath.ciContext);
}

// The size of the centred crop of an upright source that fills an output of aspect aspectWidth:aspectHeight.
static void WebcamCropSize(double srcWidth, double srcHeight, double aspectWidth, double aspectHeight, 
                           double *cropWidth, double *cropHeight) {
    *cropWidth = srcWidth;
    *cropHeight = srcWidth * aspectHeight / aspectWidth;
    if (*cropHeight > srcHeight) {
        *cropHeight = srcHeight;
        *cropWidth = srcHeight * aspectWidth / aspectHeight;
    }
}

// The Core Image graph for one output (also the warm-up's).
static void WebcamImageGraph(CVPixelBufferRef src, WebcamOutputOp op, CVPixelBufferRef dst, CIImage **imageOut,
                             CIRenderDestination **destOut) {
    double srcWidth = CVPixelBufferGetWidth(src);
    double srcHeight = CVPixelBufferGetHeight(src);
    double outWidth = op.width;
    double outHeight = op.height;
    double cropWidth;
    double cropHeight;
    BOOL swap = op.turn % 180 == 90;

    // the output's size before the turn
    double uprightWidth = swap ? outHeight : outWidth;
    double uprightHeight = swap ? outWidth : outHeight;
    WebcamCropSize(srcWidth, srcHeight, uprightWidth, uprightHeight, &cropWidth, &cropHeight);

    // Core Image space: origin bottom-left, y up; a clockwise turn on screen is a negative angle
    CGAffineTransform transform = CGAffineTransformMakeTranslation(-srcWidth / 2, -srcHeight / 2);
    transform = CGAffineTransformConcat(
        transform, CGAffineTransformMakeScale(uprightWidth / cropWidth, uprightHeight / cropHeight));
    transform = CGAffineTransformConcat(transform, CGAffineTransformMakeRotation(-op.turn * M_PI / 180));
    if (op.mirror) {
        transform = CGAffineTransformConcat(transform, CGAffineTransformMakeScale(-1, 1));
    }
    transform = CGAffineTransformConcat(transform, CGAffineTransformMakeTranslation(outWidth / 2, outHeight / 2));
    CIImage *image = [CIImage imageWithCVPixelBuffer:src options:@{kCIImageColorSpace: [NSNull null]}];
    *imageOut = [[[image imageByClampingToExtent] imageByApplyingTransform:transform]
        imageByCroppingToRect:CGRectMake(0, 0, outWidth, outHeight)];
    CIRenderDestination *dest = [[CIRenderDestination alloc] initWithPixelBuffer:dst];
    dest.colorSpace = NULL;
    *destOut = dest;
}

// The warm-up's renders: each output format through every turn, with and without a mirror, and a plain scale with and
// without the 4:3 crop. Core Image builds a kernel per shape, and a cold one cost 75-182 ms on the pump queue.
static void WebcamWarmRenders(CIContext *context) {
    NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                            (id)kCVPixelBufferMetalCompatibilityKey: @YES};
    enum { kWarmFormats = 3, kWarmShapes = 9, kWarmOps = kWarmFormats * kWarmShapes };
    enum { kWarmSrcWidth = 64, kWarmSrcHeight = 36 };
    OSType formats[kWarmFormats] = {kCVPixelFormatType_32BGRA, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                    kCVPixelFormatType_420YpCbCr8BiPlanarFullRange};
    // turn, mirror, width, height
    static const int kWarmShapeTable[kWarmShapes][4] = {
        {0, 0, 48, 36},  {0, 0, 32, 18},  {0, 1, 32, 18},   {90, 0, 18, 32},  {90, 1, 36, 64},
        {180, 0, 32, 18}, {180, 1, 32, 18}, {270, 0, 27, 36}, {270, 1, 27, 36}};
    WebcamOutputOp ops[kWarmOps];
    for (int i = 0; i < kWarmOps; i++) {
        ops[i] = (WebcamOutputOp){kWarmShapeTable[i % kWarmShapes][0], kWarmShapeTable[i % kWarmShapes][1],
                                  kWarmShapeTable[i % kWarmShapes][2], kWarmShapeTable[i % kWarmShapes][3],
                                  formats[i / kWarmShapes]};
    }
    CVPixelBufferRef src = NULL;
    CVPixelBufferCreate(NULL, kWarmSrcWidth, kWarmSrcHeight, kCVPixelFormatType_32BGRA,
                        (__bridge CFDictionaryRef)attrs, &src);
    for (int i = 0; src && i < kWarmOps; i++) {
        CVPixelBufferRef out = NULL;
        CVPixelBufferCreate(NULL, ops[i].width, ops[i].height, ops[i].format, (__bridge CFDictionaryRef)attrs, &out);
        if (!out) {
            continue;
        }
        CIImage *image;
        CIRenderDestination *dest;
        WebcamImageGraph(src, ops[i], out, &image, &dest);
        [[context startTaskToRender:image toDestination:dest error:nil] waitUntilCompletedAndReturnError:nil];
        CVPixelBufferRelease(out);
    }
    if (src) {
        CVPixelBufferRelease(src);
    }
}

// Pump queue, when the warm-up is over: its context becomes the gpu path's, unless the path or the source changed
// meanwhile. No context means no Metal device: the CG canvas for good.
static void WebcamWarmFinished(CIContext *context, NSString *deviceName, double ms) {
    gWebcamPath.warming = NO;
    if (gWebcamPath.backend != kWcPathGpu) {
        return;
    }
    if (!context) {
        gWebcamPath.backend = kWcPathCg;
        LOG(@"webcam: no Metal device for the frame path; using the CG canvas");
        return;
    }
    if (!gWebcam.camera.on) {
        return;   // left meanwhile: the next apply warms up again
    }
    gWebcamPath.ciContext = context;
    LOG(@"webcam: frame path gpu (Core Image on %@; device, context and first renders %.1f ms, "
        @"off the pump queue)", deviceName, ms);
}

// Pump queue, from WebcamApply: the Metal device, the context and one small render of each kind (the kernel compiles),
// on a background queue, so the first frames don't wait for them on the pump queue.
static void WebcamPathWarm(void) {
    WebcamPathChoose();
    if (gWebcamPath.backend != kWcPathGpu || gWebcamPath.ciContext || gWebcamPath.warming) {
        return;
    }
    gWebcamPath.warming = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint64_t start = WebcamNowNs();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        CIContext *context = device ? [CIContext contextWithMTLDevice:device options:@{
                                          kCIContextWorkingColorSpace: [NSNull null],
                                          kCIContextOutputColorSpace: [NSNull null],
                                          kCIContextCacheIntermediates: @NO, kCIContextName: @"simcam.webcam"}]
                                    : nil;
        if (context) {
            WebcamWarmRenders(context);
        }
        NSString *name = device.name;
        double ms = (WebcamNowNs() - start) / 1e6;
        dispatch_async(gSessionLayer.pumpQueue, ^{ WebcamWarmFinished(context, name, ms); });
    });
}

// A buffer from *pool, which is remade when the size or format changes; IOSurface-backed and Metal-compatible.
static CVPixelBufferRef WebcamPoolBuffer(CVPixelBufferPoolRef *pool, int *poolWidth, int *poolHeight,
                                         OSType *poolFormat, int width, int height, OSType format) {
    if (*pool && (*poolWidth != width || *poolHeight != height || (poolFormat && *poolFormat != format))) {
        CVPixelBufferPoolRelease(*pool);
        *pool = NULL;
    }

    if (!*pool) {
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(format), (id)kCVPixelBufferWidthKey: @(width),
                                (id)kCVPixelBufferHeightKey: @(height), (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                                (id)kCVPixelBufferMetalCompatibilityKey: @YES};
        if (CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, pool) != kCVReturnSuccess) {
            *pool = NULL;
            return NULL;
        }
        *poolWidth = width;
        *poolHeight = height;
        if (poolFormat) {
            *poolFormat = format;
        }
    }

    CVPixelBufferRef buffer = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, *pool, &buffer);

    // VTPixelTransfer tags its YCbCr output 709 (and converts with it): the same here
    if (buffer && format != kCVPixelFormatType_32BGRA) {
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
    }

    return buffer;
}

// The newest ring frame into a pooled buffer. SCW_OK: *out holds it (+1). Otherwise as scw_read_latest.
static int WebcamCopyFrame(uint64_t lastSeq, scw_slot_read *slotRead, CVPixelBufferRef *out) {
    const scw_shm_reader *reader = &gWebcam.ring.reader;
    *out = NULL;
    CVPixelBufferRef buffer = WebcamPoolBuffer(&gWebcamPath.framePool.pool, &gWebcamPath.framePool.width,
                                               &gWebcamPath.framePool.height, NULL, (int)reader->geom.width,
                                               (int)reader->geom.height, kCVPixelFormatType_32BGRA);
    if (!buffer) {
        return SCW_E_ARG;
    }
    // a pool's buffer always locks; if it didn't, scw_read_latest refuses the NULL base address (SCW_E_ARG)
    (void)CVPixelBufferLockBaseAddress(buffer, 0);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(buffer);
    int rc = scw_read_latest(reader->map, &reader->geom, lastSeq, CVPixelBufferGetBaseAddress(buffer), bytesPerRow,
                             bytesPerRow * reader->geom.height, slotRead);
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    if (rc == SCW_OK) {
        *out = buffer;
    } else {
        CVPixelBufferRelease(buffer);
    }
    return rc;
}

// Pump queue: a render the pump gave up on has ended (ok: without an error), `ms` after it started.
static void WebcamGpuLateRenderEnded(BOOL ok, double ms) {
    if (!gWebcamPath.lateRender.running) {
        return;   // cg for good meanwhile (the timeout cap): stay there
    }
    gWebcamPath.lateRender.running = NO;
    if (!ok) {
        LOG(@"webcam: the late Core Image render ended in an error after %.0f ms; "
            @"the CG canvas for the rest of the process", ms);
        return;
    }
    gWebcamPath.backend = kWcPathGpu;
    LOG(@"webcam: the late Core Image render ended OK after %.0f ms; back to the gpu path%@", ms,
        gWebcam.camera.on && !gWebcamPath.ciContext ? @" (warming up a new context)" : @"");
    // The source was left (its context released) and re-applied meanwhile; otherwise the next apply warms up.
    if (gWebcam.camera.on && !gWebcamPath.ciContext) {
        WebcamPathWarm();
    }
}

// Pump queue: the render just given up on. YES: wait for its completion, then gpu again; NO: cg for good.
static BOOL WebcamGpuRenderTimedOut(void) {
    uint64_t now = WebcamNowNs();
    gWebcamPath.lateRender.timesNs[gWebcamPath.lateRender.count++ % kWcGpuMaxLate] = now;
    // the oldest of the last kWcGpuMaxLate timeouts (this one included)
    uint64_t oldest = gWebcamPath.lateRender.timesNs[gWebcamPath.lateRender.count % kWcGpuMaxLate];
    BOOL capReached = gWebcamPath.lateRender.count >= kWcGpuMaxLate && now - oldest < kWcGpuLateWindowNs;
    LOG(@"webcam: a Core Image render took over %d ms (%ld so far); %@", kWcGpuWaitMs, gWebcamPath.lateRender.count,
        capReached ? [NSString stringWithFormat:@"%d within %.0f s: the CG canvas for the rest of the process",
                                                kWcGpuMaxLate, kWcGpuLateWindowNs / 1e9]
                   : @"the CG canvas until that render ends, then the gpu path again if it ended OK");
    gWebcamPath.lateRender.running = !capReached;
    return !capReached;
}

static BOOL WebcamRenderGpu(CVPixelBufferRef src, WebcamOutputOp op, CVPixelBufferRef dst) {
    CIContext *context = gWebcamPath.ciContext;

    // The render is set up and completes on gWebcamPath.gpuWaitQueue. Set-up means building the graph and Core Image's
    // synchronous startTaskToRender, which took up to 421 ms on a loaded Mac.
    // The pump queue waits at most kWcGpuWaitMs for both. A task that can't be started is a failed render.
    // The block holds src, dst, the task and its context until the render is over. So a render given up on still
    // writes into a buffer that no pool hands out meanwhile. That buffer also outlives the pools released when the
    // source is left, because a pool's buffers keep it alive.
    // Whoever sets `outcome` first decides what happens to dst:
    //   1, the block: the render was in time, and the pump uses dst.
    //   2, the pump: the render is late. dst is dropped, and the block reports the completion to the pump queue
    //      (WebcamGpuLateRenderEnded).
    // Each buffer is retained once here and released once in the block.
    if (!gWebcamPath.gpuWaitQueue) {
        gWebcamPath.gpuWaitQueue = dispatch_queue_create("simcam.webcam.gpuwait", DISPATCH_QUEUE_SERIAL);
    }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block BOOL ok = NO;
    __block NSError *renderErr = nil;
    __block _Atomic int outcome = 0;
    long stallMs = WebcamTestStallMsForNextRender();
    uint64_t start = WebcamNowNs();
    CVPixelBufferRetain(src);
    CVPixelBufferRetain(dst);
    dispatch_async(gWebcamPath.gpuWaitQueue, ^{
        if (stallMs) {
            usleep((useconds_t)stallMs * 1000);   // test hook only
        }
        CIImage *image;
        CIRenderDestination *dest;
        WebcamImageGraph(src, op, dst, &image, &dest);
        NSError *err = nil;
        CIRenderTask *task = [context startTaskToRender:image toDestination:dest error:&err];
        BOOL rendered = task && [task waitUntilCompletedAndReturnError:&err];
        ok = rendered;
        renderErr = err;
        CVPixelBufferRelease(src);
        CVPixelBufferRelease(dst);
        int expected = 0;
        if (atomic_compare_exchange_strong(&outcome, &expected, 1)) {
            dispatch_semaphore_signal(done);
            return;
        }
        double ms = (WebcamNowNs() - start) / 1e6;
        dispatch_async(gSessionLayer.pumpQueue, ^{ WebcamGpuLateRenderEnded(rendered, ms); });
    });

    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kWcGpuWaitMs * NSEC_PER_MSEC))) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&outcome, &expected, 2)) {
            WebcamGpuRenderTimedOut();
            return NO;
        }
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);   // it ended just now: the signal is on its way
    }

    if (!ok) {
        LOG(@"webcam: Core Image render failed (%@); the frame path falls back to the CG canvas", renderErr);
        return NO;
    }

    return YES;
}

// The rotation session for op's turn and mirror, made on first use. NULL: VideoToolbox couldn't make one.
static VTPixelRotationSessionRef WebcamVtRotationFor(WebcamOutputOp op) {
    int rotIndex = op.turn / 90 + (op.mirror ? kWcTurns : 0);
    if (gWebcamPath.vt.rotations[rotIndex]) {
        return gWebcamPath.vt.rotations[rotIndex];
    }
    CFStringRef rotations[] = {kVTRotation_0, kVTRotation_CW90, kVTRotation_180, kVTRotation_CCW90};
    VTPixelRotationSessionRef rotation = NULL;
    if (VTPixelRotationSessionCreate(NULL, &rotation) != noErr || !rotation) {
        LOG(@"webcam: VTPixelRotationSessionCreate failed");
        return NULL;
    }
    // every rotation session supports these two properties and values, so setting them can't fail
    (void)VTSessionSetProperty(rotation, kVTPixelRotationPropertyKey_Rotation, rotations[op.turn / 90]);
    if (op.mirror) {
        (void)VTSessionSetProperty(rotation, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
    }
    gWebcamPath.vt.rotations[rotIndex] = rotation;
    return rotation;
}

// The transfer session that crops to the clean aperture and scales, made on first use. NULL: VideoToolbox couldn't
// make one.
static VTPixelTransferSessionRef WebcamVtTransfer(void) {
    if (gWebcamPath.vt.transfer) {
        return gWebcamPath.vt.transfer;
    }
    if (VTPixelTransferSessionCreate(NULL, &gWebcamPath.vt.transfer) != noErr || !gWebcamPath.vt.transfer) {
        gWebcamPath.vt.transfer = NULL;
        LOG(@"webcam: VTPixelTransferSessionCreate failed");
        return NULL;
    }
    // a transfer session supports this property and value, so setting it can't fail
    (void)VTSessionSetProperty(gWebcamPath.vt.transfer, kVTPixelTransferPropertyKey_ScalingMode,
                               kVTScalingMode_CropSourceToCleanAperture);
    return gWebcamPath.vt.transfer;
}

// The vt backend: turn (and mirror) into a ring-sized buffer, then crop to the output's aspect and scale into dst.
static BOOL WebcamRenderVideoToolbox(CVPixelBufferRef src, WebcamOutputOp op, CVPixelBufferRef dst) {
    int swap = op.turn % 180 == 90;
    int srcWidth = (int)CVPixelBufferGetWidth(src);
    int srcHeight = (int)CVPixelBufferGetHeight(src);
    int turnedWidth = swap ? srcHeight : srcWidth;
    int turnedHeight = swap ? srcWidth : srcHeight;

    VTPixelRotationSessionRef rotation = WebcamVtRotationFor(op);
    if (!rotation) {
        return NO;
    }
    VTPixelTransferSessionRef transfer = WebcamVtTransfer();
    if (!transfer) {
        return NO;
    }

    CVPixelBufferRef turned = WebcamPoolBuffer(&gWebcamPath.vt.turnedPool[swap], &gWebcamPath.vt.turnedPoolWidth[swap],
                                               &gWebcamPath.vt.turnedPoolHeight[swap], NULL, turnedWidth, turnedHeight,
                                               kCVPixelFormatType_32BGRA);
    if (!turned) {
        return NO;
    }
    OSStatus status = VTPixelRotationSessionRotateImage(rotation, src, turned);
    double cropWidth;
    double cropHeight;

    // `turned` is already turned: crop it to the output's own aspect
    WebcamCropSize(turnedWidth, turnedHeight, op.width, op.height, &cropWidth, &cropHeight);

    if (!status) {
        // a clean aperture only when the crop is smaller than the turned frame by at least half a pixel
        if (cropWidth < turnedWidth - 0.5 || cropHeight < turnedHeight - 0.5) {
            CVBufferSetAttachment(turned, kCVImageBufferCleanApertureKey, (__bridge CFDictionaryRef)@{
                                      (id)kCVImageBufferCleanApertureWidthKey: @(cropWidth),
                                      (id)kCVImageBufferCleanApertureHeightKey: @(cropHeight),
                                      (id)kCVImageBufferCleanApertureHorizontalOffsetKey: @0,
                                      (id)kCVImageBufferCleanApertureVerticalOffsetKey: @0},
                                  kCVAttachmentMode_ShouldNotPropagate);
        } else {
            CVBufferRemoveAttachment(turned, kCVImageBufferCleanApertureKey);
        }
        status = VTPixelTransferSessionTransferImage(transfer, turned, dst);
    }

    CVPixelBufferRelease(turned);

    if (status) {
        LOG(@"webcam: VideoToolbox frame path failed (%d); falling back to the CG canvas", (int)status);
        return NO;
    }

    return YES;
}

// One output from the ring frame, into the cache's pool. The identity (same size, BGRA, no turn) is the frame itself.
// NULL: the backend failed or timed out (the path is then cg, and scene.inc draws from a CGImage of the last frame).
static CVPixelBufferRef WebcamRenderOutput(CVPixelBufferRef src, WebcamOutputOp op, WebcamOutputCache *cache) {
    BOOL identity = !op.turn && !op.mirror && op.format == kCVPixelFormatType_32BGRA &&
                    op.width == (int)CVPixelBufferGetWidth(src) && op.height == (int)CVPixelBufferGetHeight(src);
    if (identity) {
        return CVPixelBufferRetain(src);
    }
    uint64_t start = WebcamNowNs();
    CVPixelBufferRef dst = WebcamPoolBuffer(&cache->pool, &cache->poolWidth, &cache->poolHeight, &cache->poolFormat,
                                            op.width, op.height, op.format);
    if (dst && gWebcamPath.backend > kWcPathCg) {
        BOOL rendered = gWebcamPath.backend == kWcPathGpu ? WebcamRenderGpu(src, op, dst)
                                                          : WebcamRenderVideoToolbox(src, op, dst);
        if (rendered) {
            double ms = (WebcamNowNs() - start) / 1e6;
            gWebcam.stats.renderMs += ms;
            gWebcam.stats.renders++;
            static int slowLogged;
            if (ms > kWcSlowRenderMs && slowLogged < kWcSlowRenderLogs) {
                slowLogged++;
                LOG(@"webcam: slow render %.1f ms (%dx%d '%.4s' turn %d mirror %d)", ms, op.width, op.height,
                    (char *)&(OSType){CFSwapInt32HostToBig(op.format)}, op.turn, op.mirror);
            }
            return dst;
        }
        // Straight to the CG canvas, never from gpu to vt: VideoToolbox is software here and costs more than CG.
        // (After a timeout, WebcamGpuLateRenderEnded may make it gpu again.)
        gWebcamPath.backend = kWcPathCg;
    }

    if (dst) {
        CVPixelBufferRelease(dst);
    }
    return NULL;
}

// An op as one number, for WebcamOutputCache.opKey. The width and height are below 16384 (rings and formats are at most
// 8192).
static uint64_t WebcamOutputOpKey(WebcamOutputOp op) {
    return ((uint64_t)op.format << 32) | ((uint64_t)(op.turn / 90) << 30) | ((uint64_t)(op.mirror & 1) << 29) |
           ((uint64_t)(op.width & 0x3fff) << 14) | (uint64_t)(op.height & 0x3fff);
}

static void WebcamOutputCacheClear(WebcamOutputCache *cache, BOOL freePool) {
    if (cache->lastOutput) {
        CVPixelBufferRelease(cache->lastOutput);
    }
    cache->lastOutput = NULL;
    cache->frameGen = 0;
    cache->opKey = 0;
    if (freePool && cache->pool) {
        CVPixelBufferPoolRelease(cache->pool);
        cache->pool = NULL;
    }
}

static void WebcamOutputCacheFree(void *cache) {   // SimcamSink / SimcamRecording dealloc
    if (cache) {
        WebcamOutputCacheClear((WebcamOutputCache *)cache, YES);
        free(cache);
    }
}

static WebcamOutputCache *WebcamOutputCacheFor(void **slot) {
    if (!*slot) {
        *slot = calloc(1, sizeof(WebcamOutputCache));
    }
    return *slot;
}

// The newest frame through op (+1): the buffer already made for it when the frame and op are the same (the frame shown
// again).
static CVPixelBufferRef WebcamCachedOutput(WebcamOutputCache *cache, WebcamOutputOp op) {
    uint64_t key = WebcamOutputOpKey(op);
    BOOL sameFrameAndOp = cache->lastOutput && cache->frameGen == gWebcam.frame.bufferGen && cache->opKey == key;
    if (sameFrameAndOp) {
        return CVPixelBufferRetain(cache->lastOutput);
    }
    WebcamOutputCacheClear(cache, NO);
    CVPixelBufferRef out = WebcamRenderOutput(gWebcam.frame.buffer, op, cache);
    if (out) {
        cache->lastOutput = CVPixelBufferRetain(out);
        cache->frameGen = gWebcam.frame.bufferGen;
        cache->opKey = key;
    }
    return out;
}

// The fallback's picture once the frame path is gone: the last frame as a CGImage.
static void WebcamFallBackToCG(void) {
    if (!gWebcam.frame.buffer) {
        return;
    }
    // a failed conversion leaves no image: this tick shows the scene, and the next frame is read as a CGImage
    CGImageRef image = NULL;
    (void)VTCreateCGImageFromCVPixelBuffer(gWebcam.frame.buffer, NULL, &image);
    if (gWebcam.frame.image) {
        CGImageRelease(gWebcam.frame.image);
    }
    gWebcam.frame.image = image;
    CVPixelBufferRelease(gWebcam.frame.buffer);
    gWebcam.frame.buffer = NULL;
}

// The tick's source buffer for a live frame (+1): the canvas. NULL means use scene.inc's CG canvas; gWebcam.frame.image
// is then set.
static CVPixelBufferRef WebcamCanvas(SimcamSession *sess, CVPixelBufferRef frame, int angle) {
    gWebcamPath.canvas.direct = NO;
    int frameWidth = (int)CVPixelBufferGetWidth(frame);
    int frameHeight = (int)CVPixelBufferGetHeight(frame);
    double formatWidth = sess && sess.formatWidth > 0 ? sess.formatWidth : kSrcW;
    double formatHeight = sess && sess.formatHeight > 0 ? sess.formatHeight : kSrcH;
    double cropWidth;
    double cropHeight;

    // the active format's aspect at the ring's size
    WebcamCropSize(frameWidth, frameHeight, formatWidth, formatHeight, &cropWidth, &cropHeight);
    int captureAngle = ((angle % 360) + 360) % 360;
    WebcamOutputOp op = { (360 - captureAngle) % 360, 0, (int)lround(cropWidth), (int)lround(cropHeight),
                kCVPixelFormatType_32BGRA };
    CVPixelBufferRef canvas = WebcamCachedOutput(&gWebcamPath.canvas.cache, op);   // frame is gWebcam.frame.buffer
    if (!canvas) {
        WebcamFallBackToCG();
        return NULL;
    }

    gWebcamPath.canvas.op = op;
    gWebcamPath.canvas.direct = YES;
    gWebcamPath.canvas.angle = captureAngle;
    return canvas;
}

// A video data or preview sink's buffer straight from the ring frame (+1); NULL: the sink converts the canvas as usual.
static CVPixelBufferRef WebcamSinkOutput(SimcamSink *sink, BOOL preview) {
    if (!gWebcam.camera.on && sink.wcOut) {   // the source was left: the sink's pool goes too
        WebcamOutputCacheFree(sink.wcOut);
        sink.wcOut = NULL;
    }

    // SIMCAM_OFF_SENSOR_LAYOUT: the old sensor layout (video.inc)
    if (!gWebcamPath.canvas.direct || !gWebcam.frame.buffer || !SensorLayoutOn()) {
        return NULL;
    }
    WebcamOutputOp op = { 0, 0, sink.width, sink.height, sink.pixelFormat };

    if (preview) {
        // MirrorFlip after the canvas: the vertical flip when its angle is 90 or 270, else the horizontal one
        op.turn = (360 - gWebcamPath.canvas.angle) % 360;
        op.format = kCVPixelFormatType_32BGRA;
        NSArray *note = OrientVirtualOn() ? PreviewNoteFor(sink.sinkID) : nil;
        if ([note[1] boolValue]) {
            int flipAngle = (([note[0] intValue] % 180) + 180) % 180;
            op.mirror = 1;
            if (flipAngle == 90) {
                op.turn = (op.turn + 180) % 360;
            }
        }
    } else {
        // SinkRotate after the canvas: turn by the connection's rotation, then mirror
        // (SIMCAM_OFF_DATA_OUTPUT_ROTATION: neither)
        int connectionRotation = DataOutputRotationOn() ? ((sink.dataOutputRotationDeg % 360) + 360) % 360 : 0;
        if (connectionRotation % 90) {
            connectionRotation = 0;
        }
        op.turn = (connectionRotation - gWebcamPath.canvas.angle + 360) % 360;
        op.mirror = DataOutputRotationOn() ? sink.mirrored : 0;
    }

    WebcamOutputOp canvasOp = gWebcamPath.canvas.op;

    // the same picture as the canvas (e.g. a portrait app's preview): shared, read-only
    BOOL sameAsCanvas = op.turn == canvasOp.turn && op.mirror == canvasOp.mirror && op.width == canvasOp.width &&
                        op.height == canvasOp.height && op.format == canvasOp.format;
    if (sameAsCanvas) {
        return CVPixelBufferRetain(gWebcamPath.canvas.cache.lastOutput);
    }
    void *slot = sink.wcOut;
    WebcamOutputCache *cache = WebcamOutputCacheFor(&slot);
    sink.wcOut = slot;
    return cache ? WebcamCachedOutput(cache, op) : NULL;
}

// A movie frame straight from the ring frame (+1), for a recording whose size or format differs from the canvas
// buffer, when the aspects match (it is then the canvas's picture at the movie's size). NULL: audio.inc converts the
// canvas.
static CVPixelBufferRef WebcamMovieOutput(SimcamRecording *recording, CVPixelBufferRef canvas) {
    if (!gWebcamPath.canvas.direct || !gWebcam.frame.buffer || !canvas) {
        return NULL;
    }
    if (canvas != gWebcamPath.canvas.cache.lastOutput) {
        return NULL;
    }
    int canvasWidth = (int)CVPixelBufferGetWidth(canvas);
    int canvasHeight = (int)CVPixelBufferGetHeight(canvas);

    // the canvas itself
    if (!recording.converter && canvasWidth == recording.width && canvasHeight == recording.height) {
        return NULL;
    }

    // another aspect: SinkConvert stretches, as for the scene
    BOOL otherAspect = fabs((double)canvasWidth * recording.height - (double)canvasHeight * recording.width) >
                       kWcAspectTolerance * canvasWidth * recording.height;
    if (otherAspect) {
        return NULL;
    }
    WebcamOutputOp op = gWebcamPath.canvas.op;
    op.width = recording.width;
    op.height = recording.height;
    op.format = recording.converter ? recording.converter.pixelFormat : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    void *slot = recording.wcOut;
    WebcamOutputCache *cache = WebcamOutputCacheFor(&slot);
    recording.wcOut = slot;
    return cache ? WebcamCachedOutput(cache, op) : NULL;
}

static void WebcamPathReset(void) {   // the source was left or re-applied
    gWebcamPath.canvas.direct = NO;
    WebcamOutputCacheClear(&gWebcamPath.canvas.cache, NO);
}

// The source was left: free the pools, the VT sessions and the GPU context (70-130 MB at 4K).
static void WebcamPathFree(void) {
    WebcamPathReset();
    WebcamOutputCacheClear(&gWebcamPath.canvas.cache, YES);

    if (gWebcamPath.framePool.pool) {
        CVPixelBufferPoolRelease(gWebcamPath.framePool.pool);
        gWebcamPath.framePool.pool = NULL;
    }

    for (int i = 0; i < 2; i++) {
        if (gWebcamPath.vt.turnedPool[i]) {
            CVPixelBufferPoolRelease(gWebcamPath.vt.turnedPool[i]);
            gWebcamPath.vt.turnedPool[i] = NULL;
        }
    }

    for (int i = 0; i < kWcRotationSessions; i++) {
        if (gWebcamPath.vt.rotations[i]) {
            VTPixelRotationSessionInvalidate(gWebcamPath.vt.rotations[i]);
            CFRelease(gWebcamPath.vt.rotations[i]);
            gWebcamPath.vt.rotations[i] = NULL;
        }
    }

    if (gWebcamPath.vt.transfer) {
        VTPixelTransferSessionInvalidate(gWebcamPath.vt.transfer);
        CFRelease(gWebcamPath.vt.transfer);
        gWebcamPath.vt.transfer = NULL;
    }

    gWebcamPath.ciContext = nil;   // a render still in flight holds its own reference
}
