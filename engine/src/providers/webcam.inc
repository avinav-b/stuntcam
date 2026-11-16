// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): the live Mac webcam source.
// ============================================================================
// Source `webcam` or `webcam:<uniqueID>` (SIMCAM_SOURCE, control file source=): frames from the camera helper
// ("Stuntcam Camera.app", helper/) through the read-only shm ring of docs/webcam-ring.md (webcam/ring.c and
// ring_shm.c, included by simcam.m). `webcam` alone means the camera named in $SIMCAM_HOME/webcam/default. $SIMCAM_HOME
// is that env variable, else the directory above SIMCAM_SIM_DIR's sims/. Pump queue only, except the reader file.
//   live     the ring is running and its newest frame is under 500 ms old. Each tick copies a new complete slot
//            under its seqlock into a pooled ring-sized buffer, and webcam_path.inc makes the canvas and each video
//            data and preview buffer from it in one pass (crop, scale, turn, mirror; on the GPU). If that path is
//            unavailable, scene.inc draws the frame aspect-fill into its canvas, as for a video frame. The slot's
//            capture time is the sample's presentation time (kept monotonic and at most 200 ms behind the tick).
//   offline  the last frame stays for 1 s, then the scene with a "webcam offline" band giving the reason. While offline
//            the shm name is re-opened every second (a restarted helper, an idle quit, a newer ring).
//   phase    the session's pump timer is re-armed so that ticks land 6 ms after the helper publishes a frame, whenever
//            the mean offset of the last 8 frames leaves 4-9 ms (at most once a second). The period follows the camera
//            when the format's rate is within 1 % of the camera's (or of twice it). Leaving the source restores the
//            nominal schedule; other sources never see the lock. Off switch: SIMCAM_WEBCAM_PHASE=0 (free-running pump).
//   late     while locked at 1x or 2x the camera's rate, a tick that finds no new frame although one is due (a camera
//            interval since the last one's publish) polls the ring for up to 5 ms before showing the last frame again:
//            a frame published just after its tick then costs that tick up to 5 ms, not a duplicate plus a skip.
//   reader   $SIMCAM_HOME/webcam/readers/<udid>-<pid> (rings, want, active), rewritten every second while the source is
//            selected and removed when it is left: the helper opens the camera only while some reader is active.
// Stats every 5 s in the engine log ("webcam stat").
// ============================================================================
#import <UIKit/UIKit.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
static const uint32_t kWcRings[] = {1};
static const uint64_t kWcFreezeNs = 1000000000ull, kWcMaxLagNs = 200000000ull;
static const int64_t kWcPhaseNs = 6000000, kWcPhaseLo = 4000000, kWcPhaseHi = 9000000;
static const uint64_t kWcLateNs = 5000000;   // a tick with a frame due but none new waits this long for it
static const uint32_t kWcMaxW = 4096, kWcMaxH = 2160;   // larger rings (either orientation) stay offline: per-frame copy and draw
static uint64_t M13Ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // the ring's host clock

// pump queue
static BOOL gWcOn;                         // webcam is the applied source
static NSString *gWcSpecCam;               // uniqueID named by the spec; nil: the default camera
static NSString *gWcCam;                   // uniqueID in use
static char gWcHome[1024], gWcUdid[128];
static scw_shm_reader gWcR; static BOOL gWcRInit;
static uint64_t gWcNextResolve;
static CGImageRef gWcFrame; static uint64_t gWcEpoch, gWcLastSeq;
static void M13WcPathReset(void), M13WcPathFree(void), M13WcWarm(void);   // webcam_path.inc
static void M13WcBufsFree(void);
static CVPixelBufferRef gWcPB; static uint64_t gWcPBGen;   // the newest frame on the frame path (webcam_path.inc), and a count of them
static uint64_t gWcOfflineAt; static BOOL gWcLive, gWcEverLive;
static NSString *gWcBandText;
static uint64_t gWcPts, gWcLag;            // this tick's presentation time; tick - capture of the newest frame
static BOOL gWcHadFrame;                   // a frame was read since the source was applied
static void *gWcLockTimer; static uint64_t gWcLockAt, gWcPeriod; static int64_t gWcErr[8]; static int gWcErrN;
static uint64_t gWcBaseSeq, gWcBasePub;    // the camera's measured interval: publish times since the lock began
static uint64_t gWcLastPub;                // the newest frame's publish time
static long gWcLateOk, gWcLateMiss; static double gWcLateMs;   // late frames waited for (and found / not found)
static uint64_t gWcStatAt; static long gWcTicks, gWcNew, gWcDup, gWcSkip, gWcTorn, gWcOff, gWcRearms;
static double gWcCopyMs, gWcLatMs, gWcLatMax, gWcErrMs, gWcTickMs, gWcPubMin = 1e9, gWcPubMax; static long gWcTickN;   // tick: pump tick start -> sinks fed
// reader file (its own queue)
static dispatch_queue_t gWcFileQ; static dispatch_source_t gWcFileT; static char gWcReaderPath[1300];
static _Atomic int gWcOnA; static _Atomic uint64_t gWcTickNs, gWcWant;

static BOOL M13WcOn(void) { return gWcOn; }
static CMTime M13WcPts(void) { return CMTimeMake((int64_t)gWcPts, 1000000000); }
static void M13WcTickEnd(uint64_t t0) { gWcTickMs += (M13Ns() - t0) / 1e6; gWcTickN++; }   // pump.inc, webcam ticks

// $SIMCAM_HOME (exactly as the CLI and helper spell it: it is part of the ring name) and this simulator's UDID
static BOOL M13WcPaths(void) {
    if (gWcHome[0]) return YES;
    const char *h = getenv("SIMCAM_HOME"), *sd = getenv("SIMCAM_SIM_DIR"), *u = getenv("SIMULATOR_UDID");
    char tmp[1024] = "";
    if (h && h[0] == '/') snprintf(tmp, sizeof tmp, "%s", h);
    else if (sd && sd[0] == '/') {   // <home>/sims/<udid>
        snprintf(tmp, sizeof tmp, "%s", sd);
        size_t n = strlen(tmp); while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
        char *s = strrchr(tmp, '/'); if (s) *s = 0;
        n = strlen(tmp); if (n > 5 && strcmp(tmp + n - 5, "/sims") == 0) tmp[n - 5] = 0; else tmp[0] = 0;
    }
    size_t n = strlen(tmp); while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    if (!tmp[0]) return NO;
    snprintf(gWcHome, sizeof gWcHome, "%s", tmp);
    if (u && u[0]) snprintf(gWcUdid, sizeof gWcUdid, "%s", u);
    else if (sd) { const char *s = strrchr(sd, '/'); snprintf(gWcUdid, sizeof gWcUdid, "%s", s ? s + 1 : "unknown"); }
    else snprintf(gWcUdid, sizeof gWcUdid, "unknown");
    for (char *c = gWcUdid; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-')) *c = '_';
    return YES;
}
static NSString *M13WcReadDefault(void) {   // $SIMCAM_HOME/webcam/default: the default camera's uniqueID, one line
    char p[1100], b[160]; snprintf(p, sizeof p, "%s/webcam/default", gWcHome);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return nil;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return nil;
    b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0;
    size_t len = strlen(b); if (!len || len >= 128) return nil;
    for (size_t i = 0; i < len; i++) if ((unsigned char)b[i] < 0x20 || b[i] == 0x7f) return nil;
    return @(b);
}

// ---- reader file: rings / want / active, rewritten every second (tmp + rename), removed when the source is left
static void M13WcReaderWrite(void) {
    uint64_t now = M13Ns(), last = atomic_load(&gWcTickNs), want = atomic_load(&gWcWant);
    int active = atomic_load(&gWcOnA) && last && now - last < 1500000000ull;   // the pump ticked recently: a session runs
    char text[128], tmp[1310];
    int n = snprintf(text, sizeof text, "rings=1\nwant=%ux%u@%u\nactive=%d\n", (unsigned)(want >> 40), (unsigned)((want >> 16) & 0xffffff),
                     (unsigned)(want & 0xffff), active);
    snprintf(tmp, sizeof tmp, "%s.tmp", gWcReaderPath);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600); if (fd < 0) return;
    BOOL ok = write(fd, text, (size_t)n) == n; close(fd);
    if (!ok || rename(tmp, gWcReaderPath) != 0) unlink(tmp);
}
static void M13WcReaderStart(void) {
    if (gWcFileT || !gWcHome[0]) return;
    char d[1100]; snprintf(d, sizeof d, "%s/webcam", gWcHome); mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/webcam/readers", gWcHome); mkdir(d, 0700);
    snprintf(gWcReaderPath, sizeof gWcReaderPath, "%s/%s-%d", d, gWcUdid, getpid());
    if (!gWcFileQ) gWcFileQ = dispatch_queue_create("simcam.webcam.reader", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gWcFileQ);
    dispatch_source_set_timer(t, DISPATCH_TIME_NOW, NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{ M13WcReaderWrite(); });
    dispatch_source_set_cancel_handler(t, ^{ unlink(gWcReaderPath); });
    dispatch_resume(t); gWcFileT = t;
    LOG(@"webcam: reader file %s", gWcReaderPath);
}
static void M13WcReaderStop(void) { if (gWcFileT) { dispatch_source_cancel(gWcFileT); gWcFileT = nil; } }

static void M13WcDropFrame(void) { if (gWcFrame) CGImageRelease(gWcFrame); gWcFrame = NULL; if (gWcPB) CVPixelBufferRelease(gWcPB); gWcPB = NULL; }
static void M13WcCloseRing(void) { if (gWcRInit) scw_shm_reader_close(&gWcR); gWcRInit = NO; gWcLastSeq = 0; gWcEpoch = 0; }
static void M13WcApply(NSString *spec) {   // pump queue, from M12ApplySource
    gWcOn = YES; atomic_store(&gWcOnA, 1);
    gWcSpecCam = [spec hasPrefix:@"webcam:"] && spec.length > 7 ? [spec substringFromIndex:7] : nil;
    gWcCam = nil; gWcNextResolve = 0; gWcOfflineAt = 0; gWcLive = NO; gWcEverLive = NO; gWcBandText = nil;
    M13WcDropFrame(); M13WcCloseRing(); M13WcPathReset();
    gWcLag = 0; gWcHadFrame = NO; gWcPts = M13Ns() - 1;   // PTS continue from the previous source's (host clock)
    if (!M13WcPaths()) LOG(@"webcam: neither SIMCAM_HOME nor SIMCAM_SIM_DIR is set; the source stays offline");
    atomic_store(&gWcTickNs, M13Ns());   // applied from a tick: the first reader file already says active=1
    M13WcReaderStart();
    M13WcWarm();   // the GPU path's device and first kernel compile, off the pump queue
}
static void M13WcLeave(void) {   // pump queue: another source applied
    gWcOn = NO; atomic_store(&gWcOnA, 0);
    M13WcReaderStop(); M13WcDropFrame(); M13WcCloseRing(); M13WcPathFree(); M13WcBufsFree(); gWcCam = nil;
    LOG(@"webcam: source left");
}
// the ring name for the camera in use; for the default camera, $SIMCAM_HOME/webcam/default is re-read every second
static void M13WcResolve(uint64_t now) {
    if (!gWcHome[0] || now < gWcNextResolve) return;
    gWcNextResolve = now + SCW_REOPEN_NS;
    NSString *cam = gWcSpecCam ?: M13WcReadDefault();
    if (!cam || [cam isEqualToString:gWcCam ?: @""]) return;
    char hash[9], name[SCW_NAME_MAX + 1];
    scw_cam_hash(gWcHome, cam.UTF8String, hash);
    if (scw_shm_name(name, sizeof name, (unsigned long)getuid(), hash, 1) < 0) return;
    M13WcCloseRing();
    scw_shm_reader_init(&gWcR, name, kWcRings, 1); gWcRInit = YES;
    gWcR.max_w = kWcMaxW; gWcR.max_h = kWcMaxH;
    gWcCam = cam;
    LOG(@"webcam: camera %@ -> ring %s", cam, name);
}
static NSString *M13WcWhy(void) {
    if (!gWcHome[0]) return @"SIMCAM_HOME unknown";
    if (!gWcRInit) return gWcSpecCam ? @"no ring" : @"no default camera (start the helper, or use webcam:<uniqueID>)";
    NSString *w = @(gWcR.why);
    if ([w hasPrefix:@"webcam: "]) w = [w substringFromIndex:8];
    if ([w containsString:@"(helper not running)"]) return @"helper not running";
    if ([w hasPrefix:@"no fresh frame"]) return @"no fresh frames from the helper";
    // the shm name says nothing to someone looking at the picture (it is in the engine log)
    return [w stringByReplacingOccurrencesOfString:@" ?\\(?/scw\\.[^ )]*\\)?" withString:@"" options:NSRegularExpressionSearch range:NSMakeRange(0, w.length)];
}

// ---- phase lock: tick 6 ms after the helper's publish
static void M13WcRestorePump(M5Sess *S) {   // pump queue, every tick of another source: back to the nominal schedule once
    if (!gWcLockTimer) return;
    if (S.timer && gWcLockTimer == (__bridge void *)S.timer) {
        uint64_t p = (uint64_t)(NSEC_PER_SEC / (gPumpFps > 0 ? gPumpFps : 30));
        dispatch_source_set_timer(S.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)p), p, 0);
        LOG(@"webcam: pump back to its nominal %.3f ms period", p / 1e6);
    }
    gWcLockTimer = NULL;
}
static void M13WcPhase(M5Sess *S, uint64_t now, uint64_t pub, uint64_t seq) {
    static int phaseOff = -1; if (phaseOff < 0) { const char *e = getenv("SIMCAM_WEBCAM_PHASE"); phaseOff = e && e[0] == '0'; if (phaseOff) LOG(@"webcam: pump not locked to the ring (SIMCAM_WEBCAM_PHASE=0)"); }
    int64_t e0 = (int64_t)(now - pub);
    if (phaseOff) { if (pub <= now && e0 < 1000000000) gWcErrMs += e0 / 1e6; return; }
    if (!S.timer || pub > now || now - pub > 1000000000ull) return;
    double p0 = 1e9 / (gPumpFps > 0 ? gPumpFps : 30), p = p0;
    BOOL newTimer = gWcLockTimer != (__bridge void *)S.timer;
    if (newTimer || !gWcBaseSeq || seq <= gWcBaseSeq) { gWcBaseSeq = seq; gWcBasePub = pub; }
    else if (seq - gWcBaseSeq >= 150) {   // 5 s or more of frames: follow the camera if it is within 1 % of the format (or 2x)
        double cam = (double)(pub - gWcBasePub) / (double)(seq - gWcBaseSeq);
        for (int k = 1; k <= 2; k++) if (fabs(cam / k - p0) / p0 < 0.01) { p = cam / k; break; }
    }
    int64_t e = (int64_t)(now - pub);
    gWcErr[gWcErrN++ % 8] = e; gWcErrMs += e / 1e6;
    int n = MIN(gWcErrN, 8); int64_t sum = 0; for (int i = 0; i < n; i++) sum += gWcErr[i];
    int64_t mean = sum / n;
    BOOL off = n >= 4 && (mean < kWcPhaseLo || mean > kWcPhaseHi);
    BOOL rate = gWcPeriod && fabs(p - (double)gWcPeriod) > p * 0.0005;
    if (!newTimer && !off && !rate) return;
    if (!newTimer && now - gWcLockAt < NSEC_PER_SEC) return;
    double t = (double)pub + kWcPhaseNs + p;   // frame `pub` is consumed by this tick: the next useful tick is the next frame's
    while (t < (double)now + 2e6) t += p;
    uint64_t at = M13Ns();
    dispatch_source_set_timer(S.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(t > at ? t - at : 0)), (uint64_t)p, 0);
    if (gWcRearms < 8 || gWcRearms % 50 == 0)
        LOG(@"webcam: pump locked to the ring: next tick %.2f ms after the publish, period %.4f ms (%@; offset mean was %.2f ms over %d)",
            kWcPhaseNs / 1e6, p / 1e6, newTimer ? @"new timer" : off ? @"offset out of 4-9 ms" : @"rate", mean / 1e6, n);
    gWcLockTimer = (__bridge void *)S.timer; gWcLockAt = now; gWcPeriod = (uint64_t)p; gWcErrN = 0; gWcRearms++;
}

// A new frame is due: the pump is locked to the ring at 1x or 2x the camera's nominal rate (the header's fps) and a
// camera interval, less 3 ms, has passed since the newest frame's publish. At 2x, the tick between two frames comes
// half an interval after it, so it never waits.
static BOOL M13WcDue(M5Sess *S, uint64_t now) {
    if (!S || !S.timer || gWcLockTimer != (__bridge void *)S.timer || !gWcLastPub || now < gWcLastPub || !gWcR.g.fpsMilli) return NO;
    double p0 = 1e9 / (gPumpFps > 0 ? gPumpFps : 30), iv = 1e12 / gWcR.g.fpsMilli;
    if (!(fabs(iv - p0) < p0 * 0.02 || fabs(iv - 2 * p0) < 2 * p0 * 0.02)) return NO;
    return (double)(now - gWcLastPub) + 3e6 >= iv;
}
// Two reusable frame buffers. One is busy while a CGImage made from it lives (its provider's release clears that);
// with both busy, a one-off malloc that the release frees.
typedef struct { uint8_t *p; size_t len; _Atomic int busy; } M13WcBuf;
static M13WcBuf gWcBufs[2];
static void M13WcFree(void *info, const void *data, size_t size) { if (info) atomic_store(&((M13WcBuf *)info)->busy, 0); else free((void *)data); }
static uint8_t *M13WcBufGet(size_t len, M13WcBuf **owner) {
    *owner = NULL;
    for (int i = 0; i < 2; i++) {
        M13WcBuf *b = &gWcBufs[i];
        if (atomic_load(&b->busy)) continue;
        if (b->len != len) { free(b->p); b->p = malloc(len); b->len = b->p ? len : 0; }
        if (b->p) *owner = b;
        return b->p;
    }
    return malloc(len);
}
static void M13WcBufsFree(void) {   // source left: release the frame buffers no CGImage holds any more
    for (int i = 0; i < 2; i++) if (!atomic_load(&gWcBufs[i].busy)) { free(gWcBufs[i].p); gWcBufs[i].p = NULL; gWcBufs[i].len = 0; }
}
#include "webcam_path.inc"
// One tick: the frame to draw (borrowed): *frame on the frame path (webcam_path.inc), else the CGImage returned; both
// NULL for the scene, under the band whose text is *band.
static CGImageRef M13WcAdvance(M5Sess *S, NSString **band, BOOL *rescan, CVPixelBufferRef *frame) {
    uint64_t now = M13Ns();
    *band = nil; *rescan = NO; *frame = NULL; gWcDirect = NO; gWcTicks++;
    atomic_store(&gWcTickNs, now);
    if (S) atomic_store(&gWcWant, ((uint64_t)(S.fmtW & 0xffffff) << 40) | ((uint64_t)(S.fmtH & 0xffffff) << 16) | ((uint64_t)lround(S.fps) & 0xffff));
    if (!gWcStatAt) gWcStatAt = now;
    if (now - gWcStatAt >= 5 * NSEC_PER_SEC) {
        LOG(@"webcam stat %.1fs: ticks %ld new %ld dup %ld skipped %ld torn %ld offline %ld copy %.2f ms capture->tick avg %.1f max %.1f ms publish->tick avg %.2f min %.2f max %.2f ms rearms %ld period %.4f ms path %s render %.2f ms x %.1f tick %.2f ms late %ld/%ld %.2f ms",
            (now - gWcStatAt) / 1e9, gWcTicks, gWcNew, gWcDup, gWcSkip, gWcTorn, gWcOff, gWcNew ? gWcCopyMs / gWcNew : 0, gWcNew ? gWcLatMs / gWcNew : 0, gWcLatMax,
            gWcNew ? gWcErrMs / gWcNew : 0, gWcNew ? gWcPubMin : 0, gWcPubMax, gWcRearms, gWcPeriod / 1e6, M13WcPathName(gWcPath), gWcRenders ? gWcRenderMs / gWcRenders : 0,
            gWcTicks ? (double)gWcRenders / gWcTicks : 0, gWcTickN ? gWcTickMs / gWcTickN : 0, gWcLateOk, gWcLateOk + gWcLateMiss, gWcLateOk ? gWcLateMs / gWcLateOk : 0);
        gWcStatAt = now; gWcTicks = gWcNew = gWcDup = gWcSkip = gWcTorn = gWcOff = 0; gWcCopyMs = gWcLatMs = gWcLatMax = gWcErrMs = 0;
        gWcRenderMs = gWcTickMs = gWcPubMax = gWcLateMs = 0; gWcRenders = gWcTickN = gWcLateOk = gWcLateMiss = 0; gWcPubMin = 1e9;
    }
    M13WcResolve(now);
    scw_peek_t pk;
    BOOL live = gWcRInit && scw_shm_reader_poll(&gWcR, now, &pk);
    uint64_t cap = 0;
    if (live) {
        if (gWcR.g.epoch != gWcEpoch) {
            gWcEpoch = gWcR.g.epoch; gWcLastSeq = 0; gWcBaseSeq = 0; gWcErrN = 0; gWcLastPub = 0;   // a new helper: measure its rate and phase afresh
            LOG(@"webcam: ring %s %ux%u from '%s' (helper pid %d, epoch %016llx)", gWcR.name, gWcR.g.width, gWcR.g.height, gWcR.g.device, gWcR.g.producerPid, gWcR.g.epoch);
        }
        if (pk.latest == gWcLastSeq && M13WcDue(S, now)) {   // late: wait a little for it rather than duplicate now and skip next tick
            uint64_t w0 = M13Ns(), until = w0 + kWcLateNs; scw_peek_t p2;
            while (M13Ns() < until) { usleep(200); if (scw_peek(gWcR.map, &gWcR.g, &p2) != SCW_OK) break; if (p2.latest != gWcLastSeq) { pk = p2; break; } }
            if (pk.latest != gWcLastSeq) { now = M13Ns(); gWcLateOk++; gWcLateMs += (now - w0) / 1e6; } else gWcLateMiss++;
        }
        scw_slot_read sr; uint64_t t0 = M13Ns(); int rc;
        if (pk.latest == gWcLastSeq) rc = SCW_NONE;   // no new frame: no buffer, no copy
        else if (M13WcPathOn()) {   // the frame path: one copy into a pooled ring-sized buffer
            CVPixelBufferRef fb = NULL; rc = M13WcCopy(gWcLastSeq, &sr, &fb);
            if (rc == SCW_OK) { M13WcDropFrame(); gWcPB = fb; gWcPBGen++; }
        } else {                     // the CG canvas: the frame as a CGImage that scene.inc draws
            size_t bpr = (size_t)gWcR.g.width * 4, len = bpr * gWcR.g.height;
            M13WcBuf *owner = NULL;
            uint8_t *buf = M13WcBufGet(len, &owner);
            rc = buf ? scw_read_latest(gWcR.map, &gWcR.g, gWcLastSeq, buf, bpr, len, &sr) : SCW_E_ARG;
            if (rc == SCW_OK) {
                static CGColorSpaceRef cs; if (!cs) cs = CGColorSpaceCreateDeviceRGB();
                if (owner) atomic_store(&owner->busy, 1);
                CGDataProviderRef dp = CGDataProviderCreateWithData(owner, buf, len, M13WcFree);
                CGImageRef img = CGImageCreate(gWcR.g.width, gWcR.g.height, 8, 32, bpr, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false, kCGRenderingIntentDefault);
                CGDataProviderRelease(dp);
                if (img) { M13WcDropFrame(); gWcFrame = img; }
            } else if (!owner) free(buf);
        }
        if (rc == SCW_OK) {
            if (gWcLastSeq && sr.seq > gWcLastSeq + 1) gWcSkip += (long)(sr.seq - gWcLastSeq - 1);
            gWcLastPub = sr.publishNs;
            gWcLastSeq = sr.seq; gWcNew++; cap = sr.captureNs;
            gWcCopyMs += (M13Ns() - t0) / 1e6;
            double lat = (int64_t)(now - sr.captureNs) / 1e6; gWcLatMs += lat; if (lat > gWcLatMax) gWcLatMax = lat;
            if (gWcNew % 15 == 1) *rescan = YES;   // a moving picture: barcode detection reruns now and then
            M13WcPhase(S, now, sr.publishNs, sr.seq);
            double pt = (int64_t)(now - sr.publishNs) / 1e6; if (pt < gWcPubMin) gWcPubMin = pt; if (pt > gWcPubMax) gWcPubMax = pt;
        } else {
            if (rc == SCW_NONE) gWcDup++; else if (rc == SCW_E_TORN) gWcTorn++;
        }
        if (!gWcLive) LOG(@"webcam: live ('%s' %ux%u)", gWcR.g.device, gWcR.g.width, gWcR.g.height);
        gWcLive = YES; gWcEverLive = YES; gWcOfflineAt = 0; gWcBandText = nil;
    } else {
        gWcOff++;
        static int lastSt; if (gWcRInit && gWcR.status != lastSt) { if (gWcR.status == SCW_E_LIMIT) LOG(@"%s", gWcR.why); lastSt = gWcR.status; }
        if (gWcLive || !gWcOfflineAt) { gWcOfflineAt = now; LOG(@"webcam: offline (%s)%@", gWcRInit ? gWcR.why : "not opened", gWcFrame || gWcPB ? @"; keeping the last frame for 1 s" : @""); }
        gWcLive = NO;
        if ((gWcFrame || gWcPB) && now - gWcOfflineAt >= kWcFreezeNs) { M13WcDropFrame(); LOG(@"webcam: offline band on (%@)", M13WcWhy()); }
    }
    // presentation time: the capture time of a new frame; between frames, the tick time less the same lag. Until the
    // first frame the lag grows by a quarter period a tick towards 200 ms, so that frame's capture time (at most 200 ms
    // back) doesn't fall behind the band frames before it.
    if (cap) { uint64_t c = cap > now ? now : cap; if (now - c > kWcMaxLagNs) c = now - kWcMaxLagNs; gWcLag = now - c; gWcHadFrame = YES; }
    uint64_t pts = now - MIN(gWcLag, kWcMaxLagNs);
    if (pts <= gWcPts) pts = gWcPts + 1000;
    gWcPts = pts;
    if (!gWcHadFrame) gWcLag = MIN(kWcMaxLagNs, gWcLag + (uint64_t)(NSEC_PER_SEC / (gPumpFps > 0 ? gPumpFps : 30)) / 4);
    if (gWcPB) { *frame = gWcPB; return NULL; }
    if (gWcFrame) return gWcFrame;
    NSString *why = M13WcWhy();
    if (![why isEqualToString:gWcBandText ?: @""]) gWcBandText = why;
    *band = [@"webcam offline: " stringByAppendingString:gWcBandText];
    return NULL;
}
// The offline band near the top of the upright picture, above the scene's colour bar and below a phone's status bar;
// the text stays in the central 70 %, which aspect-fill previews don't crop (canvas coordinates: origin top-left, y down).
static void M13WcDrawBand(CGContextRef c, double cw, double ch, NSString *text) {
    double bh = MAX(36, MIN(cw, ch) * 0.075), y = ch * 0.07;
    CGContextSetRGBFillColor(c, 0.75, 0.08, 0.16, 1); CGContextFillRect(c, CGRectMake(0, y, cw, bh));
    UIGraphicsPushContext(c);
    double fs = bh * 0.42;
    NSDictionary *a = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fs], NSForegroundColorAttributeName: UIColor.whiteColor};
    CGSize sz = [text sizeWithAttributes:a];
    if (sz.width > cw * 0.7) { fs *= cw * 0.7 / sz.width; a = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fs], NSForegroundColorAttributeName: UIColor.whiteColor}; sz = [text sizeWithAttributes:a]; }
    [text drawAtPoint:CGPointMake((cw - sz.width) / 2, y + (bh - sz.height) / 2) withAttributes:a];
    UIGraphicsPopContext();
}
