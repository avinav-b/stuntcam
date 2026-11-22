// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): pause-aware op watchdog and crash sentinel.
//
// Provides: SentinelOpBegin, SentinelOpEnd, SentinelLatched, the op slots (providers/pump.inc, providers/photo.inc)
//           SentinelFeatureEnter, SentinelFeatureLeave, SentinelFeatureClearUnless, SentinelFeatureRestoreUnless,
//           SENTINEL_FEATURE_SCOPE, SENTINEL_MOTION_SCOPE, the feature key (client/motion.inc, core/lazy.inc)
//           SentinelFormatEvent, SentinelWriteEvent, kEventField*, the events file (core/trace.inc)
//           SentinelSessionErrorAllowed, kSentinelLatchStatus, kSentinelLatchAuto (providers/, core/control.inc)
// Owns:     gSentinel, the sentinel's state, kFeatureNames and kMotionFeatureKey, all defined in core/sentinel.h (which
//           says who owns what)
// Uses:     core/trace.inc, core/control.inc (TraceCrash, TraceLatch, SessInjectRuntimeError, MovieStopAll, which
//           core/trace.h and core/control.h declare)
//
// The sentinel stops a camera feature that hangs or crashes from doing it again. Ops bracket the work the engine owns,
// and the watchdog turns an op past its deadline into a hang. The crash handler attributes a crash to a feature. Two
// hangs of one feature in this process latch it at once; two crashes or hangs in recent activations latch it at the
// next activation. Each rule is stated next to the code that applies it.
// This file holds the shared state, the thread-specific feature key, the async-signal-safe event writer, the ops and
// the watchdog. The rest of the sentinel is included right after it, in this order:
//   core/crash_handler.inc      image ranges, the minimal and full signal handlers
//   core/markers.inc            load-time state and the latched-*/preflight-fail-* markers
//   core/events_file.inc        the trim, the locked append, the record queue's writer
//   core/sentinel_activate.inc  PREPARE's sentinel step (read the past, decide latches) and the sentinel's START step
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/ucontext.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <sys/stat.h>
#include <os/lock.h>
#include <mach-o/dyld.h>
#include <stdatomic.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <sys/file.h>
#include "sentinel.h"
#include "trace.h"
#include "control.h"

#define kSentinelMaxDigits 24   // room for the decimal digits of an unsigned long

// What a latch does and the latch windows are stated in core/sentinel.h, with the constants other fragments use.
static BOOL SentinelSessionErrorAllowed(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t prev = atomic_load(&gSentinel.sessionErrorAtNs);
    if (prev && now - prev < kSentinelSessionErrorIntervalNs) {
        return NO;
    }
    return atomic_compare_exchange_strong(&gSentinel.sessionErrorAtNs, &prev, now);
}

static int SentinelFeatureIndex(const char *feature) {
    for (int i = 0; i < kFeatureCount; i++) {
        if (!strcmp(kFeatureNames[i], feature)) {
            return i;
        }
    }
    return kFeatureEngine;
}

static BOOL SentinelClosesCamera(int featureIndex) { return featureIndex != kFeatureMotion; }

// ---- the thread-specific feature key: a fault on a thread inside a motion entry point counts as `motion`, ahead of
// any inflight op ----
// Each motion entry point saves the key, sets `motion` and restores it on exit, so a value set further up the thread
// survives; around the app's handler it is saved, cleared and restored, so a fault in app code never counts.
// SentinelOnSignal reads it first. SIMCAM_OFF_MOTION_KEY=1 never creates the key (it is made at load), so a motion
// fault in our image counts as `provider`, which closes the camera.
static void SentinelFeatureKeyInit(void) {
    if (!MotionKeyOn()) {
        return;
    }
    if (pthread_key_create(&gSentinel.featureKey.key, NULL) == 0) {
        atomic_store(&gSentinel.featureKey.on, 1);
    }
}

static inline void *SentinelFeatureEnter(const char *feature) {   // returns the saved value for SentinelFeatureLeave
    if (!atomic_load_explicit(&gSentinel.featureKey.on, memory_order_relaxed)) {
        return NULL;
    }
    void *saved = pthread_getspecific(gSentinel.featureKey.key);
    // pthread_setspecific fails only for an invalid key or with no memory left: a fault then counts as the inflight
    // op's, as without the key
    if (saved != (void *)feature) {
        (void)pthread_setspecific(gSentinel.featureKey.key, feature);
    }
    return saved;
}

static inline void SentinelFeatureLeave(void *saved) {
    if (!atomic_load_explicit(&gSentinel.featureKey.on, memory_order_relaxed)) {
        return;
    }
    if (pthread_getspecific(gSentinel.featureKey.key) != saved) {
        (void)pthread_setspecific(gSentinel.featureKey.key, saved);   // as in SentinelFeatureEnter
    }
}

static inline void SentinelFeatureLeaveCleanup(void **savedSlot) { SentinelFeatureLeave(*savedSlot); }

// Code that must not count as the thread's feature (app code reached from a motion entry point, or the orientation
// tracker, which is camera code) runs with the key cleared: SentinelFeatureClearUnless(keep) before it,
// SentinelFeatureRestoreUnless(keep, saved) after. `keep` is the off switch that leaves the key set there instead.
static inline void *SentinelFeatureClearUnless(BOOL keep) { return keep ? NULL : SentinelFeatureEnter(NULL); }

static inline void SentinelFeatureRestoreUnless(BOOL keep, void *saved) {
    if (!keep) {
        SentinelFeatureLeave(saved);
    }
}

// ---- async-signal-safe event writer (no stdio, no malloc) ----
// SentinelPutString and SentinelPutUnsigned append a string or an unsigned decimal to buf, which has len bytes used of
// cap (one byte is kept free for the caller's '\n'), and return the new length.
static int SentinelPutString(char *buf, int len, int cap, const char *text) {
    while (text && *text && len < cap - 1) {
        buf[len++] = *text++;
    }
    return len;
}

static int SentinelPutUnsigned(char *buf, int len, int cap, unsigned long value) {
    char digits[kSentinelMaxDigits];
    int count = 0;
    do {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while (value && count < kSentinelMaxDigits);
    while (count && len < cap - 1) {
        buf[len++] = digits[--count];
    }
    return len;
}

// The events file, $SIMCAM_STATE_DIR/<bundle id>/events.log (else <container tmp>/simcam-state/…), has one line per
// event:
//   <epoch> <activation#> <kind> <feature> <build> <detail>[<num>]
// The kinds are activate (detail traced=0|1), exit, op-begin, op-end, hang, unknown-hang, crash, crash-traced and
// crash-other (neither is ever counted), and clear (the past before it is forgotten).
// SentinelFormatEvent formats one line. A negative num adds no number.
static int SentinelFormatEvent(char *buf, int cap, long act, const char *kind, const char *feature, const char *detail,
                               long num) {
    int len = 0;
    len = SentinelPutUnsigned(buf, len, cap, (unsigned long)time(NULL));
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutUnsigned(buf, len, cap, (unsigned long)act);
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutString(buf, len, cap, kind);
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutString(buf, len, cap, feature ? feature : "-");
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutString(buf, len, cap, gSentinel.build);
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutString(buf, len, cap, detail ? detail : "");
    if (num >= 0) {
        len = SentinelPutUnsigned(buf, len, cap, (unsigned long)num);
    }
    buf[len++] = '\n';
    return len;
}

static void SentinelWriteEvent(const char *kind, const char *feature, const char *detail, long num) {
    int fd = gSentinel.events.fd;
    if (fd < 0) {
        return;
    }
    char line[kSentinelEventLineBytes];
    int len = SentinelFormatEvent(line, sizeof line, gSentinel.events.activationNumber, kind, feature, detail, num);
    // also called from the crash handler: nothing async-signal-safe can be done about a failed write, so the line is
    // lost
    (void)write(fd, line, len);
}

// ---- ops ----
// SentinelOpBegin(feature, timeout) and SentinelOpEnd(slot) bracket work the engine owns: configure, start and stop,
// and a photo until its still-complete is posted. The watchdog below turns an op past its deadline into a hang, which
// the events file keeps for the next activation. The timeout is gSentinel.scale times longer: ten with TSan or ASan
// loaded.
static int SentinelOpBegin(const char *feature, double timeoutSec) {
    if (!atomic_load(&gSentinel.live)) {
        return -1;
    }
    os_unfair_lock_lock(&gSentinel.ops.lock);
    int slot = -1;

    for (int i = 0; i < kSentinelMaxOps; i++) {
        if (!atomic_load(&gSentinel.ops.slots[i].used)) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        SentinelOp *op = &gSentinel.ops.slots[slot];
        op->feature = feature;
        op->timeout = (uint64_t)(timeoutSec * gSentinel.scale * NSEC_PER_SEC);
        op->expired = 0;
        op->deadline = atomic_load(&gSentinel.ops.activeClockNs) + op->timeout;
        atomic_store(&op->used, 1);
    }

    os_unfair_lock_unlock(&gSentinel.ops.lock);
    if (slot >= 0) {
        SentinelWriteEvent("op-begin", feature, "slot=", slot);
    }
    return slot;
}

static void SentinelOpEnd(int slot) {
    if (slot < 0 || slot >= kSentinelMaxOps) {
        return;
    }
    const char *feature = gSentinel.ops.slots[slot].feature;
    os_unfair_lock_lock(&gSentinel.ops.lock);
    atomic_store(&gSentinel.ops.slots[slot].used, 0);
    os_unfair_lock_unlock(&gSentinel.ops.lock);
    SentinelWriteEvent("op-end", feature, "slot=", slot);
}

static BOOL SentinelLatched(const char *feature) {
    return atomic_load(&gSentinel.latchedNow[SentinelFeatureIndex(feature)]) != 0;
}

static BOOL ProcessIsTraced(void) {
    struct kinfo_proc procInfo;
    size_t len = sizeof procInfo;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &procInfo, &len, NULL, 0) == 0 && (procInfo.kp_proc.p_flag & P_TRACED);
}

// ---- the watchdog ----
// A thread that ticks every 250 ms and measures the ops' deadlines on an active clock. A gap of more than 1 s between
// ticks means the process was stopped (debugger, suspension, Mac sleep): every inflight deadline restarts, and the
// clock advances by one tick only. While the process is traced (P_TRACED), an expired op is recorded as unknown-hang
// and never counted.
#define kSentinelWatchdogTickNs (250 * NSEC_PER_MSEC)

// The debugger check, every tick: a change is logged. Hangs while traced are noted, not counted.
static int SentinelWatchdogNoteTraced(void) {
    int traced = ProcessIsTraced();
    if (traced != atomic_load(&gSentinel.traced)) {
        atomic_store(&gSentinel.traced, traced);
        LOG(@"watchdog: debugger %s", traced ? "attached" : "detached");
    }
    return traced;
}

// Under the ops lock: after a stop, restarts every deadline; then marks the ops now over their deadline as expired
// (each once) and puts their features in expired. Returns how many.
static int SentinelWatchdogCollectExpired(uint64_t active, BOOL stopped, const char *expired[kSentinelMaxOps]) {
    os_unfair_lock_lock(&gSentinel.ops.lock);
    if (stopped) {
        for (int i = 0; i < kSentinelMaxOps; i++) {
            if (atomic_load(&gSentinel.ops.slots[i].used)) {
                gSentinel.ops.slots[i].deadline = active + gSentinel.ops.slots[i].timeout;
            }
        }
    }
    int nExpired = 0;
    for (int i = 0; i < kSentinelMaxOps; i++) {
        SentinelOp *op = &gSentinel.ops.slots[i];
        if (atomic_load(&op->used) && !op->expired && active > op->deadline) {
            op->expired = 1;
            expired[nExpired++] = op->feature;
        }
    }
    os_unfair_lock_unlock(&gSentinel.ops.lock);
    return nExpired;
}

// An op over its deadline: a hang of its feature, which latches at kSentinelStrikesToLatch hangs in this process.
static void SentinelWatchdogHang(const char *feature, int traced) {
    if (traced) {
        SentinelWriteEvent("unknown-hang", feature, "traced", -1);
        LOG(@"watchdog: %s op over its deadline while a debugger is attached (not counted)", feature);
        return;
    }
    int featureIndex = SentinelFeatureIndex(feature);
    int hangs = atomic_fetch_add(&gSentinel.hangs[featureIndex], 1) + 1;
    SentinelWriteEvent("hang", feature, "n=", hangs);
    LOG(@"watchdog: %s op hung (%d in this process)", feature, hangs);
    if (hangs < kSentinelStrikesToLatch || atomic_exchange(&gSentinel.latchedNow[featureIndex], 1)) {
        return;
    }
    LOG(@"watchdog: %s latched after %d hangs", feature, hangs);
    TraceLatch(featureIndex);
    // a session latch stops running sessions, with the runtime error if one is allowed; a movie latch stops active
    // recordings
    if (featureIndex == kFeatureSession) {
        SessInjectRuntimeError(kSentinelLatchAuto, NO);
    } else if (featureIndex == kFeatureMovie) {
        // Not reached yet: no movie op is bracketed with SentinelOpBegin("movie"), so a movie latch can only come from
        // an earlier activation's events (a known gap).
        MovieStopAll(kSentinelLatchStatus);
    }
    fprintf(stderr, "simcam: %s latched after %d hangs in this process\n", feature, hangs);
}

// One tick: sleep, check for a debugger, move the active clock on, and count the ops that ran over their deadline.
// *last is the previous tick's monotonic time.
static void SentinelWatchdogTick(uint64_t *last) {
    struct timespec ts = {0, (long)kSentinelWatchdogTickNs};
    nanosleep(&ts, NULL);
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t gap = now - *last;
    *last = now;
    int traced = SentinelWatchdogNoteTraced();
    // a gap over 1 s means the process was stopped, suspended or asleep: the active clock advances by one tick only
    BOOL stopped = gap > NSEC_PER_SEC;
    uint64_t advance = stopped ? kSentinelWatchdogTickNs : gap;
    uint64_t active = atomic_fetch_add(&gSentinel.ops.activeClockNs, advance) + advance;
    const char *expired[kSentinelMaxOps];
    int nExpired = SentinelWatchdogCollectExpired(active, stopped, expired);
    if (stopped) {
        LOG(@"watchdog: process was stopped for %.1f s; deadlines restarted", gap / 1e9);
    }
    for (int i = 0; i < nExpired; i++) {
        SentinelWatchdogHang(expired[i], traced);
    }
}

// The watchdog thread, started by SentinelStart: one SentinelWatchdogTick every 250 ms, for the life of the process.
static void *SentinelWatchdog(void *arg) {
    (void)pthread_setname_np("simcam.watchdog");   // the name is only for debuggers and crash reports
    uint64_t last = clock_gettime_nsec_np(CLOCK_MONOTONIC);   // includes Mac sleep, so sleep shows up as a gap
    for (;;) {
        @autoreleasepool {
            SentinelWatchdogTick(&last);
        }
    }

    return NULL;
}
