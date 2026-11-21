// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included by providers/webcam.inc): the webcam source's frame path.
// ============================================================================
// While `webcam` is the source and live, each new ring frame is copied once, under the seqlock, into a buffer from a
// ring-sized pool, and every output is made straight from that buffer in one pass: cropped to the output's aspect
// (aspect fill, centred), scaled, turned and mirrored into the output's size and pixel format.
//   canvas   what the consumers of the tick's source buffer take (metadata, photo, movie): the frame cropped to the
//            active format's aspect at the ring's size, turned by the capture angle like the scene's sensor buffer
//            (content upright once the app applies the capture angle). The plain landscape case is the frame itself.
//   vdo      video data sinks: the frame turned by (connection rotation - capture angle), mirrored when the connection
//            mirrors, in the sink's size and pixel format (the canvas plus SinkRotate and SinkConvertScale in one pass)
//   preview  the preview sink's size, mirrored as MirrorFlip mirrors the front preview
//   movie    a recording whose size or format differs from the canvas's (a 1080p ring at a 4K format), when the aspects
//            match: the canvas's picture at the movie's size in its format (what SinkConvert does on the CPU)
// Every output is kept per sink (and per recording) while the same ring frame is shown again (60 fps from a 30 fps
// camera), so the second tick of a frame renders nothing.
// Backends:
//   gpu  (default) Core Image on the Metal device (the simulator's GPU), warmed up off the pump queue (WcWarm);
//        ticks before that use the CG canvas. The pump queue waits at most 250 ms for a render (a stalled GPU must
//        not hold it: sess_SetConfiguration syncs to it from the app's thread; the mechanics are at WcGpu). A
//        render over 250 ms is dropped (never delivered or cached) and the path is cg until that render completes,
//        then gpu again if it ended OK (a cold kernel compile on a loaded Mac is one slow render, not a broken GPU).
//        The third timeout within 30 s, or a render that ends in an error, makes it cg for good. No gpu render starts
//        while one is late, so at most one late render (its buffers, task and context) is held at any time, however
//        often the source is left and re-applied.
//   vt   only with SIMCAM_WEBCAM_PATH=vt: VTPixelRotation, then VTPixelTransfer cropping to the clean aperture. In the
//        simulator VideoToolbox scales on the CPU (5-17 ms per scaled 1080p-4K output; 42-122 % app CPU in
//        measured cases), so it is never a fallback.
//   cg   scene.inc's CG canvas, the scene/image/video path (25-62 % app CPU in the same cases). Unlike the others it
//        doesn't crop to a 4:3 format: the canvas is 16:9 and the sinks stretch it into 4:3 outputs.
// SIMCAM_WEBCAM_PATH=gpu|vt|cg picks one. A gpu or vt failure logs once and moves to cg for the rest of the process; a
// gpu render over 250 ms as above. Scene, image and video never reach this file. Pump queue only, except the waits and
// the warm-up. Test hook: SIMCAM_TEST_WEBCAM_GPU_STALL=<n>:<ms>[:<count>] delays the completion of the gpu renders from
// the n-th on by <ms> (all of them, or <count>).
// ============================================================================
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
static NSArray *PreviewNoteFor(NSString *sinkID);   // mirror.inc
static BOOL OrientVirtualOn(void);

typedef struct { int r, m, w, h; OSType fmt; } WcOp;   // r: degrees clockwise, m: mirror after the turn (VT's order)
// An output kept for the ring frame it was made from: its pool, and the last buffer with its frame count and op key.
typedef struct {
    CVPixelBufferPoolRef pool;
    int pw, ph;
    OSType pf;
    CVPixelBufferRef last;
    uint64_t gen, key;
} WcOutCache;
// the values of gWcPath: the backend in use (kWcPathCg is scene.inc's canvas)
enum { kWcPathUnchosen = -1, kWcPathCg = 0, kWcPathVt = 1, kWcPathGpu = 2 };
static int gWcPath = kWcPathUnchosen;
static CIContext *gWcCI;                 // gpu: the context, once WcWarm has made it
static BOOL gWcWarming;
static dispatch_queue_t gWcWaitQ;
static long gWcStallAt = -1, gWcStallMs, gWcStallN = -1, gWcGpuN;   // test hook
#define kWcGpuWaitMs 250                 // the longest the pump queue waits for a gpu render
#define kWcGpuMaxLate 3                  // this many renders over kWcGpuWaitMs within kWcGpuLateWinNs: cg for good
#define kWcGpuLateWinNs (30 * NSEC_PER_SEC)
static BOOL gWcGpuLate;                  // a gpu render timed out and is still running: cg until it ends
static uint64_t gWcGpuLateAt[kWcGpuMaxLate];   // when the last timeouts were (a ring)
static long gWcGpuLateN;
static CVPixelBufferPoolRef gWcFPool;    // ring frames
static int gWcFPW, gWcFPH;
static WcOutCache gWcCanvasC;             // the canvas, of frame gWcPBGen
static WcOp gWcCanvasOp;
static BOOL gWcDirect;                   // this tick's canvas came from the frame path: sinks render from gWcPB
static int gWcAng;
static VTPixelRotationSessionRef gWcRot[8];
static VTPixelTransferSessionRef gWcXfer;
static CVPixelBufferPoolRef gWcRPool[2]; // VT: the turned frame (as is / swapped)
static int gWcRPW[2], gWcRPH[2];
static double gWcRenderMs;
static long gWcRenders;

static const char *WcPathName(int p) {
    return p == kWcPathGpu ? "gpu" : p == kWcPathVt ? "vt" : p == kWcPathCg && gWcGpuLate ? "cg-late" : "cg";
}
static void WcPathChoose(void) {
    if (gWcPath >= 0) return;
    const char *e = getenv(ENV_WEBCAM_PATH);
    gWcPath = e && !strcmp(e, "cg") ? kWcPathCg : e && !strcmp(e, "vt") ? kWcPathVt : kWcPathGpu;
    const char *stall = getenv(ENV_TEST_WEBCAM_GPU_STALL);
    if (stall && strchr(stall, ':')) {
        const char *colon2 = strchr(strchr(stall, ':') + 1, ':');
        gWcStallAt = atol(stall);
        gWcStallMs = atol(strchr(stall, ':') + 1);
        gWcStallN = colon2 ? atol(colon2 + 1) : -1;
        LOG(@"webcam: test hook: gpu renders from #%ld complete %ld ms late (%@)", gWcStallAt, gWcStallMs,
            gWcStallN < 0 ? @"all" : [NSString stringWithFormat:@"%ld of them", gWcStallN]);
    }
    if (gWcPath < kWcPathGpu) LOG(@"webcam: frame path %s%s", WcPathName(gWcPath), e ? " (SIMCAM_WEBCAM_PATH)" : "");
}
static BOOL WcPathOn(void) {
    WcPathChoose();
    return gWcPath == kWcPathVt || (gWcPath == kWcPathGpu && gWcCI);
}

// The Core Image graph for one output (also the warm-up's).
static void WcCrop(double w, double h, double aw, double ah, double *cw, double *ch);
static void WcGraph(CVPixelBufferRef src, WcOp op, CVPixelBufferRef dst, CIImage **imgOut,
                       CIRenderDestination **dOut) {
    double W = CVPixelBufferGetWidth(src), H = CVPixelBufferGetHeight(src), ow = op.w, oh = op.h, cw, ch;
    BOOL swap = op.r % 180 == 90;
    double uw = swap ? oh : ow, uh = swap ? ow : oh;   // the output's size before the turn
    WcCrop(W, H, uw, uh, &cw, &ch);
    // Core Image space: origin bottom-left, y up; a clockwise turn on screen is a negative angle
    CGAffineTransform t = CGAffineTransformMakeTranslation(-W / 2, -H / 2);
    t = CGAffineTransformConcat(t, CGAffineTransformMakeScale(uw / cw, uh / ch));
    t = CGAffineTransformConcat(t, CGAffineTransformMakeRotation(-op.r * M_PI / 180));
    if (op.m) t = CGAffineTransformConcat(t, CGAffineTransformMakeScale(-1, 1));
    t = CGAffineTransformConcat(t, CGAffineTransformMakeTranslation(ow / 2, oh / 2));
    CIImage *img = [CIImage imageWithCVPixelBuffer:src options:@{kCIImageColorSpace: [NSNull null]}];
    *imgOut = [[[img imageByClampingToExtent] imageByApplyingTransform:t]
        imageByCroppingToRect:CGRectMake(0, 0, ow, oh)];
    CIRenderDestination *d = [[CIRenderDestination alloc] initWithPixelBuffer:dst];
    d.colorSpace = NULL;
    *dOut = d;
}
// Pump queue, from WcApply: the Metal device, the context and one small render of each kind (the kernel compiles),
// on a background queue, so the first frames don't wait for them on the pump queue.
static void WcWarm(void) {
    WcPathChoose();
    if (gWcPath != kWcPathGpu || gWcCI || gWcWarming) return;
    gWcWarming = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint64_t t0 = WcNowNs();
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        CIContext *ci = dev ? [CIContext contextWithMTLDevice:dev options:@{
                                  kCIContextWorkingColorSpace: [NSNull null], kCIContextOutputColorSpace: [NSNull null],
                                  kCIContextCacheIntermediates: @NO, kCIContextName: @"simcam.webcam"}]
                            : nil;
        if (ci) {
            NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                                    (id)kCVPixelBufferMetalCompatibilityKey: @YES};
            // each output format through every turn, with and without a mirror, and a plain scale with and without the
            // 4:3 crop (Core Image builds a kernel per shape; a cold one cost 75-182 ms on the pump queue)
            OSType fmts[3] = {kCVPixelFormatType_32BGRA, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                              kCVPixelFormatType_420YpCbCr8BiPlanarFullRange};
            static const int sh[9][4] = {{0, 0, 48, 36},   {0, 0, 32, 18},   {0, 1, 32, 18},
                                         {90, 0, 18, 32},  {90, 1, 36, 64},  {180, 0, 32, 18},
                                         {180, 1, 32, 18}, {270, 0, 27, 36}, {270, 1, 27, 36}};
            enum { kN = 3 * 9 };
            WcOp ops[kN];
            for (int i = 0; i < kN; i++)
                ops[i] = (WcOp){sh[i % 9][0], sh[i % 9][1], sh[i % 9][2], sh[i % 9][3], fmts[i / 9]};
            CVPixelBufferRef src = NULL;
            CVPixelBufferCreate(NULL, 64, 36, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &src);
            for (int i = 0; src && i < kN; i++) {
                CVPixelBufferRef out = NULL;
                CVPixelBufferCreate(NULL, ops[i].w, ops[i].h, ops[i].fmt, (__bridge CFDictionaryRef)attrs, &out);
                if (!out) continue;
                CIImage *img;
                CIRenderDestination *d;
                WcGraph(src, ops[i], out, &img, &d);
                [[ci startTaskToRender:img toDestination:d error:nil] waitUntilCompletedAndReturnError:nil];
                CVPixelBufferRelease(out);
            }
            if (src) CVPixelBufferRelease(src);
        }
        NSString *name = dev.name;
        double ms = (WcNowNs() - t0) / 1e6;
        dispatch_async(gPumpQ, ^{
            gWcWarming = NO;
            if (gWcPath != kWcPathGpu) return;
            if (!ci) {
                gWcPath = kWcPathCg;
                LOG(@"webcam: no Metal device for the frame path; using the CG canvas");
                return;
            }
            if (!gWcOn) return;   // left meanwhile: the next apply warms up again
            gWcCI = ci;
            LOG(@"webcam: frame path gpu (Core Image on %@; device, context and first renders %.1f ms, "
                @"off the pump queue)", name, ms);
        });
    });
}

// a buffer from *pool, remade when the size or format changes; IOSurface-backed and Metal-compatible
static CVPixelBufferRef WcPoolBuf(CVPixelBufferPoolRef *pool, int *pw, int *ph, OSType *pf, int w, int h, OSType f) {
    if (*pool && (*pw != w || *ph != h || (pf && *pf != f))) {
        CVPixelBufferPoolRelease(*pool);
        *pool = NULL;
    }
    if (!*pool) {
        NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(f), (id)kCVPixelBufferWidthKey: @(w),
                                (id)kCVPixelBufferHeightKey: @(h), (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                                (id)kCVPixelBufferMetalCompatibilityKey: @YES};
        if (CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, pool) != kCVReturnSuccess) {
            *pool = NULL;
            return NULL;
        }
        *pw = w;
        *ph = h;
        if (pf) *pf = f;
    }
    CVPixelBufferRef b = NULL;
    CVPixelBufferPoolCreatePixelBuffer(NULL, *pool, &b);
    // VTPixelTransfer tags its YCbCr output 709 (and converts with it): the same here
    if (b && f != kCVPixelFormatType_32BGRA)
        CVBufferSetAttachment(b, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
    return b;
}

// The newest ring frame into a pooled buffer. SCW_OK: *out holds it (+1). Otherwise as scw_read_latest.
static int WcCopy(uint64_t lastSeq, scw_slot_read *sr, CVPixelBufferRef *out) {
    *out = NULL;
    CVPixelBufferRef b = WcPoolBuf(&gWcFPool, &gWcFPW, &gWcFPH, NULL, (int)gWcR.g.width, (int)gWcR.g.height,
                                      kCVPixelFormatType_32BGRA);
    if (!b) return SCW_E_ARG;
    CVPixelBufferLockBaseAddress(b, 0);
    size_t bpr = CVPixelBufferGetBytesPerRow(b);
    int rc = scw_read_latest(gWcR.map, &gWcR.g, lastSeq, CVPixelBufferGetBaseAddress(b), bpr, bpr * gWcR.g.height, sr);
    CVPixelBufferUnlockBaseAddress(b, 0);
    if (rc == SCW_OK) *out = b;
    else CVPixelBufferRelease(b);
    return rc;
}

// The size of the centred crop of a w x h upright source that fills an output of aspect aw:ah.
static void WcCrop(double w, double h, double aw, double ah, double *cw, double *ch) {
    *cw = w;
    *ch = w * ah / aw;
    if (*ch > h) {
        *ch = h;
        *cw = h * aw / ah;
    }
}
// Pump queue: a render the pump gave up on has ended (ok: without an error), `ms` after it started.
static void WcGpuLateEnd(BOOL ok, double ms) {
    if (!gWcGpuLate) return;   // cg for good meanwhile (the timeout cap): stay there
    gWcGpuLate = NO;
    if (!ok) {
        LOG(@"webcam: the late Core Image render ended in an error after %.0f ms; "
            @"the CG canvas for the rest of the process", ms);
        return;
    }
    gWcPath = kWcPathGpu;
    LOG(@"webcam: the late Core Image render ended OK after %.0f ms; back to the gpu path%@", ms,
        gWcOn && !gWcCI ? @" (warming up a new context)" : @"");
    // the source was left (context released) and re-applied meanwhile; else the next apply warms up
    if (gWcOn && !gWcCI) WcWarm();
}
// Pump queue: the render just given up on. YES: wait for its completion, then gpu again; NO: cg for good.
static BOOL WcGpuTimeout(void) {
    uint64_t now = WcNowNs();
    gWcGpuLateAt[gWcGpuLateN++ % kWcGpuMaxLate] = now;
    // the oldest of the last kWcGpuMaxLate (this one included)
    uint64_t first = gWcGpuLateAt[gWcGpuLateN % kWcGpuMaxLate];
    BOOL cap = gWcGpuLateN >= kWcGpuMaxLate && now - first < kWcGpuLateWinNs;
    LOG(@"webcam: a Core Image render took over %d ms (%ld so far); %@", kWcGpuWaitMs, gWcGpuLateN,
        cap ? [NSString stringWithFormat:@"%d within %.0f s: the CG canvas for the rest of the process", kWcGpuMaxLate,
                                         kWcGpuLateWinNs / 1e9]
            : @"the CG canvas until that render ends, then the gpu path again if it ended OK");
    gWcGpuLate = !cap;
    return !cap;
}
static BOOL WcGpu(CVPixelBufferRef src, WcOp op, CVPixelBufferRef dst) {
    CIContext *ci = gWcCI;
    // The render is set up (the graph and Core Image's synchronous startTaskToRender, which took up to 421 ms on a
    // loaded Mac) and completes on gWcWaitQ, and the pump queue waits for both kWcGpuWaitMs at most; a task that can't
    // be started is a failed render. The block holds src, dst, the task and its context until the render is over, so
    // a render given up on still writes into a buffer that no pool hands out meanwhile, and that outlives the pools
    // released when the source is left (a pool's buffers keep it alive). Who wins `st` decides: 1 the block (in time:
    // the pump uses dst), 2 the pump (late: dst is dropped and the block reports the completion to the pump queue,
    // WcGpuLateEnd). Each buffer is retained once here and released once in the block.
    if (!gWcWaitQ) gWcWaitQ = dispatch_queue_create("simcam.webcam.gpuwait", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL ok = NO;
    __block NSError *renderErr = nil;
    __block _Atomic int st = 0;
    long stallMs = 0;
    if (gWcStallAt >= 0 && ++gWcGpuN >= gWcStallAt && (gWcStallN < 0 || gWcGpuN < gWcStallAt + gWcStallN))
        stallMs = gWcStallMs;
    uint64_t t0 = WcNowNs();
    CVPixelBufferRetain(src);
    CVPixelBufferRetain(dst);
    dispatch_async(gWcWaitQ, ^{
        if (stallMs) usleep((useconds_t)stallMs * 1000);   // test hook only
        CIImage *img;
        CIRenderDestination *d;
        WcGraph(src, op, dst, &img, &d);
        NSError *err = nil;
        CIRenderTask *task = [ci startTaskToRender:img toDestination:d error:&err];
        BOOL rendered = task && [task waitUntilCompletedAndReturnError:&err];
        ok = rendered;
        renderErr = err;
        CVPixelBufferRelease(src);
        CVPixelBufferRelease(dst);
        int expected = 0;
        if (atomic_compare_exchange_strong(&st, &expected, 1)) {
            dispatch_semaphore_signal(sem);
            return;
        }
        double ms = (WcNowNs() - t0) / 1e6;
        dispatch_async(gPumpQ, ^{ WcGpuLateEnd(rendered, ms); });
    });
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, kWcGpuWaitMs * NSEC_PER_MSEC))) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&st, &expected, 2)) {
            WcGpuTimeout();
            return NO;
        }
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);   // it ended just now: the signal is on its way
    }
    if (!ok) {
        LOG(@"webcam: Core Image render failed (%@); the frame path falls back to the CG canvas", renderErr);
        return NO;
    }
    return YES;
}
static BOOL WcVt(CVPixelBufferRef src, WcOp op, CVPixelBufferRef dst) {
    int swap = op.r % 180 == 90, rotIndex = op.r / 90 + (op.m ? 4 : 0);
    int W = (int)CVPixelBufferGetWidth(src), H = (int)CVPixelBufferGetHeight(src), rw = swap ? H : W, rh = swap ? W : H;
    if (!gWcRot[rotIndex]) {
        CFStringRef rots[] = {kVTRotation_0, kVTRotation_CW90, kVTRotation_180, kVTRotation_CCW90};
        VTPixelRotationSessionRef rotSess = NULL;
        if (VTPixelRotationSessionCreate(NULL, &rotSess) != noErr || !rotSess) {
            LOG(@"webcam: VTPixelRotationSessionCreate failed");
            return NO;
        }
        VTSessionSetProperty(rotSess, kVTPixelRotationPropertyKey_Rotation, rots[op.r / 90]);
        if (op.m) VTSessionSetProperty(rotSess, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
        gWcRot[rotIndex] = rotSess;
    }
    if (!gWcXfer) {
        if (VTPixelTransferSessionCreate(NULL, &gWcXfer) != noErr || !gWcXfer) {
            gWcXfer = NULL;
            LOG(@"webcam: VTPixelTransferSessionCreate failed");
            return NO;
        }
        VTSessionSetProperty(gWcXfer, kVTPixelTransferPropertyKey_ScalingMode,
                             kVTScalingMode_CropSourceToCleanAperture);
    }
    CVPixelBufferRef turned = WcPoolBuf(&gWcRPool[swap], &gWcRPW[swap], &gWcRPH[swap], NULL, rw, rh,
                                           kCVPixelFormatType_32BGRA);
    if (!turned) return NO;
    OSStatus e = VTPixelRotationSessionRotateImage(gWcRot[rotIndex], src, turned);
    double cw, ch;
    WcCrop(rw, rh, op.w, op.h, &cw, &ch);   // `turned` is already turned: crop to the output's own aspect
    if (!e) {
        if (cw < rw - 0.5 || ch < rh - 0.5)
            CVBufferSetAttachment(turned, kCVImageBufferCleanApertureKey, (__bridge CFDictionaryRef)@{
                                      (id)kCVImageBufferCleanApertureWidthKey: @(cw),
                                      (id)kCVImageBufferCleanApertureHeightKey: @(ch),
                                      (id)kCVImageBufferCleanApertureHorizontalOffsetKey: @0,
                                      (id)kCVImageBufferCleanApertureVerticalOffsetKey: @0},
                                  kCVAttachmentMode_ShouldNotPropagate);
        else
            CVBufferRemoveAttachment(turned, kCVImageBufferCleanApertureKey);
        e = VTPixelTransferSessionTransferImage(gWcXfer, turned, dst);
    }
    CVPixelBufferRelease(turned);
    if (e) {
        LOG(@"webcam: VideoToolbox frame path failed (%d); falling back to the CG canvas", (int)e);
        return NO;
    }
    return YES;
}
// One output from the ring frame, into c's pool. The identity (same size, BGRA, no turn) is the frame itself. NULL:
// the backend failed or timed out (the path is then cg, and scene.inc draws from a CGImage of the last frame).
static CVPixelBufferRef WcRenderOutput(CVPixelBufferRef src, WcOp op, WcOutCache *c) {
    if (!op.r && !op.m && op.fmt == kCVPixelFormatType_32BGRA && op.w == (int)CVPixelBufferGetWidth(src) &&
        op.h == (int)CVPixelBufferGetHeight(src))
        return CVPixelBufferRetain(src);
    uint64_t t0 = WcNowNs();
    CVPixelBufferRef dst = WcPoolBuf(&c->pool, &c->pw, &c->ph, &c->pf, op.w, op.h, op.fmt);
    if (dst && gWcPath > kWcPathCg) {
        if (gWcPath == kWcPathGpu ? WcGpu(src, op, dst) : WcVt(src, op, dst)) {
            double ms = (WcNowNs() - t0) / 1e6;
            gWcRenderMs += ms;
            gWcRenders++;
            static int slow;
            if (ms > 30 && slow < 8) {
                slow++;
                LOG(@"webcam: slow render %.1f ms (%dx%d '%.4s' turn %d mirror %d)", ms, op.w, op.h,
                    (char *)&(OSType){CFSwapInt32HostToBig(op.fmt)}, op.r, op.m);
            }
            return dst;
        }
        // straight to the CG canvas, never gpu -> vt: VideoToolbox is software here and costs more than CG (after a
        // timeout, WcGpuLateEnd may make it gpu again)
        gWcPath = kWcPathCg;
    }
    if (dst) CVPixelBufferRelease(dst);
    return NULL;
}
static uint64_t WcKey(WcOp op) {   // w, h < 16384 (rings and formats are at most 8192)
    return ((uint64_t)op.fmt << 32) | ((uint64_t)(op.r / 90) << 30) | ((uint64_t)(op.m & 1) << 29) |
           ((uint64_t)(op.w & 0x3fff) << 14) | (uint64_t)(op.h & 0x3fff);
}
static void WcOutClear(WcOutCache *c, BOOL pool) {
    if (c->last) CVPixelBufferRelease(c->last);
    c->last = NULL;
    c->gen = 0;
    c->key = 0;
    if (pool && c->pool) {
        CVPixelBufferPoolRelease(c->pool);
        c->pool = NULL;
    }
}
static void WcOutFree(void *p) {   // SimcamSink / SimcamRecording dealloc
    if (p) {
        WcOutClear((WcOutCache *)p, YES);
        free(p);
    }
}
static WcOutCache *WcOutOf(void **slot) {
    if (!*slot) *slot = calloc(1, sizeof(WcOutCache));
    return *slot;
}
// The newest frame through op (+1): the buffer already made for it when the frame and op are the same (the frame shown
// again).
static CVPixelBufferRef WcCached(WcOutCache *c, WcOp op) {
    uint64_t key = WcKey(op);
    if (c->last && c->gen == gWcPBGen && c->key == key) return CVPixelBufferRetain(c->last);
    WcOutClear(c, NO);
    CVPixelBufferRef out = WcRenderOutput(gWcPB, op, c);
    if (out) {
        c->last = CVPixelBufferRetain(out);
        c->gen = gWcPBGen;
        c->key = key;
    }
    return out;
}

// The fallback's picture once the frame path is gone: the last frame as a CGImage.
static void WcToCG(void) {
    if (!gWcPB) return;
    CGImageRef img = NULL;
    VTCreateCGImageFromCVPixelBuffer(gWcPB, NULL, &img);
    if (gWcFrame) CGImageRelease(gWcFrame);
    gWcFrame = img;
    CVPixelBufferRelease(gWcPB);
    gWcPB = NULL;
}
// The tick's source buffer for a live frame (+1): the canvas. NULL: use scene.inc's CG canvas (gWcFrame is then set).
static CVPixelBufferRef WcCanvas(SimcamSession *S, CVPixelBufferRef frame, int ang) {
    gWcDirect = NO;
    int W = (int)CVPixelBufferGetWidth(frame), H = (int)CVPixelBufferGetHeight(frame);
    double fw = S && S.fmtW > 0 ? S.fmtW : kSrcW, fh = S && S.fmtH > 0 ? S.fmtH : kSrcH, cw, ch;
    WcCrop(W, H, fw, fh, &cw, &ch);   // the active format's aspect at the ring's size
    int a = ((ang % 360) + 360) % 360;
    WcOp op = { (360 - a) % 360, 0, (int)lround(cw), (int)lround(ch), kCVPixelFormatType_32BGRA };
    CVPixelBufferRef canvas = WcCached(&gWcCanvasC, op);   // frame is gWcPB
    if (!canvas) {
        WcToCG();
        return NULL;
    }
    gWcCanvasOp = op;
    gWcDirect = YES;
    gWcAng = a;
    return canvas;
}
// A video data or preview sink's buffer straight from the ring frame (+1); NULL: the sink converts the canvas as usual.
static CVPixelBufferRef WcSinkOut(SimcamSink *k, BOOL preview) {
    if (!gWcOn && k.wcOut) {   // the source was left: the sink's pool goes too
        WcOutFree(k.wcOut);
        k.wcOut = NULL;
    }
    // SIMCAM_OFF_SENSOR_LAYOUT: the old sensor layout (video.inc)
    if (!gWcDirect || !gWcPB || getenv(ENV_OFF_SENSOR_LAYOUT)) return NULL;
    WcOp op = { 0, 0, k.w, k.h, k.fmt };
    if (preview) {
        // MirrorFlip after the canvas: the vertical flip when its angle is 90/270, else the horizontal one
        op.r = (360 - gWcAng) % 360;
        op.fmt = kCVPixelFormatType_32BGRA;
        NSArray *note = OrientVirtualOn() ? PreviewNoteFor(k.sinkID) : nil;
        if ([note[1] boolValue]) {
            int flipAngle = (([note[0] intValue] % 180) + 180) % 180;
            op.m = 1;
            if (flipAngle == 90) op.r = (op.r + 180) % 360;
        }
    } else {
        // SinkRotate after the canvas: turn by the connection's rotation, then mirror
        // (SIMCAM_OFF_DATA_OUTPUT_ROTATION: neither)
        int connRot = getenv(ENV_OFF_DATA_OUTPUT_ROTATION) ? 0 : ((k.vdoRot % 360) + 360) % 360;
        if (connRot % 90) connRot = 0;
        op.r = (connRot - gWcAng + 360) % 360;
        op.m = getenv(ENV_OFF_DATA_OUTPUT_ROTATION) ? 0 : k.mirrored;
    }
    WcOp co = gWcCanvasOp;
    // the same picture as the canvas (e.g. a portrait app's preview): shared, read-only
    if (op.r == co.r && op.m == co.m && op.w == co.w && op.h == co.h && op.fmt == co.fmt)
        return CVPixelBufferRetain(gWcCanvasC.last);
    void *slot = k.wcOut;
    WcOutCache *c = WcOutOf(&slot);
    k.wcOut = slot;
    return c ? WcCached(c, op) : NULL;
}
// A movie frame straight from the ring frame (+1), for a recording whose size or format differs from the canvas `pb`,
// when the aspects match (it is then the canvas's picture at the movie's size). NULL: audio.inc converts the canvas.
static CVPixelBufferRef WcMovieOut(SimcamRecording *r, CVPixelBufferRef pb) {
    if (!gWcDirect || !gWcPB || !pb || pb != gWcCanvasC.last) return NULL;
    int cw = (int)CVPixelBufferGetWidth(pb), ch = (int)CVPixelBufferGetHeight(pb);
    if (!r.conv && cw == r.w && ch == r.h) return NULL;   // the canvas itself
    // another aspect: SinkConvert stretches, as for the scene
    if (fabs((double)cw * r.h - (double)ch * r.w) > 0.005 * cw * r.h) return NULL;
    WcOp op = gWcCanvasOp;
    op.w = r.w;
    op.h = r.h;
    op.fmt = r.conv ? r.conv.fmt : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    void *slot = r.wcOut;
    WcOutCache *c = WcOutOf(&slot);
    r.wcOut = slot;
    return c ? WcCached(c, op) : NULL;
}
static void WcPathReset(void) {   // the source was left or re-applied
    gWcDirect = NO;
    WcOutClear(&gWcCanvasC, NO);
}
// the source was left: the pools, the VT sessions and the GPU context (70-130 MB at 4K)
static void WcPathFree(void) {
    WcPathReset();
    WcOutClear(&gWcCanvasC, YES);
    if (gWcFPool) {
        CVPixelBufferPoolRelease(gWcFPool);
        gWcFPool = NULL;
    }
    for (int i = 0; i < 2; i++)
        if (gWcRPool[i]) {
            CVPixelBufferPoolRelease(gWcRPool[i]);
            gWcRPool[i] = NULL;
        }
    for (int i = 0; i < 8; i++)
        if (gWcRot[i]) {
            VTPixelRotationSessionInvalidate(gWcRot[i]);
            CFRelease(gWcRot[i]);
            gWcRot[i] = NULL;
        }
    if (gWcXfer) {
        VTPixelTransferSessionInvalidate(gWcXfer);
        CFRelease(gWcXfer);
        gWcXfer = NULL;
    }
    gWcCI = nil;   // a render still in flight holds its own reference
}
