// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): pause-aware op watchdog and crash sentinel
// (design §5.4).
// This file: the overview below, the shared sentinel state, the thread-specific feature key, the async-signal-safe
// event writer, ops and the watchdog. The rest of the sentinel, included right after it, in this order:
//   core/crash_handler.inc      image ranges, the minimal and full signal handlers
//   core/markers.inc            load-time state and the latched-*/preflight-fail-* markers
//   core/events_file.inc        the trim, the locked append, the record queue's writer
//   core/sentinel_activate.inc  PREPARE 1 (read the past, decide latches) and START 1-3
//
// Ops. SCOpBegin(feature, timeout) / SCOpEnd(op) bracket work the engine owns (configure/start/stop, a photo until its
// still-complete is posted). A watchdog thread ticks every 250 ms and measures deadlines on an *active* clock:
//   - a gap > 1 s between ticks means the process was stopped (debugger, suspension, Mac sleep): every inflight
//     deadline restarts and the clock advances by one tick only;
//   - while P_TRACED, an expired op is recorded as unknown-hang and never counted;
//   - with TSan/ASan loaded, timeouts are x10.
// An expired op is a hang, recorded for the next activation. Two hangs of one feature in this process latch it
// in-process (SCLatched), and the latch error-completes (design §5.4 table) with SC_LATCH_STATUS, which apps get as
// AVFoundation's generic -11800 with that OSStatus underneath: photo -> every photo not yet encoded fails; movie ->
// active recordings stop with it and new ones fail at once; session -> running sessions get a runtime error and stop,
// and a latched startRunning gets at most one runtime error per 5 s (design §5.4; in between it is stopped without
// one). Before any session exists a latch means stock (the activation-time latch, below). Not wired yet: no movie op is
// bracketed with SCOpBegin("movie"), so the movie latch can only come from an earlier activation's events (TODO.md).
//
// Sentinel. A chained SIGSEGV/BUS/ILL/ABRT/TRAP/FPE handler attributes a crash, async-signal-safely, against image
// ranges cached at activation: the PC is in our image (feature = the inflight op's, else "provider"), or an op is
// inflight and the PC is in AVFCapture/AVFoundation/CMCapture/CoreMedia/CoreVideo/IOSurface/ImageIO/VideoToolbox
// (feature = the op's). Anything else is recorded as "crash-other" and never counted. It appends one line to the events
// file, then re-raises.
//
// Events file: $SIMCAM_STATE_DIR/<bundle id>/events.log (else <container tmp>/simcam-state/…), one line per event:
//   <epoch> <activation#> <kind> <feature> <build> <detail>
// kinds: activate (detail traced=0|1), exit, op-begin, op-end, hang, unknown-hang, crash, crash-traced (never counted),
// crash-other (never counted), clear.
// At activation: two crash/hang events of one feature with this build, within the last 20 activations and 24 h, latch
// it: the gate stays closed (no camera) and one stderr line says why. A previous traced activation that ended with an
// op inflight and no crash/exit line prints "last debug run ended inside <feature> op"; it is never counted.
// SIMCAM_SENTINEL=0 disables ops, watchdog and sentinel; SIMCAM_SENTINEL=clear appends a clear line (forgets the past).
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

#define SC_MAXOPS 64
// the OSStatus a latch error-completes with (under AVFoundation's generic -11800); also the default status of the
// control file's photo-error / movie-error test events
#define SC_LATCH_STATUS (-16800)
#define SC_STRIKES_TO_LATCH 2      // crashes/hangs of one feature (in this process, or in the window) that latch it
#define SC_WINDOW_SECONDS 86400    // 24 h: the latch window, and how long a marker is valid
#define SC_WINDOW_ACTIVATIONS 20   // the latch window's other bound: the last 20 activations
static void SessInjectRuntimeError(int status, BOOL serverDied);   // core/control.inc
static void MovieStopAll(int status);                // core/control.inc
static void TRCrash(int sig);                                // core/trace.inc (async-signal-safe)
static void TRLatch(int fi);                                 // core/trace.inc
// design §5.4: a latched session gets at most one runtime error per 5 s, whoever posts it (the watchdog at latch time,
// or a latched startRunning). Returns YES and stamps the time if one may be posted now.
static _Atomic uint64_t gSCSessErrAt;
// SessInjectRuntimeError(SC_LATCH_AUTO): the session latch's stop. Whether it carries the runtime error is decided on
// the pump queue when it finds a running session, so a latch that fires with no session running doesn't use up the 5 s
// window.
#define SC_LATCH_AUTO INT_MIN
static BOOL SCSessionErrorAllowed(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), prev = atomic_load(&gSCSessErrAt);
    if (prev && now - prev < 5 * NSEC_PER_SEC) return NO;
    return atomic_compare_exchange_strong(&gSCSessErrAt, &prev, now);
}
typedef struct {
    _Atomic int used;       // the slot holds an inflight op
    const char *feature;    // what a hang or crash in this op counts against (gSCFeatures)
    uint64_t deadline;      // on the active clock (gSCActiveNs)
    uint64_t timeout;       // the op's timeout (ns, sanitizer-scaled)
    int expired;            // the watchdog has recorded this op's hang
} SCOp;
static SCOp gSCOps[SC_MAXOPS];
static os_unfair_lock gSCLock = OS_UNFAIR_LOCK_INIT;
static _Atomic uint64_t gSCActiveNs;          // active clock (ns), advanced by the watchdog
static _Atomic int gSCOn, gSCTraced;
static double gSCScale = 1;
static int gSCFd = -1, gSCAct;                // events fd (O_APPEND), this activation's number
static char gSCBuild[32] = "-", gSCPath[1024];
// PREPARE 1: which gSCDir events.log opened in (0 state dir, 1 container tmp); this build's marker goes only there
static int gSCIdx = -1;
// `activate` (a timeout, crash or exception during activation) is a feature like the others: two strikes latch the
// camera. `motion` (a crash in the CoreMotion stand-in, attributed through the thread-specific feature key below)
// latches CoreMotion only: it never closes the camera, it only writes latched-motion-<build>.
enum {   // indexes into gSCFeatures
    SC_FSESSION = 0, SC_FPHOTO, SC_FMOVIE, SC_FPROVIDER, SC_FENGINE, SC_FACTIVATE, SC_FMOTION,
    SC_NFEAT
};
static const char *gSCFeatures[SC_NFEAT] = {"session", "photo", "movie", "provider", "engine", "activate", "motion"};
static _Atomic int gSCHangs[SC_NFEAT], gSCLatchedNow[SC_NFEAT];
static int SCFeatureIndex(const char *f) {
    for (int i = 0; i < SC_NFEAT; i++)
        if (!strcmp(gSCFeatures[i], f)) return i;
    return SC_FENGINE;
}
static BOOL SCClosesCamera(int fi) { return fi != SC_FMOTION; }

// ---- the thread-specific feature key (design §5.4 provider path (b)) ----
// Created at load. Every motion entry point saves the key's value, sets `motion` and restores the saved value on exit
// (never clears it, so a value set further up the same thread survives); around the call into the app's handler the key
// is saved, cleared and restored the same way, so a fault in app code never counts. SCOnSignal reads it first (a
// lock-free TSD read). SIMCAM_OFF_MOTION_KEY=1 (ledger L59) never creates it: a motion fault in our image is then
// `provider`, which closes the camera.
static const char kSCMotion[] = "motion";
static pthread_key_t gSCFeatKey;
static _Atomic int gSCFeatKeyOn;
static void SCFeatKeyInit(void) {
    if (getenv(ENV_OFF_MOTION_KEY)) return;
    if (pthread_key_create(&gSCFeatKey, NULL) == 0) atomic_store(&gSCFeatKeyOn, 1);
}
static inline void *SCFeatEnter(const char *f) {   // returns the saved value for SCFeatLeave
    if (!atomic_load_explicit(&gSCFeatKeyOn, memory_order_relaxed)) return NULL;
    void *saved = pthread_getspecific(gSCFeatKey);
    if (saved != (void *)f) pthread_setspecific(gSCFeatKey, f);
    return saved;
}
static inline void SCFeatLeave(void *sv) {
    if (!atomic_load_explicit(&gSCFeatKeyOn, memory_order_relaxed)) return;
    if (pthread_getspecific(gSCFeatKey) != sv) pthread_setspecific(gSCFeatKey, sv);
}
static inline void SCFeatLeaveP(void **sv) { SCFeatLeave(*sv); }
// a motion entry point: `motion` on this thread until the enclosing scope ends (restored on every exit path)
#define SC_MOTION_SCOPE void *_scMotionSaved __attribute__((cleanup(SCFeatLeaveP), unused)) = SCFeatEnter(kSCMotion)

// ---- async-signal-safe event writer (no stdio, no malloc) ----
// SCPutS / SCPutU append a string / an unsigned decimal to b (n bytes used of cap, one kept for the caller's '\n') and
// return the new length.
static int SCPutS(char *b, int n, int cap, const char *s) {
    while (s && *s && n < cap - 1) b[n++] = *s++;
    return n;
}
static int SCPutU(char *b, int n, int cap, unsigned long v) {
    char digits[24];
    int k = 0;
    do {
        digits[k++] = '0' + v % 10;
        v /= 10;
    } while (v && k < 24);
    while (k && n < cap - 1) b[n++] = digits[--k];
    return n;
}
// one events-file line: "<epoch> <activation#> <kind> <feature> <build> <detail>[<num>]\n" (num < 0: none)
static int SCFormat(char *b, int cap, long act, const char *kind, const char *feature, const char *detail, long num) {
    int n = 0;
    n = SCPutU(b, n, cap, (unsigned long)time(NULL));
    n = SCPutS(b, n, cap, " ");
    n = SCPutU(b, n, cap, (unsigned long)act);
    n = SCPutS(b, n, cap, " ");
    n = SCPutS(b, n, cap, kind);
    n = SCPutS(b, n, cap, " ");
    n = SCPutS(b, n, cap, feature ? feature : "-");
    n = SCPutS(b, n, cap, " ");
    n = SCPutS(b, n, cap, gSCBuild);
    n = SCPutS(b, n, cap, " ");
    n = SCPutS(b, n, cap, detail ? detail : "");
    if (num >= 0) n = SCPutU(b, n, cap, (unsigned long)num);
    b[n++] = '\n';
    return n;
}
static void SCEventRaw(const char *kind, const char *feature, const char *detail, long num) {
    int fd = gSCFd;
    if (fd < 0) return;
    char b[256];
    int n = SCFormat(b, sizeof b, gSCAct, kind, feature, detail, num);
    // also called from the crash handler: nothing async-signal-safe to do about a failed write, so the line is lost
    (void)write(fd, b, n);
}

// ---- ops ----
static int SCOpBegin(const char *feature, double timeoutSec) {
    if (!atomic_load(&gSCOn)) return -1;
    os_unfair_lock_lock(&gSCLock);
    int slot = -1;
    for (int i = 0; i < SC_MAXOPS; i++) {
        if (!atomic_load(&gSCOps[i].used)) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        SCOp *o = &gSCOps[slot];
        o->feature = feature;
        o->timeout = (uint64_t)(timeoutSec * gSCScale * NSEC_PER_SEC);
        o->expired = 0;
        o->deadline = atomic_load(&gSCActiveNs) + o->timeout;
        atomic_store(&o->used, 1);
    }
    os_unfair_lock_unlock(&gSCLock);
    if (slot >= 0) SCEventRaw("op-begin", feature, "slot=", slot);
    return slot;
}
static void SCOpEnd(int slot) {
    if (slot < 0 || slot >= SC_MAXOPS) return;
    const char *f = gSCOps[slot].feature;
    os_unfair_lock_lock(&gSCLock);
    atomic_store(&gSCOps[slot].used, 0);
    os_unfair_lock_unlock(&gSCLock);
    SCEventRaw("op-end", f, "slot=", slot);
}
static BOOL SCLatched(const char *feature) { return atomic_load(&gSCLatchedNow[SCFeatureIndex(feature)]) != 0; }

static BOOL SCIsTraced(void) {
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && (kp.kp_proc.p_flag & P_TRACED);
}
static void *SCWatchdog(void *arg) {
    pthread_setname_np("simcam.watchdog");
    const uint64_t tick = 250 * NSEC_PER_MSEC;
    uint64_t last = clock_gettime_nsec_np(CLOCK_MONOTONIC);   // includes Mac sleep, so sleep shows up as a gap
    for (;;) @autoreleasepool {
        struct timespec ts = {0, (long)tick};
        nanosleep(&ts, NULL);
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), gap = now - last;
        last = now;
        int traced = SCIsTraced();
        if (traced != atomic_load(&gSCTraced)) {
            atomic_store(&gSCTraced, traced);
            LOG(@"watchdog: debugger %s", traced ? "attached" : "detached");
        }
        // a gap over 1 s (stopped, suspended or asleep) advances the active clock by one tick only
        uint64_t active = atomic_fetch_add(&gSCActiveNs, gap > NSEC_PER_SEC ? tick : gap) +
                          (gap > NSEC_PER_SEC ? tick : gap);
        os_unfair_lock_lock(&gSCLock);
        if (gap > NSEC_PER_SEC) {   // stopped, suspended or asleep: restart every deadline
            for (int i = 0; i < SC_MAXOPS; i++)
                if (atomic_load(&gSCOps[i].used)) gSCOps[i].deadline = active + gSCOps[i].timeout;
        }
        const char *expired[SC_MAXOPS];
        int nExpired = 0;
        for (int i = 0; i < SC_MAXOPS; i++) {
            SCOp *o = &gSCOps[i];
            if (atomic_load(&o->used) && !o->expired && active > o->deadline) {
                o->expired = 1;
                expired[nExpired++] = o->feature;
            }
        }
        os_unfair_lock_unlock(&gSCLock);
        if (gap > NSEC_PER_SEC) LOG(@"watchdog: process was stopped for %.1f s; deadlines restarted", gap / 1e9);
        for (int i = 0; i < nExpired; i++) {
            if (traced) {
                SCEventRaw("unknown-hang", expired[i], "traced", -1);
                LOG(@"watchdog: %s op over its deadline while a debugger is attached (not counted)", expired[i]);
                continue;
            }
            int fi = SCFeatureIndex(expired[i]), n = atomic_fetch_add(&gSCHangs[fi], 1) + 1;
            SCEventRaw("hang", expired[i], "n=", n);
            LOG(@"watchdog: %s op hung (%d in this process)", expired[i], n);
            if (n >= SC_STRIKES_TO_LATCH && !atomic_exchange(&gSCLatchedNow[fi], 1)) {
                LOG(@"watchdog: %s latched after %d hangs", expired[i], n);
                TRLatch(fi);
                // session: running sessions stop, with the error if allowed; movie: active recordings stop
                if (fi == SC_FSESSION) SessInjectRuntimeError(SC_LATCH_AUTO, NO);
                else if (fi == SC_FMOVIE) MovieStopAll(SC_LATCH_STATUS);
                fprintf(stderr, "simcam: %s latched after %d hangs in this process\n", expired[i], n);
            }
        }
    }
    return NULL;
}
