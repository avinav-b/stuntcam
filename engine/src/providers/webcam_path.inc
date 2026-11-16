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
//            mirrors, in the sink's size and pixel format (the canvas plus M7Rotate and M5ConvertScale in one pass)
//   preview  the preview sink's size, mirrored as M12Flip mirrors the front preview
//   movie    a recording whose size or format differs from the canvas's (a 1080p ring at a 4K format), when the aspects
//            match: the canvas's picture at the movie's size in its format (what M5Convert did on the CPU)
// Every output is kept per sink (and per recording) while the same ring frame is shown again (60 fps from a 30 fps
// camera), so the second tick of a frame renders nothing.
// Backends:
//   gpu  (default) Core Image on the Metal device (the simulator's GPU). The device, the context and a first render
//        (the kernel compile) are made off the pump queue when the source is applied (M13WcWarm); ticks before that
//        use the CG canvas. Each render is set up and waited for on gWcWaitQ, the pump queue waits at most 250 ms for it (a
//        stalled GPU must not hold the pump queue: sess_SetConfiguration syncs to it from the app's thread), and the
//        wait holds the render's buffers until it ends. A render over 250 ms is dropped (never delivered or cached)
//        and the path is cg until that render's own completion: if it then ends OK, the path is gpu again (a cold
//        kernel compile on a loaded Mac is one slow render, not a broken GPU). The third timeout within 30 s, or a
//        render that ends in an error, makes it cg for good. No gpu render starts while one is late, so at most one
//        late render (its buffers, task and context) is held at any time, however often the source is left and
//        re-applied meanwhile.
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
static NSArray *M12PreviewNote(NSString *sinkID);   // scene.inc
static BOOL M12OrientOn(void);

typedef struct { int r, m, w, h; OSType fmt; } M13WcOp;   // r: degrees clockwise, m: mirror after the turn (VT's order)
// An output kept for the ring frame it was made from: its pool, and the last buffer with its frame count and op key.
typedef struct { CVPixelBufferPoolRef pool; int pw, ph; OSType pf; CVPixelBufferRef last; uint64_t gen, key; } M13WcOutC;
static int gWcPath = -1;                 // 2 gpu, 1 vt, 0 cg (scene.inc's canvas); -1: not chosen yet
static CIContext *gWcCI; static BOOL gWcWarming;   // gpu: the context, once M13WcWarm has made it
static dispatch_queue_t gWcWaitQ;
static long gWcStallAt = -1, gWcStallMs, gWcStallN = -1, gWcGpuN;   // test hook
#define kWcGpuWaitMs 250                 // the longest the pump queue waits for a gpu render
#define kWcGpuMaxLate 3                  // this many renders over kWcGpuWaitMs within kWcGpuLateWinNs: cg for good
#define kWcGpuLateWinNs (30 * NSEC_PER_SEC)
static BOOL gWcGpuLate;                  // a gpu render timed out and is still running: cg until it ends
static uint64_t gWcGpuLateAt[kWcGpuMaxLate]; static long gWcGpuLateN;   // when the last timeouts were (a ring)
static CVPixelBufferPoolRef gWcFPool; static int gWcFPW, gWcFPH;               // ring frames
static M13WcOutC gWcCanvasC;             // the canvas, of frame gWcPBGen
static M13WcOp gWcCanvasOp;
static BOOL gWcDirect; static int gWcAng; // this tick's canvas came from the frame path: sinks render from gWcPB
static VTPixelRotationSessionRef gWcRot[8]; static VTPixelTransferSessionRef gWcXfer;
static CVPixelBufferPoolRef gWcRPool[2]; static int gWcRPW[2], gWcRPH[2];      // VT: the turned frame (as is / swapped)
static double gWcRenderMs; static long gWcRenders;

static const char *M13WcPathName(int p) { return p == 2 ? "gpu" : p == 1 ? "vt" : p == 0 && gWcGpuLate ? "cg-late" : "cg"; }
static void M13WcPathChoose(void) {
    if (gWcPath >= 0) return;
    const char *e = getenv("SIMCAM_WEBCAM_PATH");
    gWcPath = e && !strcmp(e, "cg") ? 0 : e && !strcmp(e, "vt") ? 1 : 2;
    const char *st = getenv("SIMCAM_TEST_WEBCAM_GPU_STALL");
    if (st && strchr(st, ':')) {
        const char *c2 = strchr(strchr(st, ':') + 1, ':');
        gWcStallAt = atol(st); gWcStallMs = atol(strchr(st, ':') + 1); gWcStallN = c2 ? atol(c2 + 1) : -1;
        LOG(@"webcam: test hook: gpu renders from #%ld complete %ld ms late (%@)", gWcStallAt, gWcStallMs, gWcStallN < 0 ? @"all" : [NSString stringWithFormat:@"%ld of them", gWcStallN]);
    }
    if (gWcPath < 2) LOG(@"webcam: frame path %s%s", M13WcPathName(gWcPath), e ? " (SIMCAM_WEBCAM_PATH)" : "");
}
static BOOL M13WcPathOn(void) { M13WcPathChoose(); return gWcPath == 1 || (gWcPath == 2 && gWcCI); }

// The Core Image graph for one output (also the warm-up's).
static void M13WcCrop(double w, double h, double aw, double ah, double *cw, double *ch);
static void M13WcGraph(CVPixelBufferRef src, M13WcOp op, CVPixelBufferRef dst, CIImage **imgOut, CIRenderDestination **dOut) {
    double W = CVPixelBufferGetWidth(src), H = CVPixelBufferGetHeight(src), ow = op.w, oh = op.h, cw, ch;
    BOOL sw = op.r % 180 == 90; double uw = sw ? oh : ow, uh = sw ? ow : oh;   // the output's size before the turn
    M13WcCrop(W, H, uw, uh, &cw, &ch);
    // Core Image space: origin bottom-left, y up; a clockwise turn on screen is a negative angle
    CGAffineTransform t = CGAffineTransformMakeTranslation(-W / 2, -H / 2);
    t = CGAffineTransformConcat(t, CGAffineTransformMakeScale(uw / cw, uh / ch));
    t = CGAffineTransformConcat(t, CGAffineTransformMakeRotation(-op.r * M_PI / 180));
    if (op.m) t = CGAffineTransformConcat(t, CGAffineTransformMakeScale(-1, 1));
    t = CGAffineTransformConcat(t, CGAffineTransformMakeTranslation(ow / 2, oh / 2));
    CIImage *img = [CIImage imageWithCVPixelBuffer:src options:@{kCIImageColorSpace: [NSNull null]}];
    *imgOut = [[[img imageByClampingToExtent] imageByApplyingTransform:t] imageByCroppingToRect:CGRectMake(0, 0, ow, oh)];
    CIRenderDestination *d = [[CIRenderDestination alloc] initWithPixelBuffer:dst]; d.colorSpace = NULL;
    *dOut = d;
}
// Pump queue, from M13WcApply: the Metal device, the context and one small render of each kind (the kernel compiles),
// on a background queue, so the first frames don't wait for them on the pump queue.
static void M13WcWarm(void) {
    M13WcPathChoose();
    if (gWcPath != 2 || gWcCI || gWcWarming) return;
    gWcWarming = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint64_t t0 = M13Ns();
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        CIContext *ci = dev ? [CIContext contextWithMTLDevice:dev options:@{kCIContextWorkingColorSpace: [NSNull null], kCIContextOutputColorSpace: [NSNull null],
                                                                            kCIContextCacheIntermediates: @NO, kCIContextName: @"simcam.webcam"}] : nil;
        if (ci) {
            NSDictionary *a = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES};
            // each output format through every turn, with and without a mirror, and a plain scale with and without the
            // 4:3 crop (Core Image builds a kernel per shape; a cold one cost 75-182 ms on the pump queue)
            OSType fs[3] = {kCVPixelFormatType_32BGRA, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange};
            static const int sh[9][4] = {{0, 0, 48, 36}, {0, 0, 32, 18}, {0, 1, 32, 18}, {90, 0, 18, 32}, {90, 1, 36, 64},
                                         {180, 0, 32, 18}, {180, 1, 32, 18}, {270, 0, 27, 36}, {270, 1, 27, 36}};
            enum { kN = 3 * 9 }; M13WcOp ops[kN];
            for (int i = 0; i < kN; i++) ops[i] = (M13WcOp){sh[i % 9][0], sh[i % 9][1], sh[i % 9][2], sh[i % 9][3], fs[i / 9]};
            CVPixelBufferRef s = NULL; CVPixelBufferCreate(NULL, 64, 36, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)a, &s);
            for (int i = 0; s && i < kN; i++) {
                CVPixelBufferRef o = NULL; CVPixelBufferCreate(NULL, ops[i].w, ops[i].h, ops[i].fmt, (__bridge CFDictionaryRef)a, &o);
                if (!o) continue;
                CIImage *img; CIRenderDestination *d; M13WcGraph(s, ops[i], o, &img, &d);
                [[ci startTaskToRender:img toDestination:d error:nil] waitUntilCompletedAndReturnError:nil];
                CVPixelBufferRelease(o);
            }
            if (s) CVPixelBufferRelease(s);
        }
        NSString *name = dev.name; double ms = (M13Ns() - t0) / 1e6;
        dispatch_async(gPumpQ, ^{
            gWcWarming = NO;
            if (gWcPath != 2) return;
            if (!ci) { gWcPath = 0; LOG(@"webcam: no Metal device for the frame path; using the CG canvas"); return; }
            if (!gWcOn) return;   // left meanwhile: the next apply warms up again
            gWcCI = ci;
            LOG(@"webcam: frame path gpu (Core Image on %@; device, context and first renders %.1f ms, off the pump queue)", name, ms);
        });
    });
}

// a buffer from *pool, remade when the size or format changes; IOSurface-backed and Metal-compatible
static CVPixelBufferRef M13WcPoolBuf(CVPixelBufferPoolRef *pool, int *pw, int *ph, OSType *pf, int w, int h, OSType f) {
    if (*pool && (*pw != w || *ph != h || (pf && *pf != f))) { CVPixelBufferPoolRelease(*pool); *pool = NULL; }
    if (!*pool) {
        NSDictionary *a = @{(id)kCVPixelBufferPixelFormatTypeKey: @(f), (id)kCVPixelBufferWidthKey: @(w), (id)kCVPixelBufferHeightKey: @(h),
                            (id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES};
        if (CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)a, pool) != kCVReturnSuccess) { *pool = NULL; return NULL; }
        *pw = w; *ph = h; if (pf) *pf = f;
    }
    CVPixelBufferRef b = NULL; CVPixelBufferPoolCreatePixelBuffer(NULL, *pool, &b);
    if (b && f != kCVPixelFormatType_32BGRA)   // VTPixelTransfer tags its YCbCr output 709 (and converts with it): the same here
        CVBufferSetAttachment(b, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    return b;
}

// The newest ring frame into a pooled buffer. SCW_OK: *out holds it (+1). Otherwise as scw_read_latest.
static int M13WcCopy(uint64_t lastSeq, scw_slot_read *sr, CVPixelBufferRef *out) {
    *out = NULL;
    CVPixelBufferRef b = M13WcPoolBuf(&gWcFPool, &gWcFPW, &gWcFPH, NULL, (int)gWcR.g.width, (int)gWcR.g.height, kCVPixelFormatType_32BGRA);
    if (!b) return SCW_E_ARG;
    CVPixelBufferLockBaseAddress(b, 0);
    size_t bpr = CVPixelBufferGetBytesPerRow(b);
    int rc = scw_read_latest(gWcR.map, &gWcR.g, lastSeq, CVPixelBufferGetBaseAddress(b), bpr, bpr * gWcR.g.height, sr);
    CVPixelBufferUnlockBaseAddress(b, 0);
    if (rc == SCW_OK) *out = b; else CVPixelBufferRelease(b);
    return rc;
}

// The size of the centred crop of a w x h upright source that fills an output of aspect aw:ah.
static void M13WcCrop(double w, double h, double aw, double ah, double *cw, double *ch) {
    *cw = w; *ch = w * ah / aw;
    if (*ch > h) { *ch = h; *cw = h * aw / ah; }
}
// Pump queue: a render the pump gave up on has ended (ok: without an error), `ms` after it started.
static void M13WcGpuLateEnd(BOOL ok, double ms) {
    if (!gWcGpuLate) return;   // cg for good meanwhile (the timeout cap): stay there
    gWcGpuLate = NO;
    if (!ok) { LOG(@"webcam: the late Core Image render ended in an error after %.0f ms; the CG canvas for the rest of the process", ms); return; }
    gWcPath = 2;
    LOG(@"webcam: the late Core Image render ended OK after %.0f ms; back to the gpu path%@", ms, gWcOn && !gWcCI ? @" (warming up a new context)" : @"");
    if (gWcOn && !gWcCI) M13WcWarm();   // the source was left (context released) and re-applied meanwhile; else the next apply warms up
}
// Pump queue: the render just given up on. YES: wait for its completion, then gpu again; NO: cg for good.
static BOOL M13WcGpuTimeout(void) {
    uint64_t now = M13Ns();
    gWcGpuLateAt[gWcGpuLateN++ % kWcGpuMaxLate] = now;
    uint64_t first = gWcGpuLateAt[gWcGpuLateN % kWcGpuMaxLate];   // the oldest of the last kWcGpuMaxLate (this one included)
    BOOL cap = gWcGpuLateN >= kWcGpuMaxLate && now - first < kWcGpuLateWinNs;
    LOG(@"webcam: a Core Image render took over %d ms (%ld so far); %@", kWcGpuWaitMs, gWcGpuLateN,
        cap ? [NSString stringWithFormat:@"%d within %.0f s: the CG canvas for the rest of the process", kWcGpuMaxLate, kWcGpuLateWinNs / 1e9]
            : @"the CG canvas until that render ends, then the gpu path again if it ended OK");
    gWcGpuLate = !cap;
    return !cap;
}
static BOOL M13WcGpu(CVPixelBufferRef src, M13WcOp op, CVPixelBufferRef dst) {
    CIContext *ci = gWcCI;
    // The render is set up (the graph and Core Image's synchronous startTaskToRender, which took up to 421 ms on a
    // loaded Mac) and completes on gWcWaitQ, and the pump queue waits for both kWcGpuWaitMs at most; a task that can't
    // be started is a failed render. The block holds src, dst, the task and its context until the render is over, so
    // a render given up on still writes into a buffer that no pool hands out meanwhile, and that outlives the pools
    // released when the source is left (a pool's buffers keep it alive). Who wins `st` decides: 1 the block (in time:
    // the pump uses dst), 2 the pump (late: dst is dropped and the block reports the completion to the pump queue,
    // M13WcGpuLateEnd). Each buffer is retained once here and released once in the block.
    if (!gWcWaitQ) gWcWaitQ = dispatch_queue_create("simcam.webcam.gpuwait", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL ok = NO; __block NSError *we = nil; __block _Atomic int st = 0;
    long stallMs = 0;
    if (gWcStallAt >= 0 && ++gWcGpuN >= gWcStallAt && (gWcStallN < 0 || gWcGpuN < gWcStallAt + gWcStallN)) stallMs = gWcStallMs;
    uint64_t t0 = M13Ns();
    CVPixelBufferRetain(src); CVPixelBufferRetain(dst);
    dispatch_async(gWcWaitQ, ^{
        if (stallMs) usleep((useconds_t)stallMs * 1000);   // test hook only
        CIImage *img; CIRenderDestination *d; M13WcGraph(src, op, dst, &img, &d);
        NSError *err = nil; CIRenderTask *task = [ci startTaskToRender:img toDestination:d error:&err];
        BOOL r = task && [task waitUntilCompletedAndReturnError:&err];
        ok = r; we = err;
        CVPixelBufferRelease(src); CVPixelBufferRelease(dst);
        int w = 0;
        if (atomic_compare_exchange_strong(&st, &w, 1)) { dispatch_semaphore_signal(sem); return; }
        double ms = (M13Ns() - t0) / 1e6;
        dispatch_async(gPumpQ, ^{ M13WcGpuLateEnd(r, ms); });
    });
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, kWcGpuWaitMs * NSEC_PER_MSEC))) {
        int w = 0;
        if (atomic_compare_exchange_strong(&st, &w, 2)) { M13WcGpuTimeout(); return NO; }
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);   // it ended just now: the signal is on its way
    }
    if (!ok) { LOG(@"webcam: Core Image render failed (%@); the frame path falls back to the CG canvas", we); return NO; }
    return YES;
}
static BOOL M13WcVt(CVPixelBufferRef src, M13WcOp op, CVPixelBufferRef dst) {
    int sw = op.r % 180 == 90, ri = op.r / 90 + (op.m ? 4 : 0);
    int W = (int)CVPixelBufferGetWidth(src), H = (int)CVPixelBufferGetHeight(src), rw = sw ? H : W, rh = sw ? W : H;
    if (!gWcRot[ri]) {
        CFStringRef rots[] = {kVTRotation_0, kVTRotation_CW90, kVTRotation_180, kVTRotation_CCW90};
        VTPixelRotationSessionRef rs = NULL;
        if (VTPixelRotationSessionCreate(NULL, &rs) != noErr || !rs) { LOG(@"webcam: VTPixelRotationSessionCreate failed"); return NO; }
        VTSessionSetProperty(rs, kVTPixelRotationPropertyKey_Rotation, rots[op.r / 90]);
        if (op.m) VTSessionSetProperty(rs, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
        gWcRot[ri] = rs;
    }
    if (!gWcXfer) {
        if (VTPixelTransferSessionCreate(NULL, &gWcXfer) != noErr || !gWcXfer) { gWcXfer = NULL; LOG(@"webcam: VTPixelTransferSessionCreate failed"); return NO; }
        VTSessionSetProperty(gWcXfer, kVTPixelTransferPropertyKey_ScalingMode, kVTScalingMode_CropSourceToCleanAperture);
    }
    CVPixelBufferRef R = M13WcPoolBuf(&gWcRPool[sw], &gWcRPW[sw], &gWcRPH[sw], NULL, rw, rh, kCVPixelFormatType_32BGRA);
    if (!R) return NO;
    OSStatus e = VTPixelRotationSessionRotateImage(gWcRot[ri], src, R);
    double cw, ch; M13WcCrop(rw, rh, op.w, op.h, &cw, &ch);   // R is already turned: crop to the output's own aspect
    if (!e) {
        if (cw < rw - 0.5 || ch < rh - 0.5)
            CVBufferSetAttachment(R, kCVImageBufferCleanApertureKey, (__bridge CFDictionaryRef)@{(id)kCVImageBufferCleanApertureWidthKey: @(cw),
                (id)kCVImageBufferCleanApertureHeightKey: @(ch), (id)kCVImageBufferCleanApertureHorizontalOffsetKey: @0, (id)kCVImageBufferCleanApertureVerticalOffsetKey: @0},
                kCVAttachmentMode_ShouldNotPropagate);
        else CVBufferRemoveAttachment(R, kCVImageBufferCleanApertureKey);
        e = VTPixelTransferSessionTransferImage(gWcXfer, R, dst);
    }
    CVPixelBufferRelease(R);
    if (e) { LOG(@"webcam: VideoToolbox frame path failed (%d); falling back to the CG canvas", (int)e); return NO; }
    return YES;
}
// One output from the ring frame, into c's pool. The identity (same size, BGRA, no turn) is the frame itself. NULL:
// the backend failed or timed out (the path is then cg, and scene.inc draws from a CGImage of the last frame).
static CVPixelBufferRef M13WcOut(CVPixelBufferRef src, M13WcOp op, M13WcOutC *c) {
    if (!op.r && !op.m && op.fmt == kCVPixelFormatType_32BGRA && op.w == (int)CVPixelBufferGetWidth(src) && op.h == (int)CVPixelBufferGetHeight(src))
        return CVPixelBufferRetain(src);
    uint64_t t0 = M13Ns();
    CVPixelBufferRef dst = M13WcPoolBuf(&c->pool, &c->pw, &c->ph, &c->pf, op.w, op.h, op.fmt);
    if (dst && gWcPath > 0) {
        if (gWcPath == 2 ? M13WcGpu(src, op, dst) : M13WcVt(src, op, dst)) {
            double ms = (M13Ns() - t0) / 1e6; gWcRenderMs += ms; gWcRenders++;
            static int slow; if (ms > 30 && slow < 8) { slow++; LOG(@"webcam: slow render %.1f ms (%dx%d '%.4s' turn %d mirror %d)", ms, op.w, op.h, (char *)&(OSType){CFSwapInt32HostToBig(op.fmt)}, op.r, op.m); }
            return dst;
        }
        gWcPath = 0;   // straight to the CG canvas, never gpu -> vt: VideoToolbox is software here and costs more than CG
                       // (after a timeout, M13WcGpuLateEnd may make it gpu again)
    }
    if (dst) CVPixelBufferRelease(dst);
    return NULL;
}
static uint64_t M13WcKey(M13WcOp op) {   // w, h < 16384 (rings and formats are at most 8192)
    return ((uint64_t)op.fmt << 32) | ((uint64_t)(op.r / 90) << 30) | ((uint64_t)(op.m & 1) << 29) | ((uint64_t)(op.w & 0x3fff) << 14) | (uint64_t)(op.h & 0x3fff);
}
static void M13WcOutClear(M13WcOutC *c, BOOL pool) {
    if (c->last) CVPixelBufferRelease(c->last);
    c->last = NULL; c->gen = 0; c->key = 0;
    if (pool && c->pool) { CVPixelBufferPoolRelease(c->pool); c->pool = NULL; }
}
static void M13WcOutFree(void *p) { if (p) { M13WcOutClear((M13WcOutC *)p, YES); free(p); } }   // M5Sink / M5Rec dealloc
static M13WcOutC *M13WcOutOf(void **slot) { if (!*slot) *slot = calloc(1, sizeof(M13WcOutC)); return *slot; }
// The newest frame through op (+1): the buffer already made for it when the frame and op are the same (the frame shown again).
static CVPixelBufferRef M13WcCached(M13WcOutC *c, M13WcOp op) {
    uint64_t key = M13WcKey(op);
    if (c->last && c->gen == gWcPBGen && c->key == key) return CVPixelBufferRetain(c->last);
    M13WcOutClear(c, NO);
    CVPixelBufferRef out = M13WcOut(gWcPB, op, c);
    if (out) { c->last = CVPixelBufferRetain(out); c->gen = gWcPBGen; c->key = key; }
    return out;
}

// The fallback's picture once the frame path is gone: the last frame as a CGImage.
static void M13WcToCG(void) {
    if (!gWcPB) return;
    CGImageRef img = NULL; VTCreateCGImageFromCVPixelBuffer(gWcPB, NULL, &img);
    if (gWcFrame) CGImageRelease(gWcFrame);
    gWcFrame = img; CVPixelBufferRelease(gWcPB); gWcPB = NULL;
}
// The tick's source buffer for a live frame (+1): the canvas. NULL: use scene.inc's CG canvas (gWcFrame is then set).
static CVPixelBufferRef M13WcCanvas(M5Sess *S, CVPixelBufferRef frame, int ang) {
    gWcDirect = NO;
    int W = (int)CVPixelBufferGetWidth(frame), H = (int)CVPixelBufferGetHeight(frame);
    double fw = S && S.fmtW > 0 ? S.fmtW : kSrcW, fh = S && S.fmtH > 0 ? S.fmtH : kSrcH, cw, ch;
    M13WcCrop(W, H, fw, fh, &cw, &ch);   // the active format's aspect at the ring's size
    int a = ((ang % 360) + 360) % 360;
    M13WcOp op = { (360 - a) % 360, 0, (int)lround(cw), (int)lround(ch), kCVPixelFormatType_32BGRA };
    CVPixelBufferRef c = M13WcCached(&gWcCanvasC, op);   // frame is gWcPB
    if (!c) { M13WcToCG(); return NULL; }
    gWcCanvasOp = op;
    gWcDirect = YES; gWcAng = a;
    return c;
}
// A video data or preview sink's buffer straight from the ring frame (+1); NULL: the sink converts the canvas as usual.
static CVPixelBufferRef M13WcSinkOut(M5Sink *k, BOOL preview) {
    if (!gWcOn && k.wcOut) { M13WcOutFree(k.wcOut); k.wcOut = NULL; }   // the source was left: the sink's pool goes too
    if (!gWcDirect || !gWcPB || getenv("M7_SRC_LEGACY")) return NULL;
    M13WcOp op = { 0, 0, k.w, k.h, k.fmt };
    if (preview) {   // M12Flip after the canvas: the vertical flip when its angle is 90/270, else the horizontal one
        op.r = (360 - gWcAng) % 360; op.fmt = kCVPixelFormatType_32BGRA;
        NSArray *note = M12OrientOn() ? M12PreviewNote(k.sinkID) : nil;
        if ([note[1] boolValue]) { int fa = (([note[0] intValue] % 180) + 180) % 180; op.m = 1; if (fa == 90) op.r = (op.r + 180) % 360; }
    } else {         // M7Rotate after the canvas: turn by the connection's rotation, then mirror
        int r0 = getenv("M7_NO_VDOROT") ? 0 : ((k.vdoRot % 360) + 360) % 360;
        if (r0 % 90) r0 = 0;
        op.r = (r0 - gWcAng + 360) % 360; op.m = getenv("M7_NO_VDOROT") ? 0 : k.mirrored;
    }
    M13WcOp co = gWcCanvasOp;
    if (op.r == co.r && op.m == co.m && op.w == co.w && op.h == co.h && op.fmt == co.fmt)
        return CVPixelBufferRetain(gWcCanvasC.last);   // the same picture as the canvas (e.g. a portrait app's preview): shared, read-only
    void *slot = k.wcOut; M13WcOutC *c = M13WcOutOf(&slot); k.wcOut = slot;
    return c ? M13WcCached(c, op) : NULL;
}
// A movie frame straight from the ring frame (+1), for a recording whose size or format differs from the canvas `pb`,
// when the aspects match (it is then the canvas's picture at the movie's size). NULL: audio.inc converts the canvas.
static CVPixelBufferRef M13WcMovieOut(M5Rec *r, CVPixelBufferRef pb) {
    if (!gWcDirect || !gWcPB || !pb || pb != gWcCanvasC.last) return NULL;
    int cw = (int)CVPixelBufferGetWidth(pb), ch = (int)CVPixelBufferGetHeight(pb);
    if (!r.conv && cw == r.w && ch == r.h) return NULL;   // the canvas itself
    if (fabs((double)cw * r.h - (double)ch * r.w) > 0.005 * cw * r.h) return NULL;   // another aspect: M5Convert stretches, as for the scene
    M13WcOp op = gWcCanvasOp; op.w = r.w; op.h = r.h; op.fmt = r.conv ? r.conv.fmt : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    void *slot = r.wcOut; M13WcOutC *c = M13WcOutOf(&slot); r.wcOut = slot;
    return c ? M13WcCached(c, op) : NULL;
}
static void M13WcPathReset(void) {   // the source was left or re-applied
    gWcDirect = NO;
    M13WcOutClear(&gWcCanvasC, NO);
}
static void M13WcPathFree(void) {    // the source was left: the pools, the VT sessions and the GPU context (70-130 MB at 4K)
    M13WcPathReset();
    M13WcOutClear(&gWcCanvasC, YES);
    if (gWcFPool) { CVPixelBufferPoolRelease(gWcFPool); gWcFPool = NULL; }
    for (int i = 0; i < 2; i++) if (gWcRPool[i]) { CVPixelBufferPoolRelease(gWcRPool[i]); gWcRPool[i] = NULL; }
    for (int i = 0; i < 8; i++) if (gWcRot[i]) { VTPixelRotationSessionInvalidate(gWcRot[i]); CFRelease(gWcRot[i]); gWcRot[i] = NULL; }
    if (gWcXfer) { VTPixelTransferSessionInvalidate(gWcXfer); CFRelease(gWcXfer); gWcXfer = NULL; }
    gWcCI = nil;   // a render still in flight holds its own reference
}
