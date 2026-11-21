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
// larger rings (either orientation) stay offline: per-frame copy and draw
static const uint32_t kWcMaxW = 4096, kWcMaxH = 2160;
static uint64_t WcNowNs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // the ring's host clock

// pump queue
static BOOL gWcOn;                         // webcam is the applied source
static NSString *gWcSpecCam;               // uniqueID named by the spec; nil: the default camera
static NSString *gWcCam;                   // uniqueID in use
static char gWcHome[1024], gWcUdid[128];
static scw_shm_reader gWcR;
static BOOL gWcRInit;
static uint64_t gWcNextResolve;
static CGImageRef gWcFrame;
static uint64_t gWcEpoch, gWcLastSeq;
static void WcPathReset(void), WcPathFree(void), WcWarm(void);   // webcam_path.inc
static void WcFrameBufsFree(void);
// the newest frame on the frame path (webcam_path.inc), and a count of them
static CVPixelBufferRef gWcPB;
static uint64_t gWcPBGen;
static uint64_t gWcOfflineAt;
static BOOL gWcLive, gWcEverLive;
static NSString *gWcBandText;
static uint64_t gWcPts, gWcLag;            // this tick's presentation time; tick - capture of the newest frame
static BOOL gWcHadFrame;                   // a frame was read since the source was applied
// the phase lock: the timer it re-armed, when, at what period, and the last 8 publish->tick offsets
static void *gWcLockTimer;
static uint64_t gWcLockAt, gWcPeriod;
static int64_t gWcErr[8];
static int gWcErrN;
static uint64_t gWcBaseSeq, gWcBasePub;    // the camera's measured interval: publish times since the lock began
static uint64_t gWcLastPub;                // the newest frame's publish time
static long gWcLateOk, gWcLateMiss;        // late frames waited for (and found / not found)
static double gWcLateMs;
// stats for the 5 s "webcam stat" line
static uint64_t gWcStatAt;
static long gWcTicks, gWcNew, gWcDup, gWcSkip, gWcTorn, gWcOff, gWcRearms;
static double gWcCopyMs, gWcLatMs, gWcLatMax, gWcErrMs, gWcTickMs, gWcPubMin = 1e9, gWcPubMax;
static long gWcTickN;   // tick: pump tick start -> sinks fed
// reader file (its own queue)
static dispatch_queue_t gWcFileQ;
static dispatch_source_t gWcFileT;
static char gWcReaderPath[1300];
static _Atomic int gWcOnA;
static _Atomic uint64_t gWcTickNs, gWcWant;

static BOOL WcIsOn(void) { return gWcOn; }
static CMTime WcPts(void) { return CMTimeMake((int64_t)gWcPts, 1000000000); }
static void WcTickEnd(uint64_t t0) {   // pump.inc, webcam ticks
    gWcTickMs += (WcNowNs() - t0) / 1e6;
    gWcTickN++;
}

// $SIMCAM_HOME (exactly as the CLI and helper spell it: it is part of the ring name) and this simulator's UDID
static BOOL WcFindHome(void) {
    if (gWcHome[0]) return YES;
    const char *home = getenv(ENV_HOME), *simDir = getenv(ENV_SIM_DIR), *udid = getenv("SIMULATOR_UDID");
    char tmp[1024] = "";
    if (home && home[0] == '/') {
        snprintf(tmp, sizeof tmp, "%s", home);
    } else if (simDir && simDir[0] == '/') {   // <home>/sims/<udid>
        snprintf(tmp, sizeof tmp, "%s", simDir);
        size_t n = strlen(tmp);
        while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
        char *slash = strrchr(tmp, '/');
        if (slash) *slash = 0;
        n = strlen(tmp);
        if (n > 5 && strcmp(tmp + n - 5, "/sims") == 0) tmp[n - 5] = 0;
        else tmp[0] = 0;
    }
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    if (!tmp[0]) return NO;
    snprintf(gWcHome, sizeof gWcHome, "%s", tmp);
    if (udid && udid[0]) {
        snprintf(gWcUdid, sizeof gWcUdid, "%s", udid);
    } else if (simDir) {
        const char *slash = strrchr(simDir, '/');
        snprintf(gWcUdid, sizeof gWcUdid, "%s", slash ? slash + 1 : "unknown");
    } else {
        snprintf(gWcUdid, sizeof gWcUdid, "unknown");
    }
    for (char *c = gWcUdid; *c; c++)
        if (!(isalnum((unsigned char)*c) || *c == '-')) *c = '_';
    return YES;
}
static NSString *WcReadDefault(void) {   // $SIMCAM_HOME/webcam/default: the default camera's uniqueID, one line
    char path[1100], buf[160];
    snprintf(path, sizeof path, "%s/webcam/default", gWcHome);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return nil;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return nil;
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    size_t len = strlen(buf);
    if (!len || len >= 128) return nil;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)buf[i] < 0x20 || buf[i] == 0x7f) return nil;
    return @(buf);
}

// ---- reader file: rings / want / active, rewritten every second (tmp + rename), removed when the source is left
static void WcReaderWrite(void) {
    uint64_t now = WcNowNs(), last = atomic_load(&gWcTickNs), want = atomic_load(&gWcWant);
    // the pump ticked recently: a session runs
    int active = atomic_load(&gWcOnA) && last && now - last < 1500000000ull;
    char text[128], tmp[1310];
    int n = snprintf(text, sizeof text, "rings=1\nwant=%ux%u@%u\nactive=%d\n", (unsigned)(want >> 40),
                     (unsigned)((want >> 16) & 0xffffff), (unsigned)(want & 0xffff), active);
    snprintf(tmp, sizeof tmp, "%s.tmp", gWcReaderPath);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    BOOL ok = write(fd, text, (size_t)n) == n;
    close(fd);
    if (!ok || rename(tmp, gWcReaderPath) != 0) unlink(tmp);
}
static void WcReaderStart(void) {
    if (gWcFileT || !gWcHome[0]) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/webcam", gWcHome);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/webcam/readers", gWcHome);
    mkdir(dir, 0700);
    snprintf(gWcReaderPath, sizeof gWcReaderPath, "%s/%s-%d", dir, gWcUdid, getpid());
    if (!gWcFileQ) gWcFileQ = dispatch_queue_create("simcam.webcam.reader", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gWcFileQ);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{ WcReaderWrite(); });
    dispatch_source_set_cancel_handler(timer, ^{ unlink(gWcReaderPath); });
    dispatch_resume(timer);
    gWcFileT = timer;
    LOG(@"webcam: reader file %s", gWcReaderPath);
}
static void WcReaderStop(void) {
    if (gWcFileT) {
        dispatch_source_cancel(gWcFileT);
        gWcFileT = nil;
    }
}

static void WcDropFrame(void) {
    if (gWcFrame) CGImageRelease(gWcFrame);
    gWcFrame = NULL;
    if (gWcPB) CVPixelBufferRelease(gWcPB);
    gWcPB = NULL;
}
static void WcCloseRing(void) {
    if (gWcRInit) scw_shm_reader_close(&gWcR);
    gWcRInit = NO;
    gWcLastSeq = 0;
    gWcEpoch = 0;
}
static void WcApply(NSString *spec) {   // pump queue, from SceneApplySource
    gWcOn = YES;
    atomic_store(&gWcOnA, 1);
    gWcSpecCam = [spec hasPrefix:@"webcam:"] && spec.length > 7 ? [spec substringFromIndex:7] : nil;
    gWcCam = nil;
    gWcNextResolve = 0;
    gWcOfflineAt = 0;
    gWcLive = NO;
    gWcEverLive = NO;
    gWcBandText = nil;
    WcDropFrame();
    WcCloseRing();
    WcPathReset();
    gWcLag = 0;
    gWcHadFrame = NO;
    gWcPts = WcNowNs() - 1;   // PTS continue from the previous source's (host clock)
    if (!WcFindHome()) LOG(@"webcam: neither SIMCAM_HOME nor SIMCAM_SIM_DIR is set; the source stays offline");
    atomic_store(&gWcTickNs, WcNowNs());   // applied from a tick: the first reader file already says active=1
    WcReaderStart();
    WcWarm();   // the GPU path's device and first kernel compile, off the pump queue
}
static void WcLeave(void) {   // pump queue: another source applied
    gWcOn = NO;
    atomic_store(&gWcOnA, 0);
    WcReaderStop();
    WcDropFrame();
    WcCloseRing();
    WcPathFree();
    WcFrameBufsFree();
    gWcCam = nil;
    LOG(@"webcam: source left");
}
// the ring name for the camera in use; for the default camera, $SIMCAM_HOME/webcam/default is re-read every second
static void WcResolve(uint64_t now) {
    if (!gWcHome[0] || now < gWcNextResolve) return;
    gWcNextResolve = now + SCW_REOPEN_NS;
    NSString *cam = gWcSpecCam ?: WcReadDefault();
    if (!cam || [cam isEqualToString:gWcCam ?: @""]) return;
    char hash[9], name[SCW_NAME_MAX + 1];
    scw_cam_hash(gWcHome, cam.UTF8String, hash);
    if (scw_shm_name(name, sizeof name, (unsigned long)getuid(), hash, 1) < 0) return;
    WcCloseRing();
    scw_shm_reader_init(&gWcR, name, kWcRings, 1);
    gWcRInit = YES;
    gWcR.max_w = kWcMaxW;
    gWcR.max_h = kWcMaxH;
    gWcCam = cam;
    LOG(@"webcam: camera %@ -> ring %s", cam, name);
}
static NSString *WcOfflineReason(void) {
    if (!gWcHome[0]) return @"SIMCAM_HOME unknown";
    if (!gWcRInit) return gWcSpecCam ? @"no ring" : @"no default camera (start the helper, or use webcam:<uniqueID>)";
    NSString *why = @(gWcR.why);
    if ([why hasPrefix:@"webcam: "]) why = [why substringFromIndex:8];
    if ([why containsString:@"(helper not running)"]) return @"helper not running";
    if ([why hasPrefix:@"no fresh frame"]) return @"no fresh frames from the helper";
    // the shm name says nothing to someone looking at the picture (it is in the engine log)
    return [why stringByReplacingOccurrencesOfString:@" ?\\(?/scw\\.[^ )]*\\)?" withString:@""
                                             options:NSRegularExpressionSearch range:NSMakeRange(0, why.length)];
}

// ---- phase lock: tick 6 ms after the helper's publish
// pump queue, every tick of another source: back to the nominal schedule once
static void WcRestorePump(SimcamSession *S) {
    if (!gWcLockTimer) return;
    if (S.timer && gWcLockTimer == (__bridge void *)S.timer) {
        uint64_t period = (uint64_t)(NSEC_PER_SEC / PumpFps());
        dispatch_source_set_timer(S.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)period), period, 0);
        LOG(@"webcam: pump back to its nominal %.3f ms period", period / 1e6);
    }
    gWcLockTimer = NULL;
}
static void WcPhaseLock(SimcamSession *S, uint64_t now, uint64_t pub, uint64_t seq) {
    static int phaseOff = -1;
    if (phaseOff < 0) {
        const char *e = getenv(ENV_WEBCAM_PHASE);
        phaseOff = e && e[0] == '0';
        if (phaseOff) LOG(@"webcam: pump not locked to the ring (SIMCAM_WEBCAM_PHASE=0)");
    }
    int64_t offset0 = (int64_t)(now - pub);
    if (phaseOff) {
        if (pub <= now && offset0 < 1000000000) gWcErrMs += offset0 / 1e6;
        return;
    }
    if (!S.timer || pub > now || now - pub > 1000000000ull) return;
    double nominal = 1e9 / PumpFps(), period = nominal;
    BOOL newTimer = gWcLockTimer != (__bridge void *)S.timer;
    if (newTimer || !gWcBaseSeq || seq <= gWcBaseSeq) {
        gWcBaseSeq = seq;
        gWcBasePub = pub;
    } else if (seq - gWcBaseSeq >= 150) {
        // 5 s or more of frames: follow the camera if it is within 1 % of the format (or 2x)
        double cam = (double)(pub - gWcBasePub) / (double)(seq - gWcBaseSeq);
        for (int k = 1; k <= 2; k++)
            if (fabs(cam / k - nominal) / nominal < 0.01) {
                period = cam / k;
                break;
            }
    }
    int64_t offset = (int64_t)(now - pub);
    gWcErr[gWcErrN++ % 8] = offset;
    gWcErrMs += offset / 1e6;
    int n = MIN(gWcErrN, 8);
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += gWcErr[i];
    int64_t mean = sum / n;
    BOOL off = n >= 4 && (mean < kWcPhaseLo || mean > kWcPhaseHi);
    BOOL rate = gWcPeriod && fabs(period - (double)gWcPeriod) > period * 0.0005;
    if (!newTimer && !off && !rate) return;
    if (!newTimer && now - gWcLockAt < NSEC_PER_SEC) return;
    // frame `pub` is consumed by this tick: the next useful tick is the next frame's
    double next = (double)pub + kWcPhaseNs + period;
    while (next < (double)now + 2e6) next += period;
    uint64_t at = WcNowNs();
    dispatch_source_set_timer(S.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(next > at ? next - at : 0)),
                              (uint64_t)period, 0);
    if (gWcRearms < 8 || gWcRearms % 50 == 0)
        LOG(@"webcam: pump locked to the ring: next tick %.2f ms after the publish, period %.4f ms "
            @"(%@; offset mean was %.2f ms over %d)",
            kWcPhaseNs / 1e6, period / 1e6, newTimer ? @"new timer" : off ? @"offset out of 4-9 ms" : @"rate",
            mean / 1e6, n);
    gWcLockTimer = (__bridge void *)S.timer;
    gWcLockAt = now;
    gWcPeriod = (uint64_t)period;
    gWcErrN = 0;
    gWcRearms++;
}

// A new frame is due: the pump is locked to the ring at 1x or 2x the camera's nominal rate (the header's fps) and a
// camera interval, less 3 ms, has passed since the newest frame's publish. At 2x, the tick between two frames comes
// half an interval after it, so it never waits.
static BOOL WcFrameDue(SimcamSession *S, uint64_t now) {
    if (!S || !S.timer || gWcLockTimer != (__bridge void *)S.timer || !gWcLastPub || now < gWcLastPub ||
        !gWcR.g.fpsMilli)
        return NO;
    double nominal = 1e9 / PumpFps(), interval = 1e12 / gWcR.g.fpsMilli;
    if (!(fabs(interval - nominal) < nominal * 0.02 || fabs(interval - 2 * nominal) < 2 * nominal * 0.02)) return NO;
    return (double)(now - gWcLastPub) + 3e6 >= interval;
}
// Two reusable frame buffers. One is busy while a CGImage made from it lives (its provider's release clears that);
// with both busy, a one-off malloc that the release frees.
typedef struct { uint8_t *p; size_t len; _Atomic int busy; } WcFrameBuf;
static WcFrameBuf gWcBufs[2];
static void WcFrameBufRelease(void *info, const void *data, size_t size) {
    if (info) atomic_store(&((WcFrameBuf *)info)->busy, 0);
    else free((void *)data);
}
static uint8_t *WcFrameBufGet(size_t len, WcFrameBuf **owner) {
    *owner = NULL;
    for (int i = 0; i < 2; i++) {
        WcFrameBuf *b = &gWcBufs[i];
        if (atomic_load(&b->busy)) continue;
        if (b->len != len) {
            free(b->p);
            b->p = malloc(len);
            b->len = b->p ? len : 0;
        }
        if (b->p) *owner = b;
        return b->p;
    }
    return malloc(len);
}
static void WcFrameBufsFree(void) {   // source left: release the frame buffers no CGImage holds any more
    for (int i = 0; i < 2; i++)
        if (!atomic_load(&gWcBufs[i].busy)) {
            free(gWcBufs[i].p);
            gWcBufs[i].p = NULL;
            gWcBufs[i].len = 0;
        }
}
#include "webcam_path.inc"
// Every 5 s: the "webcam stat" line, then the counters start again
static void WebcamStatsTick(uint64_t now) {
    if (!gWcStatAt) gWcStatAt = now;
    if (now - gWcStatAt < 5 * NSEC_PER_SEC) return;
    LOG(@"webcam stat %.1fs: ticks %ld new %ld dup %ld skipped %ld torn %ld offline %ld copy %.2f ms "
        @"capture->tick avg %.1f max %.1f ms publish->tick avg %.2f min %.2f max %.2f ms rearms %ld period %.4f ms "
        @"path %s render %.2f ms x %.1f tick %.2f ms late %ld/%ld %.2f ms",
        (now - gWcStatAt) / 1e9, gWcTicks, gWcNew, gWcDup, gWcSkip, gWcTorn, gWcOff,
        gWcNew ? gWcCopyMs / gWcNew : 0,                               // copy
        gWcNew ? gWcLatMs / gWcNew : 0, gWcLatMax,                     // capture->tick
        gWcNew ? gWcErrMs / gWcNew : 0, gWcNew ? gWcPubMin : 0, gWcPubMax,   // publish->tick
        gWcRearms, gWcPeriod / 1e6,                                    // rearms, period
        WcPathName(gWcPath), gWcRenders ? gWcRenderMs / gWcRenders : 0,
        gWcTicks ? (double)gWcRenders / gWcTicks : 0,                  // path, render (ms, renders per tick)
        gWcTickN ? gWcTickMs / gWcTickN : 0,                           // tick
        gWcLateOk, gWcLateOk + gWcLateMiss, gWcLateOk ? gWcLateMs / gWcLateOk : 0);   // late
    gWcStatAt = now;
    gWcTicks = gWcNew = gWcDup = gWcSkip = gWcTorn = gWcOff = 0;
    gWcCopyMs = gWcLatMs = gWcLatMax = gWcErrMs = 0;
    gWcRenderMs = gWcTickMs = gWcPubMax = gWcLateMs = 0;
    gWcRenders = gWcTickN = gWcLateOk = gWcLateMiss = 0;
    gWcPubMin = 1e9;
}
// A late frame: poll the ring up to kWcLateNs for a frame newer than the last one shown. Found: *peek is updated and
// *now is the time it was found.
static void WebcamWaitForLateFrame(scw_peek_t *peek, uint64_t *now) {
    uint64_t w0 = WcNowNs(), until = w0 + kWcLateNs;
    scw_peek_t peek2;
    while (WcNowNs() < until) {
        usleep(200);
        if (scw_peek(gWcR.map, &gWcR.g, &peek2) != SCW_OK) break;
        if (peek2.latest != gWcLastSeq) {
            *peek = peek2;
            break;
        }
    }
    if (peek->latest != gWcLastSeq) {
        *now = WcNowNs();
        gWcLateOk++;
        gWcLateMs += (*now - w0) / 1e6;
    } else {
        gWcLateMiss++;
    }
}
// The newest ring frame, read into gWcPB (the frame path) or gWcFrame (a CGImage for scene.inc's canvas). As
// scw_read_latest: SCW_OK with *sr filled, else the frame shown stays.
static int WebcamReadFrame(scw_slot_read *sr) {
    int rc;
    if (WcPathOn()) {   // the frame path: one copy into a pooled ring-sized buffer
        CVPixelBufferRef fb = NULL;
        rc = WcCopy(gWcLastSeq, sr, &fb);
        if (rc == SCW_OK) {
            WcDropFrame();
            gWcPB = fb;
            gWcPBGen++;
        }
    } else {                     // the CG canvas: the frame as a CGImage that scene.inc draws
        size_t bpr = (size_t)gWcR.g.width * 4, len = bpr * gWcR.g.height;
        WcFrameBuf *owner = NULL;
        uint8_t *buf = WcFrameBufGet(len, &owner);
        rc = buf ? scw_read_latest(gWcR.map, &gWcR.g, gWcLastSeq, buf, bpr, len, sr) : SCW_E_ARG;
        if (rc == SCW_OK) {
            static CGColorSpaceRef cs;
            if (!cs) cs = CGColorSpaceCreateDeviceRGB();
            if (owner) atomic_store(&owner->busy, 1);
            CGDataProviderRef dp = CGDataProviderCreateWithData(owner, buf, len, WcFrameBufRelease);
            CGImageRef img = CGImageCreate(gWcR.g.width, gWcR.g.height, 8, 32, bpr, cs,
                                           kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL, false,
                                           kCGRenderingIntentDefault);
            CGDataProviderRelease(dp);
            if (img) {
                WcDropFrame();
                gWcFrame = img;
            }
        } else if (!owner) {
            free(buf);
        }
    }
    return rc;
}
// A new frame was read (copy started at t0): its counters and timing stats, the phase lock, the rescan cadence
static void WebcamCountNewFrame(SimcamSession *S, uint64_t now, uint64_t t0, const scw_slot_read *sr, BOOL *rescan) {
    if (gWcLastSeq && sr->seq > gWcLastSeq + 1) gWcSkip += (long)(sr->seq - gWcLastSeq - 1);
    gWcLastPub = sr->publishNs;
    gWcLastSeq = sr->seq;
    gWcNew++;
    gWcCopyMs += (WcNowNs() - t0) / 1e6;
    double lat = (int64_t)(now - sr->captureNs) / 1e6;
    gWcLatMs += lat;
    if (lat > gWcLatMax) gWcLatMax = lat;
    if (gWcNew % 15 == 1) *rescan = YES;   // a moving picture: barcode detection reruns now and then
    WcPhaseLock(S, now, sr->publishNs, sr->seq);
    double pubToTick = (int64_t)(now - sr->publishNs) / 1e6;
    if (pubToTick < gWcPubMin) gWcPubMin = pubToTick;
    if (pubToTick > gWcPubMax) gWcPubMax = pubToTick;
}
// A tick with the ring not live: the offline state, its log lines, and the last frame dropped after kWcFreezeNs
static void WebcamOfflineTick(uint64_t now) {
    gWcOff++;
    static int lastSt;
    if (gWcRInit && gWcR.status != lastSt) {
        if (gWcR.status == SCW_E_LIMIT) LOG(@"%s", gWcR.why);
        lastSt = gWcR.status;
    }
    if (gWcLive || !gWcOfflineAt) {
        gWcOfflineAt = now;
        LOG(@"webcam: offline (%s)%@", gWcRInit ? gWcR.why : "not opened",
            gWcFrame || gWcPB ? @"; keeping the last frame for 1 s" : @"");
    }
    gWcLive = NO;
    if ((gWcFrame || gWcPB) && now - gWcOfflineAt >= kWcFreezeNs) {
        WcDropFrame();
        LOG(@"webcam: offline band on (%@)", WcOfflineReason());
    }
}
// This tick's presentation time (gWcPts): the capture time `cap` of a new frame (0: none); between frames, the tick
// time less the same lag. Until the first frame the lag grows by a quarter period a tick towards 200 ms, so that
// frame's capture time (at most 200 ms back) doesn't fall behind the band frames before it.
static void WebcamNextPts(uint64_t now, uint64_t cap) {
    if (cap) {
        uint64_t c = cap > now ? now : cap;
        if (now - c > kWcMaxLagNs) c = now - kWcMaxLagNs;
        gWcLag = now - c;
        gWcHadFrame = YES;
    }
    uint64_t pts = now - MIN(gWcLag, kWcMaxLagNs);
    if (pts <= gWcPts) pts = gWcPts + 1000;
    gWcPts = pts;
    if (!gWcHadFrame) gWcLag = MIN(kWcMaxLagNs, gWcLag + (uint64_t)(NSEC_PER_SEC / PumpFps()) / 4);
}
// One tick: the frame to draw (borrowed): *frame on the frame path (webcam_path.inc), else the CGImage returned; both
// NULL for the scene, under the band whose text is *band.
static CGImageRef WcAdvance(SimcamSession *S, NSString **band, BOOL *rescan, CVPixelBufferRef *frame) {
    uint64_t now = WcNowNs();
    *band = nil;
    *rescan = NO;
    *frame = NULL;
    gWcDirect = NO;
    gWcTicks++;
    atomic_store(&gWcTickNs, now);
    if (S)
        atomic_store(&gWcWant, ((uint64_t)(S.fmtW & 0xffffff) << 40) | ((uint64_t)(S.fmtH & 0xffffff) << 16) |
                                   ((uint64_t)lround(S.fps) & 0xffff));
    WebcamStatsTick(now);
    WcResolve(now);
    scw_peek_t peek;
    BOOL live = gWcRInit && scw_shm_reader_poll(&gWcR, now, &peek);
    uint64_t cap = 0;
    if (live) {
        if (gWcR.g.epoch != gWcEpoch) {   // a new helper: measure its rate and phase afresh
            gWcEpoch = gWcR.g.epoch;
            gWcLastSeq = 0;
            gWcBaseSeq = 0;
            gWcErrN = 0;
            gWcLastPub = 0;
            LOG(@"webcam: ring %s %ux%u from '%s' (helper pid %d, epoch %016llx)", gWcR.name, gWcR.g.width,
                gWcR.g.height, gWcR.g.device, gWcR.g.producerPid, gWcR.g.epoch);
        }
        // late: wait a little for it rather than duplicate now and skip next tick
        if (peek.latest == gWcLastSeq && WcFrameDue(S, now)) WebcamWaitForLateFrame(&peek, &now);
        scw_slot_read sr;
        uint64_t t0 = WcNowNs();
        int rc = peek.latest == gWcLastSeq ? SCW_NONE : WebcamReadFrame(&sr);   // no new frame: no buffer, no copy
        if (rc == SCW_OK) {
            cap = sr.captureNs;
            WebcamCountNewFrame(S, now, t0, &sr, rescan);
        } else {
            if (rc == SCW_NONE) gWcDup++;
            else if (rc == SCW_E_TORN) gWcTorn++;
        }
        if (!gWcLive) LOG(@"webcam: live ('%s' %ux%u)", gWcR.g.device, gWcR.g.width, gWcR.g.height);
        gWcLive = YES;
        gWcEverLive = YES;
        gWcOfflineAt = 0;
        gWcBandText = nil;
    } else {
        WebcamOfflineTick(now);
    }
    WebcamNextPts(now, cap);
    if (gWcPB) {
        *frame = gWcPB;
        return NULL;
    }
    if (gWcFrame) return gWcFrame;
    NSString *why = WcOfflineReason();
    if (![why isEqualToString:gWcBandText ?: @""]) gWcBandText = why;
    *band = [@"webcam offline: " stringByAppendingString:gWcBandText];
    return NULL;
}
// The offline band near the top of the upright picture, above the scene's colour bar and below a phone's status bar;
// the text stays in the central 70 %, which aspect-fill previews don't crop (canvas coordinates: origin top-left, y
// down).
static void WcDrawBand(CGContextRef c, double cw, double ch, NSString *text) {
    double bh = MAX(36, MIN(cw, ch) * 0.075), y = ch * 0.07;
    CGContextSetRGBFillColor(c, 0.75, 0.08, 0.16, 1);
    CGContextFillRect(c, CGRectMake(0, y, cw, bh));
    UIGraphicsPushContext(c);
    double fontSize = bh * 0.42;
    NSDictionary *attrs = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fontSize],
                            NSForegroundColorAttributeName: UIColor.whiteColor};
    CGSize size = [text sizeWithAttributes:attrs];
    if (size.width > cw * 0.7) {
        fontSize *= cw * 0.7 / size.width;
        attrs = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fontSize],
                  NSForegroundColorAttributeName: UIColor.whiteColor};
        size = [text sizeWithAttributes:attrs];
    }
    [text drawAtPoint:CGPointMake((cw - size.width) / 2, y + (bh - size.height) / 2) withAttributes:attrs];
    UIGraphicsPopContext();
}
