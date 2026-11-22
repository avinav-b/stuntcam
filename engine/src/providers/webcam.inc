// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): the live Mac webcam source.
//
// Provides: WebcamApply, WebcamLeave, WebcamAdvance, WebcamRestorePump, WebcamDrawOfflineBand (providers/scene.inc)
//           WebcamIsOn, WebcamNowNs, WebcamPts, WebcamTickEnd (providers/pump.inc)
// Owns:     gWebcam, the source's state, defined in providers/webcam.h with which queue owns what
//           gWebcam.frame.image is also read by providers/scene.inc; webcam_path.inc reads and writes the rest
// Uses:     webcam/ring.c, webcam/ring_shm.c, providers/webcam_path.inc (included at the end of
//           this file), providers/video.inc, fig/session_state.inc
//
// ============================================================================
// Source `webcam` or `webcam:<uniqueID>`, set by SIMCAM_SOURCE or the control file's source=.
// Frames come from the camera helper ("Stuntcam Camera.app", helper/) through a read-only shared-memory ring
// (docs/webcam-ring.md; webcam/ring.c and ring_shm.c, included by simcam.m).
// `webcam` alone means the camera named in $SIMCAM_HOME/webcam/default. $SIMCAM_HOME is that environment variable, or
// else the directory above SIMCAM_SIM_DIR's sims/.
// Everything here runs on the pump queue, except the reader file, which has its own queue.
//
//   live     The ring is running and its newest frame is under 500 ms old.
//            Each tick copies a new complete slot, under its seqlock, into a pooled ring-sized buffer. webcam_path.inc
//            then makes the canvas and every video data and preview buffer from it in one pass: crop, scale, turn and
//            mirror, on the GPU.
//            If that path is unavailable, scene.inc draws the frame aspect-fill into its canvas, as for a video frame.
//            The slot's capture time becomes the sample's presentation time. It is kept monotonic, and at most 200 ms
//            behind the tick.
//   offline  The last frame stays for 1 s. Then the scene shows, with a "webcam offline" band that gives the reason.
//            While offline, the shared-memory name is opened again every second. That picks up a restarted helper,
//            an idle quit or a newer ring.
//   phase    The pump timer is re-armed so that ticks land 6 ms after the helper publishes a frame.
//            It is re-armed when the mean offset of the last 8 frames leaves 4-9 ms, at most once a second.
//            The period follows the camera when the format's rate is within 1 % of the camera's, or of twice it.
//            Leaving the source restores the nominal schedule, so other sources never see the lock.
//            Off switch: SIMCAM_WEBCAM_PHASE=0 (a free-running pump).
//   late     This applies while the pump is locked at 1x or 2x the camera's rate. A frame is due when a camera
//            interval has passed since the last one's publish. A tick that finds no new frame although one is due
//            polls the ring for up to 5 ms before it shows the last frame again.
//            So a frame published just after its tick costs that tick up to 5 ms, instead of a duplicate frame
//            followed by a skipped one.
//   reader   The reader file, $SIMCAM_HOME/webcam/readers/<udid>-<pid>, holds rings, want and active.
//            It is rewritten every second while the source is selected, and removed when it is left. The helper opens
//            the camera only while some reader is active.
// Stats go to the engine log every 5 s ("webcam stat").
// ============================================================================
#import <UIKit/UIKit.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "webcam.h"
#include "../fig/session.h"
#include "pump.h"

// the ring versions this engine reads (cli/internal/webcam's test reads this line)
static const uint32_t kWcRings[] = {1};

// Timing: offline, presentation times, the phase lock and late frames.
static const uint64_t kWcFreezeNs = 1000000000ull;   // offline: the last frame stays this long before the band shows
static const uint64_t kWcMaxLagNs = 200000000ull;    // a frame's presentation time is at most this far behind its tick
static const int64_t kWcPhaseNs = 6000000;           // the phase lock aims ticks this long after the helper's publish
static const int64_t kWcPhaseLowNs = 4000000;        // it re-arms when the mean publish-to-tick offset leaves
static const int64_t kWcPhaseHighNs = 9000000;       // kWcPhaseLowNs..kWcPhaseHighNs
static const uint64_t kWcLateNs = 5000000;           // a tick with a frame due but none new waits this long for it

// Larger rings (in either orientation) stay offline: each frame is copied and drawn every tick.
static const uint32_t kWcMaxWidth = 4096;
static const uint32_t kWcMaxHeight = 2160;

// Buffer sizes, and the lengths of fixed prefixes.
#define kWcReaderTmpBytes 1310         // the reader file's path with ".tmp" appended
#define kWcCameraIdBytes 128           // a camera uniqueID is shorter than this
#define kWcDefaultFileBytes 160        // the most read from $SIMCAM_HOME/webcam/default
#define kWcSimsSuffixLen 5             // strlen("/sims")
#define kWcWhyPrefixLen 8              // strlen("webcam: ")

#define kWcPtsTimescale 1000000000     // presentation times are in nanoseconds

// The reader file.
#define kWcReaderActiveNs 1500000000ull   // the reader file says active while the pump ticked this recently
#define kWcReaderLeewayMs 100          // the reader file timer's leeway

// The format a session wants, packed into gWebcam.readerFile.wantedFormat: width and height in 24 bits each, and the
// frame rate in 16.
#define kWcWantWidthShift 40
#define kWcWantHeightShift 16
#define kWcWantDimMask 0xffffff
#define kWcWantFpsMask 0xffff

// The phase lock: its measurement, when it follows the camera, and its log.
#define kWcPhaseMinSamples 4           // the lock acts on its window's mean once it holds this many offsets
#define kWcPhaseMaxOffsetNs 1000000000   // an offset this large is no phase measurement (a stall, a stale frame)
#define kWcRateMinFrames 150           // frames (5 s at 30 fps) before the period follows the camera's measured rate
#define kWcRateTolerance 0.01          // the camera's rate counts as the format's (or twice it) within 1 %
#define kWcPeriodTolerance 0.0005      // a period change smaller than this share doesn't re-arm the timer
#define kWcPhaseMinLeadNs 2e6          // the re-armed timer's first tick is at least this far ahead
#define kWcRearmLogFirst 8             // the first re-arms are logged, then one in kWcRearmLogEvery
#define kWcRearmLogEvery 50

// Late frames.
#define kWcDueTolerance 0.02           // the camera's rate counts as 1x or 2x the pump's within 2 %
#define kWcDueEarlyNs 3e6              // a frame counts as due this long before a full camera interval has passed
#define kWcLatePollUs 200              // how often a tick polls the ring while it waits for a late frame

// Frames and their buffers.
#define kWcRescanEveryFrames 15        // barcode detection reruns on every 15th new frame
#define kWcMinPtsStepNs 1000           // presentation times increase by at least this much a tick
#define kWcBytesPerPixel 4             // BGRA
#define kWcBitsPerComponent 8
#define kWcBitsPerPixel 32

static uint64_t WebcamNowNs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // the ring's host clock

static BOOL WebcamIsOn(void) { return gWebcam.camera.on; }

static CMTime WebcamPts(void) { return CMTimeMake((int64_t)gWebcam.tick.ptsNs, kWcPtsTimescale); }

static void WebcamTickEnd(uint64_t tickStart) {   // providers/pump.inc, after each webcam tick
    gWebcam.stats.tickMs += (WebcamNowNs() - tickStart) / 1e6;
    gWebcam.stats.timedTicks++;
}

// Removes a path's trailing slashes, keeping a lone "/".
static void WebcamTrimSlashes(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = 0;
    }
}

// The home a sim directory is under: <home>/sims/<udid> gives <home>. Empty when simDir isn't in a sims/ directory.
static void WebcamHomeFromSimDir(const char *simDir, char *home, size_t cap) {
    snprintf(home, cap, "%s", simDir);
    WebcamTrimSlashes(home);
    char *slash = strrchr(home, '/');
    if (slash) {
        *slash = 0;
    }
    size_t len = strlen(home);
    if (len > kWcSimsSuffixLen && strcmp(home + len - kWcSimsSuffixLen, "/sims") == 0) {
        home[len - kWcSimsSuffixLen] = 0;
    } else {
        home[0] = 0;
    }
}

// gWebcam.ring.udid: SIMULATOR_UDID, else the sim directory's last component, made safe for a file name.
static void WebcamSetUdid(const char *udid, const char *simDir) {
    if (udid && udid[0]) {
        snprintf(gWebcam.ring.udid, sizeof gWebcam.ring.udid, "%s", udid);
    } else if (simDir) {
        const char *slash = strrchr(simDir, '/');
        snprintf(gWebcam.ring.udid, sizeof gWebcam.ring.udid, "%s", slash ? slash + 1 : "unknown");
    } else {
        snprintf(gWebcam.ring.udid, sizeof gWebcam.ring.udid, "unknown");
    }

    for (char *c = gWebcam.ring.udid; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '-')) {
            *c = '_';
        }
    }
}

// $SIMCAM_HOME (exactly as the CLI and helper spell it: it is part of the ring name) and this simulator's UDID
static BOOL WebcamFindHome(void) {
    if (gWebcam.ring.home[0]) {
        return YES;
    }
    const char *home = getenv(ENV_HOME);
    const char *simDir = getenv(ENV_SIM_DIR);
    const char *udid = getenv("SIMULATOR_UDID");
    char found[kWcHomeBytes] = "";

    if (home && home[0] == '/') {
        snprintf(found, sizeof found, "%s", home);
    } else if (simDir && simDir[0] == '/') {
        WebcamHomeFromSimDir(simDir, found, sizeof found);
    }
    WebcamTrimSlashes(found);
    if (!found[0]) {
        return NO;
    }
    snprintf(gWebcam.ring.home, sizeof gWebcam.ring.home, "%s", found);
    WebcamSetUdid(udid, simDir);
    return YES;
}

// $SIMCAM_HOME/webcam/default: the default camera's uniqueID, one line
static NSString *WebcamReadDefaultCamera(void) {
    char path[kWcHomePathBytes];
    char text[kWcDefaultFileBytes];
    snprintf(path, sizeof path, "%s/webcam/default", gWebcam.ring.home);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return nil;
    }
    ssize_t got = read(fd, text, sizeof text - 1);
    close(fd);
    if (got <= 0) {
        return nil;
    }
    text[got] = 0;
    char *newline = strchr(text, '\n');
    if (newline) {
        *newline = 0;
    }
    size_t len = strlen(text);
    if (!len || len >= kWcCameraIdBytes) {
        return nil;
    }
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)text[i] < 0x20 || text[i] == 0x7f) {
            return nil;   // a control character
        }
    }
    return @(text);
}

// ---- reader file: rings, want and active, rewritten every second (to a .tmp file, then renamed into place) and
// removed when the source is left
static void WebcamReaderFileWrite(void) {
    uint64_t now = WebcamNowNs();
    uint64_t lastTick = atomic_load(&gWebcam.readerFile.lastTickNs);
    uint64_t want = atomic_load(&gWebcam.readerFile.wantedFormat);

    // the pump ticked recently, so a session runs
    int active = atomic_load(&gWebcam.readerFile.sourceOn) && lastTick && now - lastTick < kWcReaderActiveNs;
    char text[128];
    char tmpPath[kWcReaderTmpBytes];
    int len = snprintf(text, sizeof text, "rings=1\nwant=%ux%u@%u\nactive=%d\n", (unsigned)(want >> kWcWantWidthShift),
                       (unsigned)((want >> kWcWantHeightShift) & kWcWantDimMask), (unsigned)(want & kWcWantFpsMask),
                       active);
    snprintf(tmpPath, sizeof tmpPath, "%s.tmp", gWebcam.readerFile.path);
    unlink(tmpPath);
    int fd = open(tmpPath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    BOOL ok = write(fd, text, (size_t)len) == len;
    close(fd);
    if (!ok || rename(tmpPath, gWebcam.readerFile.path) != 0) {
        unlink(tmpPath);
    }
}

static void WebcamReaderFileStart(void) {
    if (gWebcam.readerFile.timer || !gWebcam.ring.home[0]) {
        return;
    }
    char dir[kWcHomePathBytes];
    // the directories are usually there already; a real failure shows when the reader file is written
    snprintf(dir, sizeof dir, "%s/webcam", gWebcam.ring.home);
    (void)mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/webcam/readers", gWebcam.ring.home);
    (void)mkdir(dir, 0700);
    snprintf(gWebcam.readerFile.path, sizeof gWebcam.readerFile.path, "%s/%s-%d", dir, gWebcam.ring.udid, getpid());
    if (!gWebcam.readerFile.queue) {
        gWebcam.readerFile.queue = dispatch_queue_create("simcam.webcam.reader", DISPATCH_QUEUE_SERIAL);
    }
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gWebcam.readerFile.queue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC, kWcReaderLeewayMs * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{ WebcamReaderFileWrite(); });
    dispatch_source_set_cancel_handler(timer, ^{ unlink(gWebcam.readerFile.path); });
    dispatch_resume(timer);
    gWebcam.readerFile.timer = timer;
    LOG(@"webcam: reader file %s", gWebcam.readerFile.path);
}

static void WebcamReaderFileStop(void) {
    if (gWebcam.readerFile.timer) {
        dispatch_source_cancel(gWebcam.readerFile.timer);
        gWebcam.readerFile.timer = nil;
    }
}

static void WebcamDropFrame(void) {
    if (gWebcam.frame.image) {
        CGImageRelease(gWebcam.frame.image);
    }
    gWebcam.frame.image = NULL;
    if (gWebcam.frame.buffer) {
        CVPixelBufferRelease(gWebcam.frame.buffer);
    }
    gWebcam.frame.buffer = NULL;
}

static void WebcamCloseRing(void) {
    if (gWebcam.ring.open) {
        scw_shm_reader_close(&gWebcam.ring.reader);
    }
    gWebcam.ring.open = NO;
    gWebcam.ring.lastSeq = 0;
    gWebcam.ring.epoch = 0;
}

static void WebcamApply(NSString *spec) {   // pump queue, from SceneApplySource
    gWebcam.camera.on = YES;
    atomic_store(&gWebcam.readerFile.sourceOn, 1);
    gWebcam.camera.fromSpec = [spec hasPrefix:@"webcam:"] && spec.length > kWcSpecPrefixLen
                     ? [spec substringFromIndex:kWcSpecPrefixLen]
                     : nil;
    gWebcam.camera.inUse = nil;
    gWebcam.ring.nextResolveNs = 0;
    gWebcam.feed.offlineSinceNs = 0;
    gWebcam.feed.live = NO;
    gWebcam.feed.everLive = NO;
    gWebcam.feed.bandText = nil;
    WebcamDropFrame();
    WebcamCloseRing();
    WebcamPathReset();
    gWebcam.tick.lagNs = 0;
    gWebcam.feed.hadFrame = NO;
    gWebcam.tick.ptsNs = WebcamNowNs() - 1;   // presentation times continue from the previous source's (host clock)
    if (!WebcamFindHome()) {
        LOG(@"webcam: neither SIMCAM_HOME nor SIMCAM_SIM_DIR is set; the source stays offline");
    }
    // Applied from a tick, so the first reader file already says active=1.
    atomic_store(&gWebcam.readerFile.lastTickNs, WebcamNowNs());
    WebcamReaderFileStart();
    WebcamPathWarm();   // the GPU path's device and first kernel compile, off the pump queue
}

static void WebcamLeave(void) {   // pump queue: another source applied
    gWebcam.camera.on = NO;
    atomic_store(&gWebcam.readerFile.sourceOn, 0);
    WebcamReaderFileStop();
    WebcamDropFrame();
    WebcamCloseRing();
    WebcamPathFree();
    WebcamFrameBuffersFree();
    gWebcam.camera.inUse = nil;
    LOG(@"webcam: source left");
}

// the ring name for the camera in use; for the default camera, $SIMCAM_HOME/webcam/default is re-read every second
static void WebcamResolveRing(uint64_t now) {
    if (!gWebcam.ring.home[0] || now < gWebcam.ring.nextResolveNs) {
        return;
    }
    gWebcam.ring.nextResolveNs = now + SCW_REOPEN_NS;
    NSString *camera = gWebcam.camera.fromSpec ?: WebcamReadDefaultCamera();
    if (!camera || [camera isEqualToString:gWebcam.camera.inUse ?: @""]) {
        return;
    }
    char hash[SCW_CAM_HASH_BYTES];
    char name[SCW_NAME_MAX + 1];
    scw_cam_hash(gWebcam.ring.home, camera.UTF8String, hash);

    // the name carries the ring version: this engine reads one (kWcRings)
    if (scw_shm_name(name, sizeof name, (unsigned long)getuid(), hash, kWcRings[0]) < 0) {
        return;
    }
    WebcamCloseRing();
    scw_shm_reader_init(&gWebcam.ring.reader, name, kWcRings, (unsigned)(sizeof kWcRings / sizeof kWcRings[0]));
    gWebcam.ring.open = YES;
    gWebcam.ring.reader.maxWidth = kWcMaxWidth;
    gWebcam.ring.reader.maxHeight = kWcMaxHeight;
    gWebcam.camera.inUse = camera;
    LOG(@"webcam: camera %@ -> ring %s", camera, name);
}

static NSString *WebcamOfflineReason(void) {
    if (!gWebcam.ring.home[0]) {
        return @"SIMCAM_HOME unknown";
    }
    if (!gWebcam.ring.open) {
        return gWebcam.camera.fromSpec ? @"no ring" : @"no default camera (start the helper, or use webcam:<uniqueID>)";
    }
    NSString *why = @(gWebcam.ring.reader.why);
    if ([why hasPrefix:@"webcam: "]) {
        why = [why substringFromIndex:kWcWhyPrefixLen];
    }
    if ([why containsString:@"(helper not running)"]) {
        return @"helper not running";
    }
    if ([why hasPrefix:@"no fresh frame"]) {
        return @"no fresh frames from the helper";
    }
    // the shm name says nothing to someone looking at the picture (it is in the engine log)
    return [why stringByReplacingOccurrencesOfString:@" ?\\(?/scw\\.[^ )]*\\)?" withString:@""
                                             options:NSRegularExpressionSearch range:NSMakeRange(0, why.length)];
}

// ---- phase lock: tick 6 ms after the helper's publish
// Pump queue, on every tick of another source: back to the nominal schedule, once.
static void WebcamRestorePump(SimcamSession *sess) {
    if (!gWebcam.phaseLock.timer) {
        return;
    }
    if (sess.timer && gWebcam.phaseLock.timer == (__bridge void *)sess.timer) {
        uint64_t period = (uint64_t)(NSEC_PER_SEC / PumpFps());
        dispatch_source_set_timer(sess.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)period), period, 0);
        LOG(@"webcam: pump back to its nominal %.3f ms period", period / 1e6);
    }
    gWebcam.phaseLock.timer = NULL;
}

// SIMCAM_WEBCAM_PHASE=0 turns the phase lock off, leaving the pump free-running. It is read on the first frame.
static BOOL WebcamPhaseLockOff(void) {
    static int phaseLockOff = -1;

    if (phaseLockOff < 0) {
        const char *env = getenv(ENV_WEBCAM_PHASE);
        phaseLockOff = env && env[0] == '0';
        if (phaseLockOff) {
            LOG(@"webcam: pump not locked to the ring (SIMCAM_WEBCAM_PHASE=0)");
        }
    }

    return phaseLockOff;
}

// The period the pump should tick at. It is the format's nominal period, until kWcRateMinFrames frames have been
// counted. Then it is the camera's measured interval, or half of it, when that is within 1 % of the nominal period.
// A new timer, or a sequence number that went back, starts the count again from frame `seq`.
static double WebcamPhaseLockPeriod(BOOL newTimer, uint64_t publishNs, uint64_t seq) {
    double nominal = 1e9 / PumpFps();
    double period = nominal;

    if (newTimer || !gWebcam.rate.baseSeq || seq <= gWebcam.rate.baseSeq) {
        gWebcam.rate.baseSeq = seq;
        gWebcam.rate.basePublishNs = publishNs;
        return period;
    }

    if (seq - gWebcam.rate.baseSeq < kWcRateMinFrames) {
        return period;
    }
    double cameraInterval = (double)(publishNs - gWebcam.rate.basePublishNs) / (double)(seq - gWebcam.rate.baseSeq);

    for (int multiple = 1; multiple <= 2; multiple++) {
        if (fabs(cameraInterval / multiple - nominal) / nominal < kWcRateTolerance) {
            period = cameraInterval / multiple;
            break;
        }
    }

    return period;
}

// Adds one publish-to-tick offset to the lock's window and to the stats. It returns the mean of the window, and the
// number of offsets in it in *samplesOut.
static int64_t WebcamPhaseLockAddOffset(int64_t offset, int *samplesOut) {
    gWebcam.phaseLock.offsets[gWebcam.phaseLock.offsetCount++ % kWcPhaseWindow] = offset;
    gWebcam.stats.publishToTickMs += offset / 1e6;
    int samples = MIN(gWebcam.phaseLock.offsetCount, kWcPhaseWindow);
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += gWebcam.phaseLock.offsets[i];
    }

    *samplesOut = samples;
    return sum / samples;
}

// Re-arms the session's pump timer at `period`, so that a tick lands kWcPhaseNs after each of the helper's publishes.
// This tick consumes the frame published at publishNs, so the next useful tick is the next frame's.
static void WebcamPhaseLockRearm(SimcamSession *sess, uint64_t now, uint64_t publishNs, double period,
                                 NSString *reason, int64_t mean, int samples) {
    double next = (double)publishNs + kWcPhaseNs + period;
    while (next < (double)now + kWcPhaseMinLeadNs) {
        next += period;
    }
    uint64_t rearmAt = WebcamNowNs();
    dispatch_source_set_timer(sess.timer,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)(next > rearmAt ? next - rearmAt : 0)),
                              (uint64_t)period, 0);

    if (gWebcam.phaseLock.rearms < kWcRearmLogFirst || gWebcam.phaseLock.rearms % kWcRearmLogEvery == 0) {
        LOG(@"webcam: pump locked to the ring: next tick %.2f ms after the publish, period %.4f ms "
            @"(%@; offset mean was %.2f ms over %d)",
            kWcPhaseNs / 1e6, period / 1e6, reason, mean / 1e6, samples);
    }

    gWebcam.phaseLock.timer = (__bridge void *)sess.timer;
    gWebcam.phaseLock.rearmedAtNs = now;
    gWebcam.phaseLock.periodNs = (uint64_t)period;
    gWebcam.phaseLock.offsetCount = 0;
    gWebcam.phaseLock.rearms++;
}

// Frame `seq`, published at publishNs, was read at `now`. Re-arms the pump timer when the phase or the rate is off,
// at most once a second.
static void WebcamPhaseLock(SimcamSession *sess, uint64_t now, uint64_t publishNs, uint64_t seq) {
    int64_t freeRunOffset = (int64_t)(now - publishNs);

    if (WebcamPhaseLockOff()) {
        if (publishNs <= now && freeRunOffset < kWcPhaseMaxOffsetNs) {
            gWebcam.stats.publishToTickMs += freeRunOffset / 1e6;
        }
        return;
    }

    // a publish in the future or too long ago is no phase measurement
    if (!sess.timer || publishNs > now || now - publishNs > (uint64_t)kWcPhaseMaxOffsetNs) {
        return;
    }
    BOOL newTimer = gWebcam.phaseLock.timer != (__bridge void *)sess.timer;
    double period = WebcamPhaseLockPeriod(newTimer, publishNs, seq);
    int samples;
    int64_t mean = WebcamPhaseLockAddOffset((int64_t)(now - publishNs), &samples);

    BOOL offsetOutOfRange = samples >= kWcPhaseMinSamples && (mean < kWcPhaseLowNs || mean > kWcPhaseHighNs);
    BOOL rateChanged =
        gWebcam.phaseLock.periodNs && fabs(period - (double)gWebcam.phaseLock.periodNs) > period * kWcPeriodTolerance;
    if (!newTimer && !offsetOutOfRange && !rateChanged) {
        return;
    }
    if (!newTimer && now - gWebcam.phaseLock.rearmedAtNs < NSEC_PER_SEC) {
        return;
    }

    NSString *reason = @"rate";
    if (newTimer) {
        reason = @"new timer";
    } else if (offsetOutOfRange) {
        reason = @"offset out of 4-9 ms";
    }
    WebcamPhaseLockRearm(sess, now, publishNs, period, reason, mean, samples);
}

// Whether a new frame is due. Two things must hold:
//   - the pump is locked to the ring at 1x or 2x the camera's nominal rate (the ring header's fps);
//   - a camera interval, less 3 ms, has passed since the newest frame's publish.
// At 2x, the tick between two frames comes half an interval after the first, so it never waits.
static BOOL WebcamFrameDue(SimcamSession *sess, uint64_t now) {
    // not locked to the ring
    if (!sess || !sess.timer || gWebcam.phaseLock.timer != (__bridge void *)sess.timer) {
        return NO;
    }
    uint32_t fpsMilli = gWebcam.ring.reader.geom.fpsMilli;
    if (!gWebcam.rate.lastPublishNs || now < gWebcam.rate.lastPublishNs || !fpsMilli) {
        return NO;
    }
    double nominal = 1e9 / PumpFps();
    double interval = 1e12 / fpsMilli;   // ns per frame: fpsMilli is frames per 1000 s
    BOOL pumpAtCameraRate = fabs(interval - nominal) < nominal * kWcDueTolerance;
    BOOL pumpAtTwiceCameraRate = fabs(interval - 2 * nominal) < 2 * nominal * kWcDueTolerance;
    if (!pumpAtCameraRate && !pumpAtTwiceCameraRate) {
        return NO;
    }
    return (double)(now - gWebcam.rate.lastPublishNs) + kWcDueEarlyNs >= interval;
}

// Two reusable frame buffers, gWebcam.frameBufs. One is busy while a CGImage made from it lives; its provider's release
// clears that. With both busy, a frame gets a one-off malloc that the release frees.
static void WebcamFrameBufferRelease(void *info, const void *data, size_t size) {
    if (info) {
        atomic_store(&((WebcamFrameBuffer *)info)->busy, 0);
    } else {
        free((void *)data);
    }
}

static uint8_t *WebcamFrameBufferGet(size_t len, WebcamFrameBuffer **owner) {
    *owner = NULL;
    for (int i = 0; i < kWcFrameBufCount; i++) {
        WebcamFrameBuffer *buf = &gWebcam.frameBufs[i];
        if (atomic_load(&buf->busy)) {
            continue;
        }
        if (buf->len != len) {
            free(buf->bytes);
            buf->bytes = malloc(len);
            buf->len = buf->bytes ? len : 0;
        }
        if (buf->bytes) {
            *owner = buf;
        }
        return buf->bytes;
    }
    return malloc(len);
}

static void WebcamFrameBuffersFree(void) {   // source left: release the frame buffers no CGImage holds any more
    for (int i = 0; i < kWcFrameBufCount; i++) {
        if (!atomic_load(&gWebcam.frameBufs[i].busy)) {
            free(gWebcam.frameBufs[i].bytes);
            gWebcam.frameBufs[i].bytes = NULL;
            gWebcam.frameBufs[i].len = 0;
        }
    }
}

#include "webcam_path.inc"

// Every kWcStatIntervalNs: the "webcam stat" line, then the counters start again
static const uint64_t kWcStatIntervalNs = 5 * NSEC_PER_SEC;

// average: sum / count, 0 when count is 0
static double WebcamStatsAverage(double sum, long count) { return count ? sum / count : 0; }

// The "webcam stat" line is "webcam stat <seconds>s: " and these five parts, joined by single spaces. Scripts read the
// line (scripts/webcam-app.sh), so its text stays as it is.

// What the ticks found: new frames, the last one again (dup), frames skipped, torn reads, ticks while offline.
static NSString *WebcamStatsFramesPart(const WebcamStats *stats) {
    return [NSString stringWithFormat:@"ticks %ld new %ld dup %ld skipped %ld torn %ld offline %ld", stats->ticks,
                                      stats->newFrames, stats->repeatedFrames, stats->skippedFrames, stats->tornReads,
                                      stats->offlineTicks];
}

// Per new frame: the copy out of the ring, and the age of the frame at its tick, from its capture and its publish.
static NSString *WebcamStatsLatencyPart(const WebcamStats *stats) {
    double publishToTickMinMs = stats->newFrames ? stats->publishToTickMinMs : 0;
    return [NSString stringWithFormat:@"copy %.2f ms capture->tick avg %.1f max %.1f ms "
                                      @"publish->tick avg %.2f min %.2f max %.2f ms",
                                      WebcamStatsAverage(stats->copyMs, stats->newFrames),
                                      WebcamStatsAverage(stats->captureToTickMs, stats->newFrames),
                                      stats->captureToTickMaxMs,
                                      WebcamStatsAverage(stats->publishToTickMs, stats->newFrames), publishToTickMinMs,
                                      stats->publishToTickMaxMs];
}

// The phase lock: its re-arms so far (a running total) and the period it set.
static NSString *WebcamStatsPhaseLockPart(void) {
    return [NSString stringWithFormat:@"rearms %ld period %.4f ms", gWebcam.phaseLock.rearms,
                                      gWebcam.phaseLock.periodNs / 1e6];
}

// The frame path: its backend, the average render and renders per tick, and the average tick.
static NSString *WebcamStatsPathPart(const WebcamStats *stats) {
    double rendersPerTick = stats->ticks ? (double)stats->renders / stats->ticks : 0;
    return [NSString stringWithFormat:@"path %s render %.2f ms x %.1f tick %.2f ms",
                                      WebcamPathName(gWebcamPath.backend),
                                      WebcamStatsAverage(stats->renderMs, stats->renders), rendersPerTick,
                                      WebcamStatsAverage(stats->tickMs, stats->timedTicks)];
}

// Late frames: found out of waited for, and the average wait for the ones found.
static NSString *WebcamStatsLatePart(const WebcamStats *stats) {
    return [NSString stringWithFormat:@"late %ld/%ld %.2f ms", stats->lateFound, stats->lateFound + stats->lateMissed,
                                      WebcamStatsAverage(stats->lateWaitMs, stats->lateFound)];
}

static void WebcamStatsTick(uint64_t now) {
    WebcamStats *stats = &gWebcam.stats;
    if (!stats->sinceNs) {
        stats->sinceNs = now;
    }
    if (now - stats->sinceNs < kWcStatIntervalNs) {
        return;
    }
    double seconds = (now - stats->sinceNs) / 1e9;
    LOG(@"webcam stat %.1fs: %@ %@ %@ %@ %@", seconds, WebcamStatsFramesPart(stats), WebcamStatsLatencyPart(stats),
        WebcamStatsPhaseLockPart(), WebcamStatsPathPart(stats), WebcamStatsLatePart(stats));
    *stats = (WebcamStats){.sinceNs = now, .publishToTickMinMs = kWcStatsMinStartMs};
}

// A late frame: poll the ring up to kWcLateNs for a frame newer than the last one shown. If one is found, *peek is
// updated, and the time it was found is returned as the tick's time; else `now` is returned.
static uint64_t WebcamWaitForLateFrame(scw_peek_t *peek, uint64_t now) {
    const scw_shm_reader *reader = &gWebcam.ring.reader;
    uint64_t waitStart = WebcamNowNs();
    uint64_t until = waitStart + kWcLateNs;
    scw_peek_t polled;
    while (WebcamNowNs() < until) {
        usleep(kWcLatePollUs);
        if (scw_peek(reader->map, &reader->geom, &polled) != SCW_OK) {
            break;
        }
        if (polled.latest != gWebcam.ring.lastSeq) {
            *peek = polled;
            break;
        }
    }
    if (peek->latest == gWebcam.ring.lastSeq) {
        gWebcam.stats.lateMissed++;
        return now;
    }
    uint64_t foundAt = WebcamNowNs();
    gWebcam.stats.lateFound++;
    gWebcam.stats.lateWaitMs += (foundAt - waitStart) / 1e6;
    return foundAt;
}

// The frame path's read: the newest frame copied once into a pooled ring-sized buffer, gWebcam.frame.buffer.
static int WebcamReadIntoBuffer(scw_slot_read *slotRead) {
    CVPixelBufferRef pathFrame = NULL;
    int rc = WebcamCopyFrame(gWebcam.ring.lastSeq, slotRead, &pathFrame);
    if (rc == SCW_OK) {
        WebcamDropFrame();
        gWebcam.frame.buffer = pathFrame;
        gWebcam.frame.bufferGen++;
    }
    return rc;
}

// Device RGB, made on first use and kept for the life of the process.
static CGColorSpaceRef WebcamDeviceRGB(void) {
    static CGColorSpaceRef deviceRGB;
    if (!deviceRGB) {
        deviceRGB = CGColorSpaceCreateDeviceRGB();
    }
    return deviceRGB;
}

// The CG canvas's read: the newest frame as a CGImage that scene.inc draws, gWebcam.frame.image. Its bytes are one of
// the reusable frame buffers, or a one-off malloc while both are busy.
static int WebcamReadIntoImage(scw_slot_read *slotRead) {
    const scw_shm_reader *reader = &gWebcam.ring.reader;
    size_t bytesPerRow = (size_t)reader->geom.width * kWcBytesPerPixel;
    size_t len = bytesPerRow * reader->geom.height;
    WebcamFrameBuffer *owner = NULL;
    uint8_t *bytes = WebcamFrameBufferGet(len, &owner);
    if (!bytes) {
        return SCW_E_ARG;
    }
    int rc = scw_read_latest(reader->map, &reader->geom, gWebcam.ring.lastSeq, bytes, bytesPerRow, len, slotRead);
    if (rc != SCW_OK) {
        if (!owner) {
            free(bytes);   // a one-off buffer; a reusable one waits for the next frame
        }
        return rc;
    }

    CGColorSpaceRef deviceRGB = WebcamDeviceRGB();
    if (owner) {
        atomic_store(&owner->busy, 1);
    }
    CGDataProviderRef provider = CGDataProviderCreateWithData(owner, bytes, len, WebcamFrameBufferRelease);
    CGImageRef image = CGImageCreate(reader->geom.width, reader->geom.height, kWcBitsPerComponent, kWcBitsPerPixel,
                                     bytesPerRow, deviceRGB,
                                     kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, NULL, false,
                                     kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    if (image) {
        WebcamDropFrame();
        gWebcam.frame.image = image;
    }
    return rc;
}

// Reads the newest ring frame for the frame path or for scene.inc's canvas. It returns what scw_read_latest returns. On
// SCW_OK, *slotRead is filled. On anything else, the frame shown stays.
static int WebcamReadFrame(scw_slot_read *slotRead) {
    return WebcamPathOn() ? WebcamReadIntoBuffer(slotRead) : WebcamReadIntoImage(slotRead);
}

// A new frame was read (the copy started at copyStart): its counters and timing stats, and the phase lock. Returns YES
// when barcode detection should rerun: a moving picture is scanned again on every kWcRescanEveryFrames-th new frame.
static BOOL WebcamCountNewFrame(SimcamSession *sess, uint64_t now, uint64_t copyStart, const scw_slot_read *slotRead) {
    if (gWebcam.ring.lastSeq && slotRead->seq > gWebcam.ring.lastSeq + 1) {
        gWebcam.stats.skippedFrames += (long)(slotRead->seq - gWebcam.ring.lastSeq - 1);
    }
    gWebcam.rate.lastPublishNs = slotRead->publishNs;
    gWebcam.ring.lastSeq = slotRead->seq;
    gWebcam.stats.newFrames++;
    gWebcam.stats.copyMs += (WebcamNowNs() - copyStart) / 1e6;
    double captureToTickMs = (int64_t)(now - slotRead->captureNs) / 1e6;
    gWebcam.stats.captureToTickMs += captureToTickMs;
    if (captureToTickMs > gWebcam.stats.captureToTickMaxMs) {
        gWebcam.stats.captureToTickMaxMs = captureToTickMs;
    }
    BOOL rescan = gWebcam.stats.newFrames % kWcRescanEveryFrames == 1;

    WebcamPhaseLock(sess, now, slotRead->publishNs, slotRead->seq);
    double publishToTickMs = (int64_t)(now - slotRead->publishNs) / 1e6;
    if (publishToTickMs < gWebcam.stats.publishToTickMinMs) {
        gWebcam.stats.publishToTickMinMs = publishToTickMs;
    }
    if (publishToTickMs > gWebcam.stats.publishToTickMaxMs) {
        gWebcam.stats.publishToTickMaxMs = publishToTickMs;
    }
    return rescan;
}

// A tick with the ring not live: the offline state, its log lines, and the last frame dropped after kWcFreezeNs
static void WebcamOfflineTick(uint64_t now) {
    const scw_shm_reader *reader = &gWebcam.ring.reader;
    gWebcam.stats.offlineTicks++;
    if (gWebcam.ring.open && reader->status != gWebcam.feed.offlineStatus) {
        if (reader->status == SCW_E_LIMIT) {
            LOG(@"%s", reader->why);
        }
        gWebcam.feed.offlineStatus = reader->status;
    }
    if (gWebcam.feed.live || !gWebcam.feed.offlineSinceNs) {
        gWebcam.feed.offlineSinceNs = now;
        LOG(@"webcam: offline (%s)%@", gWebcam.ring.open ? reader->why : "not opened",
            gWebcam.frame.image || gWebcam.frame.buffer ? @"; keeping the last frame for 1 s" : @"");
    }
    gWebcam.feed.live = NO;
    if ((gWebcam.frame.image || gWebcam.frame.buffer) && now - gWebcam.feed.offlineSinceNs >= kWcFreezeNs) {
        WebcamDropFrame();
        LOG(@"webcam: offline band on (%@)", WebcamOfflineReason());
    }
}

// Sets this tick's presentation time, gWebcam.tick.ptsNs. With a new frame (captureNs is not 0), it is that frame's
// capture time. Between frames, it is the tick time less the same lag.
// Until the first frame, the lag grows by a quarter period each tick, up to 200 ms. The first frame's capture time is
// at most 200 ms back, so it can't fall behind the band frames shown before it.
static void WebcamNextPts(uint64_t now, uint64_t captureNs) {
    if (captureNs) {
        uint64_t capture = captureNs > now ? now : captureNs;
        if (now - capture > kWcMaxLagNs) {
            capture = now - kWcMaxLagNs;
        }
        gWebcam.tick.lagNs = now - capture;
        gWebcam.feed.hadFrame = YES;
    }
    uint64_t pts = now - MIN(gWebcam.tick.lagNs, kWcMaxLagNs);
    if (pts <= gWebcam.tick.ptsNs) {
        pts = gWebcam.tick.ptsNs + kWcMinPtsStepNs;
    }
    gWebcam.tick.ptsNs = pts;
    if (!gWebcam.feed.hadFrame) {
        gWebcam.tick.lagNs = MIN(kWcMaxLagNs, gWebcam.tick.lagNs + (uint64_t)(NSEC_PER_SEC / PumpFps()) / 4);
    }
}

// The format the session wants, packed for the reader file as the kWcWant* constants describe.
static uint64_t WebcamWantedFormat(SimcamSession *sess) {
    uint64_t width = (uint64_t)(sess.formatWidth & kWcWantDimMask);
    uint64_t height = (uint64_t)(sess.formatHeight & kWcWantDimMask);
    uint64_t fps = (uint64_t)lround(sess.fps) & kWcWantFpsMask;
    return (width << kWcWantWidthShift) | (height << kWcWantHeightShift) | fps;
}

// A tick starts: the direct-canvas flag clears, the stats count it, and the reader file learns that the pump ticked
// and the format the session wants.
static void WebcamTickBegin(SimcamSession *sess, uint64_t now) {
    gWebcamPath.canvas.direct = NO;
    gWebcam.stats.ticks++;
    atomic_store(&gWebcam.readerFile.lastTickNs, now);
    if (sess) {
        atomic_store(&gWebcam.readerFile.wantedFormat, WebcamWantedFormat(sess));
    }
}

// A new ring epoch means a new helper: its rate and phase are measured afresh.
static void WebcamNoteEpoch(void) {
    const scw_shm_reader *reader = &gWebcam.ring.reader;
    if (reader->geom.epoch == gWebcam.ring.epoch) {
        return;
    }
    gWebcam.ring.epoch = reader->geom.epoch;
    gWebcam.ring.lastSeq = 0;
    gWebcam.rate.baseSeq = 0;
    gWebcam.phaseLock.offsetCount = 0;
    gWebcam.rate.lastPublishNs = 0;
    LOG(@"webcam: ring %s %ux%u from '%s' (helper pid %d, epoch %016llx)", reader->name, reader->geom.width,
        reader->geom.height, reader->geom.device, reader->geom.producerPid, reader->geom.epoch);
}

// What a live tick got from the ring (WebcamLiveTick).
typedef struct {
    uint64_t now;          // the tick's time: when a late frame was found, if the tick waited for one
    uint64_t captureNs;    // the new frame's capture time; 0: no new frame
    BOOL rescan;           // the new frame reruns barcode detection
} WebcamRingRead;

// Reads the newest frame if peek found one newer than the last shown, and counts what the tick got: a new frame, the
// last one again, or a torn read.
static void WebcamTakeFrame(SimcamSession *sess, const scw_peek_t *peek, WebcamRingRead *read) {
    scw_slot_read slotRead;
    uint64_t copyStart = WebcamNowNs();
    // no new frame: no buffer, no copy
    int rc = peek->latest == gWebcam.ring.lastSeq ? SCW_NONE : WebcamReadFrame(&slotRead);
    if (rc == SCW_OK) {
        read->captureNs = slotRead.captureNs;
        read->rescan = WebcamCountNewFrame(sess, read->now, copyStart, &slotRead);
    } else if (rc == SCW_NONE) {
        gWebcam.stats.repeatedFrames++;
    } else if (rc == SCW_E_TORN) {
        gWebcam.stats.tornReads++;
    }
}

// The ring is live: the first live tick logs it, and the offline state and band text are cleared.
static void WebcamNoteLive(void) {
    if (!gWebcam.feed.live) {
        const scw_geom *geom = &gWebcam.ring.reader.geom;
        LOG(@"webcam: live ('%s' %ux%u)", geom->device, geom->width, geom->height);
    }
    gWebcam.feed.live = YES;
    gWebcam.feed.everLive = YES;
    gWebcam.feed.offlineSinceNs = 0;
    gWebcam.feed.bandText = nil;
}

// A tick with the ring live, at `now`, where peek says what the ring holds.
static WebcamRingRead WebcamLiveTick(SimcamSession *sess, uint64_t now, scw_peek_t peek) {
    WebcamRingRead read = {.now = now};
    WebcamNoteEpoch();
    // a late frame: wait a little for it rather than show a duplicate now and skip a frame next tick
    if (peek.latest == gWebcam.ring.lastSeq && WebcamFrameDue(sess, now)) {
        read.now = WebcamWaitForLateFrame(&peek, now);
    }
    WebcamTakeFrame(sess, &peek, &read);
    WebcamNoteLive();
    return read;
}

// The frame this tick shows (borrowed): the frame path's buffer, else the CGImage; with neither, the offline band.
static WebcamTickFrame WebcamShownFrame(void) {
    WebcamTickFrame shown = {0};
    if (gWebcam.frame.buffer) {
        shown.buffer = gWebcam.frame.buffer;
        return shown;
    }
    if (gWebcam.frame.image) {
        shown.image = gWebcam.frame.image;
        return shown;
    }
    NSString *why = WebcamOfflineReason();
    if (![why isEqualToString:gWebcam.feed.bandText ?: @""]) {
        gWebcam.feed.bandText = why;
    }
    shown.band = [@"webcam offline: " stringByAppendingString:gWebcam.feed.bandText];
    return shown;
}

// One tick: read the ring (live) or count the tick offline, set the presentation time, and return the frame to show.
static WebcamTickFrame WebcamAdvance(SimcamSession *sess) {
    uint64_t now = WebcamNowNs();
    WebcamTickBegin(sess, now);
    WebcamStatsTick(now);
    WebcamResolveRing(now);

    scw_peek_t peek;
    BOOL live = gWebcam.ring.open && scw_shm_reader_poll(&gWebcam.ring.reader, now, &peek);
    WebcamRingRead read = {.now = now};
    if (live) {
        read = WebcamLiveTick(sess, now, peek);
    } else {
        WebcamOfflineTick(now);
    }
    WebcamNextPts(read.now, read.captureNs);

    WebcamTickFrame shown = WebcamShownFrame();
    shown.rescan = read.rescan;
    return shown;
}

// the offline band's geometry
#define kWcBandMinHeight 36            // points
#define kWcBandHeightShare 0.075       // of the canvas's shorter side
#define kWcBandTopShare 0.07           // of the canvas's height
#define kWcBandFontShare 0.42          // of the band's height
#define kWcBandTextShare 0.7           // the widest the text may be, as a share of the canvas's width

// The offline band near the top of the upright picture, above the scene's colour bar and below a phone's status bar.
// The text stays in the central 70 %, which aspect-fill previews don't crop. Canvas coordinates: origin top-left, y
// down.
static void WebcamDrawOfflineBand(CGContextRef ctx, double width, double height, NSString *text) {
    double bandHeight = MAX(kWcBandMinHeight, MIN(width, height) * kWcBandHeightShare);
    double bandTop = height * kWcBandTopShare;
    CGContextSetRGBFillColor(ctx, 0.75, 0.08, 0.16, 1);   // a deep red
    CGContextFillRect(ctx, CGRectMake(0, bandTop, width, bandHeight));
    UIGraphicsPushContext(ctx);
    double fontSize = bandHeight * kWcBandFontShare;
    NSDictionary *attrs = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fontSize],
                            NSForegroundColorAttributeName: UIColor.whiteColor};
    CGSize size = [text sizeWithAttributes:attrs];
    if (size.width > width * kWcBandTextShare) {
        fontSize *= width * kWcBandTextShare / size.width;
        attrs = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fontSize],
                  NSForegroundColorAttributeName: UIColor.whiteColor};
        size = [text sizeWithAttributes:attrs];
    }
    [text drawAtPoint:CGPointMake((width - size.width) / 2, bandTop + (bandHeight - size.height) / 2)
        withAttributes:attrs];
    UIGraphicsPopContext();
}
