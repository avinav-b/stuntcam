// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): pause-aware op watchdog and crash sentinel (design §5.4).
//
// Ops. SCOpBegin(feature, timeout) / SCOpEnd(op) bracket work the engine owns (configure/start/stop, a photo until its
// still-complete is posted). A watchdog thread ticks every 250 ms and measures deadlines on an *active* clock:
//   - a gap > 1 s between ticks means the process was stopped (debugger, suspension, Mac sleep): every inflight deadline
//     restarts and the clock advances by one tick only;
//   - while P_TRACED, an expired op is recorded as unknown-hang and never counted;
//   - with TSan/ASan loaded, timeouts are x10.
// An expired op is a hang, recorded for the next activation. Two hangs of one feature in this process latch it in-process
// (SCLatched), and the latch error-completes (design §5.4 table) with SC_LATCH_STATUS, which apps get as AVFoundation's
// generic -11800 with that OSStatus underneath: photo -> every photo not yet encoded fails; movie ->
// active recordings stop with it and new ones fail at once; session -> running sessions get a runtime error and stop, and a
// latched startRunning gets at most one runtime error per 5 s (design §5.4; in between it is stopped without one). Before
// any session exists a latch means stock (the activation-time latch, below). Not wired yet: no movie op is bracketed with
// SCOpBegin("movie"), so the movie latch can only come from an earlier activation's events (TODO.md).
//
// Sentinel. A chained SIGSEGV/BUS/ILL/ABRT/TRAP/FPE handler attributes a crash, async-signal-safely, against image ranges
// cached at activation: the PC is in our image (feature = the inflight op's, else "provider"), or an op is inflight and
// the PC is in AVFCapture/AVFoundation/CMCapture/CoreMedia/CoreVideo/IOSurface/ImageIO/VideoToolbox (feature = the op's).
// Anything else is recorded as "crash-other" and never counted. It appends one line to the events file, then re-raises.
//
// Events file: $SIMCAM_STATE_DIR/<bundle id>/events.log (else <container tmp>/simcam-state/…), one line per event:
//   <epoch> <activation#> <kind> <feature> <build> <detail>
// kinds: activate (detail traced=0|1), exit, op-begin, op-end, hang, unknown-hang, crash, crash-traced (never counted),
// crash-other (never counted), clear.
// At activation: two crash/hang events of one feature with this build, within the last 20 activations and 24 h, latch it:
// the gate stays closed (no camera) and one stderr line says why. A previous traced activation that ended with an op
// inflight and no crash/exit line prints "last debug run ended inside <feature> op"; it is never counted.
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
#define SC_LATCH_STATUS (-16800)
static void M12RuntimeError(int status, BOOL serverDied);   // core/control.inc
static void M12StopAllRecordings(int status);                // core/control.inc
static void TRCrash(int sig);                                // core/trace.inc (async-signal-safe)
static void TRLatch(int fi);                                 // core/trace.inc
// design §5.4: a latched session gets at most one runtime error per 5 s, whoever posts it (the watchdog at latch time, or a
// latched startRunning). Returns YES and stamps the time if one may be posted now.
static _Atomic uint64_t gSCSessErrAt;
// M12RuntimeError(SC_LATCH_AUTO): the session latch's stop. Whether it carries the runtime error is decided on the pump queue
// when it finds a running session, so a latch that fires with no session running doesn't use up the 5 s window.
#define SC_LATCH_AUTO INT_MIN
static BOOL SCSessionErrorAllowed(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), prev = atomic_load(&gSCSessErrAt);
    if (prev && now - prev < 5 * NSEC_PER_SEC) return NO;
    return atomic_compare_exchange_strong(&gSCSessErrAt, &prev, now);
}
typedef struct { _Atomic int used; const char *feature; uint64_t deadline, timeout; int expired; } SCOp;
static SCOp gSCOps[SC_MAXOPS];
static os_unfair_lock gSCLock = OS_UNFAIR_LOCK_INIT;
static _Atomic uint64_t gSCActiveNs;          // active clock (ns), advanced by the watchdog
static _Atomic int gSCOn, gSCTraced;
static double gSCScale = 1;
static int gSCFd = -1, gSCAct;                // events fd (O_APPEND), this activation's number
static char gSCBuild[32] = "-", gSCPath[1024];
static int gSCIdx = -1;   // PREPARE 1: which gSCDir events.log opened in (0 state dir, 1 container tmp); this build's marker goes only there
// `activate` (a timeout, crash or exception during activation) is a feature like the others: two strikes latch the camera.
// `motion` (a crash in the CoreMotion stand-in, attributed through the thread-specific feature key below) latches
// CoreMotion only: it never closes the camera, it only writes latched-motion-<build>.
#define SC_NFEAT 7
#define SC_FMOTION 6
static const char *gSCFeatures[SC_NFEAT] = {"session", "photo", "movie", "provider", "engine", "activate", "motion"};
static _Atomic int gSCHangs[SC_NFEAT], gSCLatchedNow[SC_NFEAT];
static int SCFeatureIndex(const char *f) { for (int i = 0; i < SC_NFEAT; i++) if (!strcmp(gSCFeatures[i], f)) return i; return 4; }
static BOOL SCClosesCamera(int fi) { return fi != SC_FMOTION; }

// ---- the thread-specific feature key (design §5.4 provider path (b)) ----
// Created at load. Every motion entry point saves the key's value, sets `motion` and restores the saved value on exit (never
// clears it, so a value set further up the same thread survives); around the call into the app's handler the key is saved,
// cleared and restored the same way, so a fault in app code never counts. SCOnSignal reads it first (a lock-free TSD read).
// M17_NO_MOTION_KEY=1 (ledger L59) never creates it: a motion fault in our image is then `provider`, which closes the camera.
static const char kSCMotion[] = "motion";
static pthread_key_t gSCFeatKey; static _Atomic int gSCFeatKeyOn;
static void SCFeatKeyInit(void) {
    if (getenv("M17_NO_MOTION_KEY")) return;
    if (pthread_key_create(&gSCFeatKey, NULL) == 0) atomic_store(&gSCFeatKeyOn, 1);
}
static inline void *SCFeatEnter(const char *f) {   // returns the saved value for SCFeatLeave
    if (!atomic_load_explicit(&gSCFeatKeyOn, memory_order_relaxed)) return NULL;
    void *sv = pthread_getspecific(gSCFeatKey); if (sv != (void *)f) pthread_setspecific(gSCFeatKey, f); return sv;
}
static inline void SCFeatLeave(void *sv) {
    if (!atomic_load_explicit(&gSCFeatKeyOn, memory_order_relaxed)) return;
    if (pthread_getspecific(gSCFeatKey) != sv) pthread_setspecific(gSCFeatKey, sv);
}
static inline void SCFeatLeaveP(void **sv) { SCFeatLeave(*sv); }
// a motion entry point: `motion` on this thread until the enclosing scope ends (restored on every exit path)
#define SC_MOTION_SCOPE void *_scMotionSaved __attribute__((cleanup(SCFeatLeaveP), unused)) = SCFeatEnter(kSCMotion)

// ---- async-signal-safe event writer (no stdio, no malloc) ----
static int SCPutS(char *b, int n, int cap, const char *s) { while (s && *s && n < cap - 1) b[n++] = *s++; return n; }
static int SCPutU(char *b, int n, int cap, unsigned long v) {
    char t[24]; int k = 0; do { t[k++] = '0' + v % 10; v /= 10; } while (v && k < 24);
    while (k && n < cap - 1) b[n++] = t[--k]; return n;
}
static int SCFormat(char *b, int cap, long act, const char *kind, const char *feature, const char *detail, long num) {
    int n = 0;
    n = SCPutU(b, n, cap, (unsigned long)time(NULL)); n = SCPutS(b, n, cap, " ");
    n = SCPutU(b, n, cap, (unsigned long)act); n = SCPutS(b, n, cap, " ");
    n = SCPutS(b, n, cap, kind); n = SCPutS(b, n, cap, " "); n = SCPutS(b, n, cap, feature ? feature : "-"); n = SCPutS(b, n, cap, " ");
    n = SCPutS(b, n, cap, gSCBuild); n = SCPutS(b, n, cap, " "); n = SCPutS(b, n, cap, detail ? detail : "");
    if (num >= 0) n = SCPutU(b, n, cap, (unsigned long)num);
    b[n++] = '\n'; return n;
}
static void SCEventRaw(const char *kind, const char *feature, const char *detail, long num) {
    int fd = gSCFd; if (fd < 0) return;
    char b[256]; int n = SCFormat(b, sizeof b, gSCAct, kind, feature, detail, num); write(fd, b, n);
}

// ---- ops ----
static int SCOpBegin(const char *feature, double timeoutSec) {
    if (!atomic_load(&gSCOn)) return -1;
    os_unfair_lock_lock(&gSCLock);
    int slot = -1;
    for (int i = 0; i < SC_MAXOPS; i++) if (!atomic_load(&gSCOps[i].used)) { slot = i; break; }
    if (slot >= 0) {
        SCOp *o = &gSCOps[slot];
        o->feature = feature; o->timeout = (uint64_t)(timeoutSec * gSCScale * NSEC_PER_SEC); o->expired = 0;
        o->deadline = atomic_load(&gSCActiveNs) + o->timeout; atomic_store(&o->used, 1);
    }
    os_unfair_lock_unlock(&gSCLock);
    if (slot >= 0) SCEventRaw("op-begin", feature, "slot=", slot);
    return slot;
}
static void SCOpEnd(int slot) {
    if (slot < 0 || slot >= SC_MAXOPS) return;
    const char *f = gSCOps[slot].feature;
    os_unfair_lock_lock(&gSCLock); atomic_store(&gSCOps[slot].used, 0); os_unfair_lock_unlock(&gSCLock);
    SCEventRaw("op-end", f, "slot=", slot);
}
static BOOL SCLatched(const char *feature) { return atomic_load(&gSCLatchedNow[SCFeatureIndex(feature)]) != 0; }

static BOOL SCIsTraced(void) {
    struct kinfo_proc kp; size_t len = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && (kp.kp_proc.p_flag & P_TRACED);
}
static void *SCWatchdog(void *arg) {
    pthread_setname_np("simcam.watchdog");
    const uint64_t tick = 250 * NSEC_PER_MSEC;
    uint64_t last = clock_gettime_nsec_np(CLOCK_MONOTONIC);   // includes Mac sleep, so sleep shows up as a gap
    for (;;) @autoreleasepool {
        struct timespec ts = {0, (long)tick}; nanosleep(&ts, NULL);
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), gap = now - last; last = now;
        int traced = SCIsTraced();
        if (traced != atomic_load(&gSCTraced)) { atomic_store(&gSCTraced, traced); LOG(@"watchdog: debugger %s", traced ? "attached" : "detached"); }
        uint64_t active = atomic_fetch_add(&gSCActiveNs, gap > NSEC_PER_SEC ? tick : gap) + (gap > NSEC_PER_SEC ? tick : gap);
        os_unfair_lock_lock(&gSCLock);
        if (gap > NSEC_PER_SEC) {   // stopped, suspended or asleep: restart every deadline
            for (int i = 0; i < SC_MAXOPS; i++) if (atomic_load(&gSCOps[i].used)) gSCOps[i].deadline = active + gSCOps[i].timeout;
        }
        const char *expired[SC_MAXOPS]; int ne = 0;
        for (int i = 0; i < SC_MAXOPS; i++) {
            SCOp *o = &gSCOps[i];
            if (atomic_load(&o->used) && !o->expired && active > o->deadline) { o->expired = 1; expired[ne++] = o->feature; }
        }
        os_unfair_lock_unlock(&gSCLock);
        if (gap > NSEC_PER_SEC) LOG(@"watchdog: process was stopped for %.1f s; deadlines restarted", gap / 1e9);
        for (int i = 0; i < ne; i++) {
            if (traced) { SCEventRaw("unknown-hang", expired[i], "traced", -1); LOG(@"watchdog: %s op over its deadline while a debugger is attached (not counted)", expired[i]); continue; }
            int fi = SCFeatureIndex(expired[i]), n = atomic_fetch_add(&gSCHangs[fi], 1) + 1;
            SCEventRaw("hang", expired[i], "n=", n);
            LOG(@"watchdog: %s op hung (%d in this process)", expired[i], n);
            if (n >= 2 && !atomic_exchange(&gSCLatchedNow[fi], 1)) {
                LOG(@"watchdog: %s latched after %d hangs", expired[i], n);
                TRLatch(fi);
                if (fi == 0) M12RuntimeError(SC_LATCH_AUTO, NO);   // session: running sessions stop, with the error if allowed
                else if (fi == 2) M12StopAllRecordings(SC_LATCH_STATUS); // movie
                fprintf(stderr, "simcam: %s latched after %d hangs in this process\n", expired[i], n);
            }
        }
    }
    return NULL;
}

// ---- crash sentinel ----
typedef struct { uintptr_t lo, hi; } SCRange;
static SCRange gSCSelf, gSCRelated[12]; static _Atomic int gSCNRelated;
static struct sigaction gSCOld[32];   // what the full handler chains to
static struct sigaction gSCPre[32];   // what the minimal (prepare) handler chains to: the handler before activation
static const int kSCSigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
#define SC_NSIGS ((int)(sizeof kSCSigs / sizeof *kSCSigs))
static BOOL SCTextRange(const struct mach_header *mh, intptr_t slide, SCRange *r) {
    const struct load_command *lc = (const void *)((const struct mach_header_64 *)mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
            const struct segment_command_64 *sg = (const void *)lc; r->lo = sg->vmaddr + slide; r->hi = r->lo + sg->vmsize; return YES;
        }
    return NO;
}
// Called before the minimal handler goes in (PREPARE 1) and again after PREPARE 2's dlopens. A range is written before the
// count that covers it is published, so a handler never reads a half-written range; ranges already cached stay as they are.
static void SCCacheRanges(void) {
    Dl_info di; const struct mach_header *self = dladdr((const void *)SCCacheRanges, &di) ? di.dli_fbase : NULL;
    static const char *rel[] = {"/AVFCapture", "/AVFoundation", "/CMCapture", "/CoreMedia", "/CoreVideo", "/IOSurface", "/ImageIO", "/VideoToolbox"};
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header *mh = _dyld_get_image_header(i); const char *nm = _dyld_get_image_name(i); if (!mh || !nm) continue; size_t l = strlen(nm);
        if (mh == self) { if (!gSCSelf.hi) SCTextRange(mh, _dyld_get_image_vmaddr_slide(i), &gSCSelf); continue; }
        for (unsigned j = 0; j < sizeof rel / sizeof *rel; j++) {
            size_t rl = strlen(rel[j]); if (l < rl || strcmp(nm + l - rl, rel[j])) continue;
            int n = atomic_load(&gSCNRelated); SCRange r;
            if (n >= 12 || !SCTextRange(mh, _dyld_get_image_vmaddr_slide(i), &r)) break;
            BOOL have = NO; for (int k = 0; k < n; k++) if (gSCRelated[k].lo == r.lo) have = YES;
            if (!have) { gSCRelated[n] = r; atomic_store(&gSCNRelated, n + 1); }
            break;
        }
    }
}
static uintptr_t SCPC(void *ctx) {
    ucontext_t *uc = ctx; if (!uc || !uc->uc_mcontext) return 0;
#if defined(__arm64__)
  #ifdef __darwin_arm_thread_state64_get_pc
    return (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
  #else
    return (uintptr_t)uc->uc_mcontext->__ss.__pc;
  #endif
#elif defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext->__ss.__rip;
#else
    return 0;
#endif
}
static BOOL SCInRelated(uintptr_t pc) { int n = atomic_load(&gSCNRelated); for (int i = 0; i < n; i++) if (pc >= gSCRelated[i].lo && pc < gSCRelated[i].hi) return YES; return NO; }
// chain: the previous handler if it was one, else the default action
static void SCChain(int sig, siginfo_t *si, void *ctx, struct sigaction *old) {
    if (old && (old->sa_flags & SA_SIGINFO) && old->sa_sigaction) { sigaction(sig, old, NULL); old->sa_sigaction(sig, si, ctx); return; }
    if (old && !(old->sa_flags & SA_SIGINFO) && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) { sigaction(sig, old, NULL); old->sa_handler(sig); return; }
    signal(sig, SIG_DFL);
    // A fault the kernel generated (SEGV/BUS/ILL/FPE/TRAP from an instruction): return, so the instruction runs again under
    // SIG_DFL and the crash report shows the real exception (EXC_BAD_ACCESS ...) on the crashing thread. raise() here made it
    // EXC_CRASH (SIGSEGV) blamed on thread 0, with the crashing thread's stack empty. Sent signals (abort, kill) are re-sent.
    if (sig != SIGABRT && si && si->si_code != SI_USER && si->si_code != SI_QUEUE) return;
    raise(sig);
}
// PREPARE 1 counts this build's `motion` strikes in the window and keeps the newest one's epoch and the
// latched-motion marker's path next to the events.log in use, so the full handler can write the marker on the second strike
// itself (async-signal-safe: open, write, futimens). Motion is decided at load, so a marker the next launch's PREPARE 1 wrote
// would come one launch late. M17_NO_HANDLER_MARKER=1 (ledger L60) skips the write.
// Written to a temp file next to it (path precomputed by PREPARE 1), its until set, then
// renamed into place, so the marker never exists with mtime = now (which another process's prune reads as expired).
// M17_MARKER_INPLACE=1 (ledger L64) writes in place again, as SCWriteMarker does under it.
static int gSCMotionPrior; static long gSCMotionPriorEpoch; static char gSCMotionMarkerUse[1100], gSCMotionMarkerTmp[1200]; static BOOL gM17NoHandlerMarker;
static BOOL gM17MarkerInplace; static long gM16SlowMarkerMs, gM16PrunePauseMs;   // M17_MARKER_INPLACE (L64), M16_SLOW_MARKER_MS (pf-prune-race), M16_PRUNE_PAUSE_MS (pf-prune-kept)
static void SCMotionMarkerFromHandler(void) {   // async-signal-safe only
    if (gSCMotionPrior < 1 || !gSCMotionMarkerUse[0] || gM17NoHandlerMarker) return;
    const char *w = gM17MarkerInplace || !gSCMotionMarkerTmp[0] ? gSCMotionMarkerUse : gSCMotionMarkerTmp;
    int fd = open(w, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) return;
    write(fd, "motion\n", 7);
    struct timespec ts[2] = {{gSCMotionPriorEpoch + 86400, 0}, {gSCMotionPriorEpoch + 86400, 0}}; futimens(fd, ts); close(fd);
    if (w != gSCMotionMarkerUse && rename(w, gSCMotionMarkerUse) != 0) unlink(w);
}
static void SCOnSignal(int sig, siginfo_t *si, void *ctx) {   // async-signal-safe only
    uintptr_t pc = SCPC(ctx);
    BOOL self = pc >= gSCSelf.lo && pc < gSCSelf.hi;
    // the feature key first: a fault in our image on a thread inside a motion entry point is `motion`, not `provider`
    // or an inflight op's feature, and it never closes the camera. An app handler runs with the key cleared (and an app PC).
    const void *tk = atomic_load_explicit(&gSCFeatKeyOn, memory_order_relaxed) ? pthread_getspecific(gSCFeatKey) : NULL;
    const char *op = NULL;
    for (int i = 0; i < SC_MAXOPS; i++) if (atomic_load(&gSCOps[i].used)) { op = gSCOps[i].feature; break; }
    const char *feature = NULL;
    if (self && tk == (const void *)kSCMotion) feature = kSCMotion;
    else if (self) feature = op ? op : "provider";
    else if (op && SCInRelated(pc)) feature = op;
    // under a debugger a crash is never counted (design §5.4): it may be the user stopping in lldb
    BOOL traced = atomic_load(&gSCTraced);
    SCEventRaw(!feature ? "crash-other" : traced ? "crash-traced" : "crash", feature, "signal=", sig);
    if (feature == kSCMotion && !traced) SCMotionMarkerFromHandler();
    TRCrash(sig);   // the op ring to trace-<pid>.txt (async-signal-safe)
    SCChain(sig, si, ctx, (sig > 0 && sig < 32) ? &gSCOld[sig] : NULL);
}
// The minimal handler, installed in PREPARE 1. While prepare runs it counts as an `activate` op (design §5.4
// path a): a fault on any thread whose PC is in our image or a related image is `crash activate`. Anything else, or any
// fault once gActRunning is clear, only chains to the handler that was there before activation.
static _Atomic int gSCPrepRecorded, gSCMinimalOn;
static void SCOnSignalPrepare(int sig, siginfo_t *si, void *ctx) {   // async-signal-safe only
    if (atomic_load(&gActRunning)) {
        uintptr_t pc = SCPC(ctx);
        if (((pc >= gSCSelf.lo && pc < gSCSelf.hi) || SCInRelated(pc)) && !atomic_exchange(&gSCPrepRecorded, 1))
            SCEventRaw(atomic_load(&gSCTraced) ? "crash-traced" : "crash", "activate", "signal=", sig);
    }
    SCChain(sig, si, ctx, (sig > 0 && sig < 32) ? &gSCPre[sig] : NULL);
}
static BOOL SCIsHandler(const struct sigaction *a, void *fn) { return (a->sa_flags & SA_SIGINFO) && (void *)a->sa_sigaction == fn; }
static BOOL SCSameAction(const struct sigaction *a, const struct sigaction *b) {
    return a->sa_flags == b->sa_flags && ((a->sa_flags & SA_SIGINFO) ? (void *)a->sa_sigaction == (void *)b->sa_sigaction : a->sa_handler == b->sa_handler);
}
// install order: read the current handler first, then install ours; if what we replaced differs from the pre-read
// (someone installed in between), chain to that instead. The minimal handler never runs with gSCPre unset.
static void SCInstallMinimal(void) {
    for (int i = 0; i < SC_NSIGS; i++) {
        int sig = kSCSigs[i]; struct sigaction pre, old, sa = {0};
        sigaction(sig, NULL, &pre); gSCPre[sig] = pre;
        sa.sa_sigaction = SCOnSignalPrepare; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &old);
        if (!SCSameAction(&old, &pre)) gSCPre[sig] = old;
    }
    atomic_store(&gSCMinimalOn, 1);
}
// conditional restore: put back the pre-activation handler only where ours is still current. Something that
// installed itself on top (a crash SDK during a slow prepare) is left as it is; it chains to the minimal handler, which only
// chains on. Returns the number of signals restored.
static int SCRestoreConditional(void) {
    if (!atomic_load(&gSCMinimalOn)) return 0;
    int n = 0;
    for (int i = 0; i < SC_NSIGS; i++) {
        int sig = kSCSigs[i]; struct sigaction cur;
        if (sigaction(sig, NULL, &cur) == 0 && SCIsHandler(&cur, SCOnSignalPrepare)) { sigaction(sig, &gSCPre[sig], NULL); n++; }
    }
    return n;
}
// SWAP 6: the full handler. Where the minimal one is still current it takes over its chain (gSCPre), so the full handler never
// chains through the minimal one; where something else is current it chains to that. One record per crash either way.
static void SCInstallFull(void) {
    if (gSCFd < 0) return;
    for (int i = 0; i < SC_NSIGS; i++) {
        int sig = kSCSigs[i]; struct sigaction cur, sa = {0};
        sigaction(sig, NULL, &cur);
        gSCOld[sig] = SCIsHandler(&cur, SCOnSignalPrepare) ? gSCPre[sig] : cur;
        sa.sa_sigaction = SCOnSignal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, NULL);
    }
}

// ---- load-time state, markers, the record queue's writer ----
// Markers (mtime = until; one past its until is ignored and deleted by the next prepare):
//   <state>/<bundle>/latched-<build>         the latched camera features; read at load: the camera starts STOCK, no prepare,
//                                            and CoreMotion is stock
//   <state>/<bundle>/latched-motion-<build>  `motion`: read at load by motion only; the camera is unaffected
//   <state>/preflight-fail-<build>           a real preflight failure, until +24 h: read at load by motion only (the camera
//                                            always runs its own preflight); a passing preflight unlinks it
// Every location is covered twice: SIMCAM_STATE_DIR and the container-tmp fallback (17.5, where the state dir can't be written).
static int gSCSentinel = 1;                    // SIMCAM_SENTINEL: 0 off, 1 on, 2 clear
static char gSCBundle[256] = "unknown";
static char gSCDir[2][1024], gSCMarker[2][1100], gSCPfPath[2][1100], gSCMarkerAtLoad[1100];
static char gSCMotionMarker[2][1100];          // <dir>/latched-motion-<build>, both locations
static char gSCMotionStockAt[1100];            // the marker that made CoreMotion stock at load ("" = the stand-in)
static time_t gSCMotionStockUntil;              // its until (st_mtime) at that load-time stat
static ino_t gSCMotionStockIno;                 // and its inode: the stock line reads the features only from this version
static const char *gSCMotionStockKind;          // what it says if it is gone by the stock line: "camera" | "motion" | "preflight failed"
static BOOL gSCClearedAtLoad;
static _Atomic(const char *) gSCPathPub;       // published by PREPARE 1 (release) for the record queue
static _Atomic int gSCActPub;
static _Atomic int gActRecordedAct;            // the activation number the record queue's timeout line used (0 = none)
static char gSCLatchMsg[600];                  // PREPARE 1's latch line, printed through the record queue
static int SCMkdirP(const char *path) {
    char b[1024]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(b, 0755); *p = '/'; }
    return (mkdir(b, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}
static void SCTmpRoot(char *out, size_t cap) {   // <container tmp>/simcam-state
    const char *t = getenv("TMPDIR"); char b[1024];
    if (t && *t) snprintf(b, sizeof b, "%s", t); else snprintf(b, sizeof b, "%s", NSTemporaryDirectory().fileSystemRepresentation);
    size_t l = strlen(b); while (l > 1 && b[l - 1] == '/') b[--l] = 0;
    snprintf(out, cap, "%s/simcam-state", b);
}
// at load: no I/O apart from the marker stat()s (and the explicit clear below)
static void SCLoadInit(void) {
    const char *sw = getenv("SIMCAM_SENTINEL");
    gSCSentinel = sw && sw[0] == '0' ? 0 : sw && !strcmp(sw, "clear") ? 2 : 1;
    gSCScale = (dlsym(RTLD_DEFAULT, "__tsan_init") || dlsym(RTLD_DEFAULT, "__asan_init")) ? 10 : 1;
    Dl_info di; if (dladdr((const void *)SCLoadInit, &di) && di.dli_fname) {   // build = the content-hash dir of our dylib
        const char *e = strrchr(di.dli_fname, '/');
        if (e) { const char *s = e; while (s > di.dli_fname && s[-1] != '/') s--; snprintf(gSCBuild, sizeof gSCBuild, "%.*s", (int)(e - s), s); }
    }
    CFBundleRef mb = CFBundleGetMainBundle(); CFStringRef bid = mb ? CFBundleGetIdentifier(mb) : NULL;
    if (bid) CFStringGetCString(bid, gSCBundle, sizeof gSCBundle, kCFStringEncodingUTF8);
    const char *sd = getenv("SIMCAM_STATE_DIR"); char tmp[1024]; SCTmpRoot(tmp, sizeof tmp);
    if (sd && *sd) { snprintf(gSCDir[0], sizeof gSCDir[0], "%s/%s", sd, gSCBundle); snprintf(gSCPfPath[0], sizeof gSCPfPath[0], "%s/preflight-fail-%s", sd, gSCBuild); }
    snprintf(gSCDir[1], sizeof gSCDir[1], "%s/%s", tmp, gSCBundle); snprintf(gSCPfPath[1], sizeof gSCPfPath[1], "%s/preflight-fail-%s", tmp, gSCBuild);
    for (int i = 0; i < 2; i++) if (gSCDir[i][0]) {
        snprintf(gSCMarker[i], sizeof gSCMarker[i], "%s/latched-%s", gSCDir[i], gSCBuild);
        snprintf(gSCMotionMarker[i], sizeof gSCMotionMarker[i], "%s/latched-motion-%s", gSCDir[i], gSCBuild);
    }
    gM17NoHandlerMarker = getenv("M17_NO_HANDLER_MARKER") != NULL;
    gM17MarkerInplace = getenv("M17_MARKER_INPLACE") != NULL;
    const char *sm = getenv("M16_SLOW_MARKER_MS"); if (sm) gM16SlowMarkerMs = atol(sm);
    const char *pp = getenv("M16_PRUNE_PAUSE_MS"); if (pp) gM16PrunePauseMs = atol(pp);
}
static BOOL SCMarkerValidAtLoad(void) {
    if (gSCSentinel != 1) return NO;
    time_t now = time(NULL);
    for (int i = 0; i < 2; i++) { struct stat st; if (gSCMarker[i][0] && stat(gSCMarker[i], &st) == 0 && st.st_mtime > now) { snprintf(gSCMarkerAtLoad, sizeof gSCMarkerAtLoad, "%s", gSCMarker[i]); return YES; } }
    return NO;
}
// CoreMotion is decided once, at load, from markers only. Any valid one of this build's latched-<build> (a
// camera latch), latched-motion-<build> or preflight-fail-<build>, in either location, with the sentinel on and no clear,
// makes CoreMotion stock for the process. Markers this process writes later can't change it (a camera activation that
// fails in this launch keeps the stand-in for the launch). M17_NO_MOTION_MARKERS=1 (ledger L58) ignores them.
static BOOL SCMotionStockAtLoad(void) {
    if (gSCSentinel != 1 || getenv("M17_NO_MOTION_MARKERS")) return NO;
    time_t now = time(NULL);
    const char *cand[6] = {gSCMarker[0], gSCMarker[1], gSCMotionMarker[0], gSCMotionMarker[1], gSCPfPath[0], gSCPfPath[1]};
    for (int i = 0; i < 6; i++) {
        struct stat st;
        if (cand[i][0] && stat(cand[i], &st) == 0 && st.st_mtime > now) {
            snprintf(gSCMotionStockAt, sizeof gSCMotionStockAt, "%s", cand[i]);
            // kept for the stock line: by the first motion call a passing preflight or PREPARE 1 may have unlinked the marker
            gSCMotionStockUntil = st.st_mtime; gSCMotionStockIno = st.st_ino; gSCMotionStockKind = i < 2 ? "camera" : i < 4 ? "motion" : "preflight failed";
            return YES;
        }
    }
    return NO;
}
static void SCUnlinkLatchMarkers(const char *dir, const char *keepBuild) {   // every latched-* in dir (but keepBuild's)
    DIR *d = opendir(dir); if (!d) return; struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "latched-", 8) || (keepBuild && !strcmp(e->d_name + 8, keepBuild))) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); unlink(p);
    }
    closedir(d);
}
// SIMCAM_SENTINEL=clear at load: an explicit user action, so I/O at load is acceptable. Appends the clear line (also for an app
// that never runs prepare: a motion-only app) and unlinks the bundle's markers and this build's preflight-fail.
static void SCClearAtLoad(void) {
    if (gSCSentinel != 2) return;
    for (int i = 0; i < 2; i++) {
        if (!gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
        char p[1100]; snprintf(p, sizeof p, "%s/events.log", gSCDir[i]);
        int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644); if (fd < 0) continue;
        char b[256]; int n = SCFormat(b, sizeof b, 0, "clear", "-", "SIMCAM_SENTINEL=clear", -1); write(fd, b, n); close(fd);
        gSCClearedAtLoad = YES; break;
    }
    for (int i = 0; i < 2; i++) { if (gSCDir[i][0]) SCUnlinkLatchMarkers(gSCDir[i], NULL); if (gSCPfPath[i][0]) unlink(gSCPfPath[i]); }
}
// A marker is written to "<dir>/.<name>.tmp-<pid>", its mtime set to the until, then renamed
// into place. In place, it existed with mtime = now between open(O_TRUNC) and futimens, and another process's PREPARE 1 prune
// (st_mtime <= now) could unlink it. Temp names start with '.', so no prune, load-time stat or CLI listing matches them.
// M17_MARKER_INPLACE=1 (ledger L64) writes in place again; M16_SLOW_MARKER_MS=<n> pauses n ms after the content is written,
// before the until is set (pf-prune-race widens the window with it).
static void SCSlowMarker(const char *path) {
    if (gM16SlowMarkerMs <= 0) return;
    fprintf(stderr, "[simcam %d] M16_SLOW_MARKER_MS: marker write paused %ld ms (%s)\n", getpid(), gM16SlowMarkerMs, path);
    M16Sleep(gM16SlowMarkerMs);
}
static BOOL SCWriteMarker(const char *path, const char *content, time_t until) {
    char dir[1100]; snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; SCMkdirP(dir); }
    char tmp[1300]; const char *base = sl ? sl + 1 : path;
    if (gM17MarkerInplace) snprintf(tmp, sizeof tmp, "%s", path);
    else snprintf(tmp, sizeof tmp, "%s/.%s.tmp-%d", sl ? dir : ".", base, getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) return NO;
    write(fd, content, strlen(content));
    SCSlowMarker(path);
    struct timespec ts[2] = {{until, 0}, {until, 0}}; futimens(fd, ts); close(fd);
    if (gM17MarkerInplace) return YES;
    if (rename(tmp, path) != 0) { unlink(tmp); return NO; }
    return YES;
}
// a temp file left by a process that died mid-write: removed once it is a minute old (its mtime is the until, so ctime)
static void SCPruneTmp(const char *dir, const char *name, long now) {
    if (name[0] != '.' || !strstr(name, ".tmp-") || (strncmp(name + 1, "latched-", 8) && strncmp(name + 1, "preflight-fail-", 15))) return;
    char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, name); struct stat st;
    if (stat(p, &st) == 0 && st.st_ctime < now - 60 && unlink(p) == 0) LOG(@"sentinel: removed a stale marker temp file %s", p);
}
// Prune one expired marker: a stat, then an unlink by path, could delete a fresh marker that another
// process renamed over the expired one in between. So the file is renamed aside first (".<name>.tmp-prune-<pid>": a temp
// name, which no reader matches and SCPruneTmp removes after 60 s if this process dies here), re-checked on the renamed
// inode, and unlinked only if it is still expired; a fresh one goes back (RENAME_EXCL: if a newer marker landed at the path
// meanwhile, that one stays and ours goes). Remaining window: while it is aside (a few syscalls), a fresh marker is missing
// from its path, so another process's load-time stat in that instant reads "no marker" for one launch.
// M16_PRUNE_PAUSE_MS=<n> (activation.sh pf-prune-kept): pauses n ms after the caller's stat found the marker expired, before the rename,
// so a test can rename a fresh marker over it in between (the "kept" branch).
static BOOL SCPruneExpired(const char *dir, const char *name, const char *p, long now) {
    char aside[1400]; snprintf(aside, sizeof aside, "%s/.%s.tmp-prune-%d", dir, name, getpid());
    if (gM16PrunePauseMs > 0) { fprintf(stderr, "[simcam %d] M16_PRUNE_PAUSE_MS: prune of %s paused %ld ms\n", getpid(), p, gM16PrunePauseMs); M16Sleep(gM16PrunePauseMs); }
    if (rename(p, aside) != 0) return NO;
    struct stat st;
    if (stat(aside, &st) == 0 && st.st_mtime <= now) { unlink(aside); return YES; }
    if (renamex_np(aside, p, RENAME_EXCL) != 0) unlink(aside);
    LOG(@"sentinel: %s was re-written while being pruned: kept", p);
    return NO;
}
// PREPARE 3: a real preflight failure writes preflight-fail-<build> (until +24 h) next to the events.log in use, as the latch
// marker; a passing preflight unlinks it in both locations
static void SCPreflightMarker(BOOL passed, const char *what) {
    if (gSCSentinel == 0) return;
    for (int i = 0; i < 2; i++) {
        if (!gSCPfPath[i][0]) continue;
        if (passed) unlink(gSCPfPath[i]); else if (gSCIdx < 0 || i == gSCIdx) { char c[300]; snprintf(c, sizeof c, "%s\n", what); SCWriteMarker(gSCPfPath[i], c, time(NULL) + 86400); }
    }
}
// counted strikes of one build in the parsed past: the 20-activation / 24 h window. until[i] = the second-newest counted
// event + 24 h (the latch holds while two are in the window).
static void SCCountStrikes(NSArray *past, NSString *build, long now, int act, int counts[SC_NFEAT], long until[SC_NFEAT], long *newest) {
    long e1[SC_NFEAT] = {0}, e2[SC_NFEAT] = {0};
    for (int i = 0; i < SC_NFEAT; i++) { counts[i] = 0; until[i] = 0; }
    for (NSArray *f in past) {
        NSString *k = f[2];
        if (!([k isEqual:@"crash"] || [k isEqual:@"hang"])) continue;
        if (![f[4] isEqual:build]) continue;
        long ep = [f[0] longLongValue];
        if ([f[1] intValue] <= act - 20 || ep < now - 86400) continue;
        int fi = SCFeatureIndex([f[3] UTF8String]); counts[fi]++;
        if (ep >= e1[fi]) { e2[fi] = e1[fi]; e1[fi] = ep; } else if (ep > e2[fi]) e2[fi] = ep;
    }
    for (int i = 0; i < SC_NFEAT; i++) if (counts[i] >= 2) until[i] = e2[i] + 86400;
    if (newest) for (int i = 0; i < SC_NFEAT; i++) newest[i] = e1[i];
}
// PREPARE 1 step 6: write this build's marker ONLY next to the events.log in use (gSCIdx), so `stuntcam sentinel
// clear` (the state dir, and this container-tmp fallback on a booted sim) removes every marker that can latch; unlink it in
// the other location (one an older engine wrote to both). In both locations, delete every other build's latched-* marker the
// parsed past no longer supports (swap-back: another build's 20 activations pushed its strikes out).
// The same for latched-motion-<build> (motion latched: >= 2 `motion` strikes; it never closes the camera), and a stale
// latched-motion-<other build> is deleted like a stale latched-<other build>.
static void SCMarkersUpdate(NSArray *past, long now, BOOL latched, const char *feats, long until, BOOL mLatched, long mUntil) {
    for (int i = 0; i < 2; i++) {
        if (!gSCDir[i][0]) continue;
        if (latched && i == gSCIdx) { char c[200]; snprintf(c, sizeof c, "%s\n", feats); SCWriteMarker(gSCMarker[i], c, until); }
        else if (unlink(gSCMarker[i]) == 0 && latched) LOG(@"sentinel: removed this build's marker outside the events.log location: %s", gSCMarker[i]);
        if (mLatched && i == gSCIdx) SCWriteMarker(gSCMotionMarker[i], "motion\n", mUntil);
        else if (unlink(gSCMotionMarker[i]) == 0) LOG(@"sentinel: removed %s (%s)", gSCMotionMarker[i], mLatched ? "outside the events.log location" : "motion no longer latched");
        DIR *d = opendir(gSCDir[i]); if (!d) continue; struct dirent *e;
        while ((e = readdir(d))) {
            SCPruneTmp(gSCDir[i], e->d_name, now);
            if (strncmp(e->d_name, "latched-", 8)) continue;
            BOOL motion = !strncmp(e->d_name, "latched-motion-", 15);
            const char *b = e->d_name + (motion ? 15 : 8);
            if (!strcmp(b, gSCBuild)) continue;
            char p[1400]; snprintf(p, sizeof p, "%s/%s", gSCDir[i], e->d_name);
            int c[SC_NFEAT]; long u[SC_NFEAT]; SCCountStrikes(past, @(b), now, gSCAct, c, u, NULL);
            BOOL still = NO; for (int k = 0; k < SC_NFEAT; k++) if (c[k] >= 2 && (motion ? k == SC_FMOTION : SCClosesCamera(k))) still = YES;
            // the events say stale: removed whatever its until (a racing write of that build is stale too); otherwise only once
            // expired, through SCPruneExpired (L4)
            struct stat st;
            if (!still || stat(p, &st) != 0) { unlink(p); LOG(@"sentinel: removed stale marker %s", p); }
            else if (st.st_mtime <= now && SCPruneExpired(gSCDir[i], e->d_name, p, now)) LOG(@"sentinel: removed stale marker %s", p);
        }
        closedir(d);
    }
    // preflight-fail-<any build> past its until, in both locations' sim-level dirs. Unexpired ones of
    // other builds stay: no event records a preflight failure, so nothing says they are stale, and that build still reads them.
    for (int i = 0; i < 2; i++) {
        char dir[1100]; snprintf(dir, sizeof dir, "%s", gSCPfPath[i]); char *sl = strrchr(dir, '/'); if (!gSCPfPath[i][0] || !sl) continue; *sl = 0;
        DIR *d = opendir(dir); if (!d) continue; struct dirent *e;
        while ((e = readdir(d))) {
            SCPruneTmp(dir, e->d_name, now);
            if (strncmp(e->d_name, "preflight-fail-", 15)) continue;
            char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); struct stat st;
            if (stat(p, &st) == 0 && st.st_mtime <= now && SCPruneExpired(dir, e->d_name, p, now)) LOG(@"sentinel: removed expired marker %s", p);
        }
        closedir(d);
    }
}
// the trim: only past 4000 lines; the kept 2000 go to a temp file, which replaces events.log only if the file is
// unchanged since the read (size, mtime, inode), under flock. The record queue appends under the same lock.
static void SCTrim(void) {
    struct stat st0; if (stat(gSCPath, &st0) != 0) return;
    NSString *old = [NSString stringWithContentsOfFile:@(gSCPath) encoding:NSUTF8StringEncoding error:nil];
    NSArray *ol = [old componentsSeparatedByString:@"\n"];
    if (ol.count <= 4000) return;
    atomic_store(&gActPhase, PH_TRIM);
    NSString *kept = [[ol subarrayWithRange:NSMakeRange(ol.count - 2000, 2000)] componentsJoinedByString:@"\n"];
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.trim", gSCPath);
    if ([kept writeToFile:@(tmp) atomically:NO encoding:NSUTF8StringEncoding error:nil]) {
        M16SlowAt(PH_TRIM);
        int fd = open(gSCPath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            flock(fd, LOCK_EX); struct stat st1;
            BOOL same = fstat(fd, &st1) == 0 && st1.st_ino == st0.st_ino && st1.st_size == st0.st_size &&
                        st1.st_mtimespec.tv_sec == st0.st_mtimespec.tv_sec && st1.st_mtimespec.tv_nsec == st0.st_mtimespec.tv_nsec;
            if (same && rename(tmp, gSCPath) == 0) LOG(@"sentinel: trimmed %s to the newest 2000 lines", gSCPath);
            else { unlink(tmp); LOG(@"sentinel: trim skipped (%s changed during the trim); the next launch trims", gSCPath); }
            flock(fd, LOCK_UN); close(fd);
        } else unlink(tmp);
    }
    atomic_store(&gActPhase, PH_SENTINEL);
}
// append one line under the trim's lock; if the file was renamed meanwhile (a trim), reopen and append to the new one
static BOOL SCAppendLocked(const char *path, const char *line, size_t n) {
    for (int tries = 0; tries < 3; tries++) {
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644); if (fd < 0) return NO;
        flock(fd, LOCK_EX); struct stat a, b;
        BOOL cur = fstat(fd, &a) == 0 && stat(path, &b) == 0 && a.st_ino == b.st_ino;
        if (cur) write(fd, line, n);
        flock(fd, LOCK_UN); close(fd);
        if (cur) return YES;
    }
    return NO;
}
// the highest activation number in the last 8 KB of a file (activation numbers only grow, so the tail holds the max)
static int SCTailMaxAct(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    off_t end = lseek(fd, 0, SEEK_END), from = end > 8192 ? end - 8192 : 0; char b[8193]; ssize_t n = pread(fd, b, end - from, from); close(fd);
    if (n <= 0) return 0; b[n] = 0;
    int mx = 0; char *p = b;
    if (from > 0) { char *nl = strchr(p, '\n'); if (!nl) return 0; p = nl + 1; }   // a partial first line
    while (*p) { char *nl = strchr(p, '\n'); char *sp = strchr(p, ' '); if (sp && (!nl || sp < nl)) { int a = atoi(sp + 1); if (a > mx) mx = a; } if (!nl) break; p = nl + 1; }
    return mx;
}
// the record queue's writer: this launch's path and activation number if PREPARE 1 published them, else the
// load-time state dir, then the container-tmp fallback (mkdir -p each; the first that opens), activation = tail max + 1.
// Returns the activation number used, 0 if nothing could be written.
static int SCRecordEvent(const char *kind, const char *feature, const char *detail) {
    if (gSCSentinel == 0) return 0;
    char path[1100]; int act = 0;
    const char *pub = atomic_load_explicit(&gSCPathPub, memory_order_acquire);
    if (pub) { snprintf(path, sizeof path, "%s", pub); act = atomic_load(&gSCActPub); }
    else {
        int rec = atomic_load(&gActRecordedAct);
        for (int i = 0; i < 2 && !act; i++) {
            if (!gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
            snprintf(path, sizeof path, "%s/events.log", gSCDir[i]);
            int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644); if (fd < 0) continue; close(fd);
            act = rec ? rec : SCTailMaxAct(path) + 1;
        }
        if (!act) return 0;
    }
    char b[300]; int n = SCFormat(b, sizeof b, act, kind, feature, detail, -1);
    return SCAppendLocked(path, b, n) ? act : 0;
}

// ---- PREPARE 1 (the read-and-decide half of what was SCActivate): runs on simcam.activate ----
enum { SCP_OK = 0, SCP_OFF, SCP_LATCHED };
static int SCPrepare(void) {
    if (gSCSentinel == 0) return SCP_OFF;
    atomic_store(&gActPhase, PH_SENTINEL);
    // 1. directory: the state dir, else the container-tmp fallback (17.5): the first one events.log opens in
    int idx = -1;
    for (int i = 0; i < 2 && idx < 0; i++) {
        if (!gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
        char p[1100]; snprintf(p, sizeof p, "%s/events.log", gSCDir[i]);
        int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644); if (fd >= 0) { close(fd); idx = i; }
    }
    if (idx < 0) { LOG(@"sentinel: cannot open events.log in %s or %s; ops/sentinel off", gSCDir[0], gSCDir[1]); return SCP_OFF; }
    snprintf(gSCPath, sizeof gSCPath, "%s/events.log", gSCDir[idx]); gSCIdx = idx;
    // 2. trim (only past 4000 lines), then open
    SCTrim();
    int fd = open(gSCPath, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { LOG(@"sentinel: cannot open %s; ops/sentinel off", gSCPath); return SCP_OFF; }
    gSCFd = fd;
    if (gSCSentinel == 2 && !gSCClearedAtLoad) SCEventRaw("clear", "-", "SIMCAM_SENTINEL=clear", -1);
    // 3. the past: everything after the last clear
    NSArray *lines = [[NSString stringWithContentsOfFile:@(gSCPath) encoding:NSUTF8StringEncoding error:nil] componentsSeparatedByString:@"\n"];
    NSMutableArray *past = [NSMutableArray array];
    for (NSString *l in lines) { NSArray *f = [l componentsSeparatedByString:@" "]; if (f.count < 5) continue; if ([f[2] isEqual:@"clear"]) [past removeAllObjects]; else [past addObject:f]; }
    // activation number = highest on record + 1 (not a count of lines, so trimming the file keeps the latch window right);
    // if this launch's timeout line is already written, its number is reused
    int maxAct = 0; for (NSString *l in lines) { NSArray *f = [l componentsSeparatedByString:@" "]; if (f.count >= 5) maxAct = MAX(maxAct, [f[1] intValue]); }
    int rec = atomic_load(&gActRecordedAct);
    gSCAct = rec ? rec : maxAct + 1;
    atomic_store(&gSCActPub, gSCAct); atomic_store_explicit(&gSCPathPub, gSCPath, memory_order_release);
    // previous activation: a traced run that ended inside an op (no crash/exit line) is reported, never counted
    NSInteger lastAct = -1; for (NSInteger i = past.count - 1; i >= 0; i--) if ([past[i][2] isEqual:@"activate"]) { lastAct = i; break; }
    if (lastAct >= 0) {
        NSArray *a = past[lastAct]; BOOL traced = a.count > 5 && [a[5] isEqual:@"traced=1"], ended = NO; NSMutableDictionary *open = [NSMutableDictionary dictionary];
        for (NSInteger i = lastAct + 1; i < (NSInteger)past.count; i++) {
            NSArray *f = past[i]; NSString *k = f[2];
            if ([k isEqual:@"exit"] || [k hasPrefix:@"crash"]) ended = YES;
            else if ([k isEqual:@"op-begin"] && f.count > 5) open[f[5]] = f[3];
            else if ([k isEqual:@"op-end"] && f.count > 5) [open removeObjectForKey:f[5]];
        }
        if (traced && !ended && open.count) {
            NSString *feat = open.allValues.firstObject;
            LOG(@"sentinel: last debug run ended inside %@ op", feat);
            fprintf(stderr, "simcam: last debug run ended inside %s op; if simcam crashed: SIMCAM_SENTINEL_LATCH=%s on the next launch\n", feat.UTF8String, feat.UTF8String);
        }
    }
    // 4. image ranges, then the minimal crash handler, if no waiter has given up meanwhile (checked again after)
    SCCacheRanges();
    if (ActState() == ACT_RUNNING) { SCInstallMinimal(); if (ActState() != ACT_RUNNING) SCRestoreConditional(); }
    M16SlowAt(PH_SENTINEL);   // @sentinel: after the parse, before the latch decision
    // 5. manual latch lines, then the latches: >= 2 crash/hang events of one feature, this build, last 20 activations and 24 h
    const char *ml = getenv("SIMCAM_SENTINEL_LATCH");   // manual latch (design: 'stuntcam sentinel latch <feature>')
    if (ml && *ml) { SCEventRaw("crash", ml, "manual", -1); SCEventRaw("crash", ml, "manual", -1); }
    long now = time(NULL); int counts[SC_NFEAT]; long until[SC_NFEAT], newest[SC_NFEAT];
    SCCountStrikes(past, @(gSCBuild), now, gSCAct, counts, until, newest);
    // this build's motion strikes before this launch, for the full handler's latched-motion marker
    gSCMotionPrior = counts[SC_FMOTION]; gSCMotionPriorEpoch = newest[SC_FMOTION];
    snprintf(gSCMotionMarkerUse, sizeof gSCMotionMarkerUse, "%s", gSCMotionMarker[gSCIdx]);
    snprintf(gSCMotionMarkerTmp, sizeof gSCMotionMarkerTmp, "%s/.latched-motion-%s.tmp-%d", gSCDir[gSCIdx], gSCBuild, getpid());
    if (ml && *ml) { int fi = SCFeatureIndex(ml); counts[fi] += 2; until[fi] = MAX(until[fi], now + 86400); }
    BOOL mLatched = counts[SC_FMOTION] >= 2;
    if (mLatched) LOG(@"sentinel: motion latched (%d crashes in the last 20 activations / 24 h): CoreMotion stock from the next launch; the camera is unaffected", counts[SC_FMOTION]);
    char feats[200] = "", nums[100] = ""; long untilMax = 0; BOOL latched = NO;
    for (int i = 0; i < SC_NFEAT; i++) if (counts[i] >= 2 && SCClosesCamera(i)) {
        snprintf(feats + strlen(feats), sizeof feats - strlen(feats), "%s%s", latched ? " " : "", gSCFeatures[i]);
        snprintf(nums + strlen(nums), sizeof nums - strlen(nums), "%s%d", latched ? ", " : "", counts[i]);
        untilMax = MAX(untilMax, until[i]); latched = YES;
        LOG(@"sentinel: %s latched (%d crashes/hangs in the last 20 activations / 24 h); no camera", gSCFeatures[i], counts[i]);
    }
    // 6. markers, in both locations
    SCMarkersUpdate(past, now, latched, feats, untilMax, mLatched, until[SC_FMOTION]);
    // 7. a camera-closing latch -> STOCK (the caller restores the handler); the preflight never runs on a latched launch
    if (latched) {
        snprintf(gSCLatchMsg, sizeof gSCLatchMsg, "simcam: %s latched after %s crashes/hangs in the last 20 activations or 24 h; no camera. "
                 "Relaunch with SIMCAM_SENTINEL=clear to retry (events: %s)", feats, nums, gSCPath);
        SCEventRaw("activate", "-", "latched=1 traced=", atomic_load(&gSCTraced));
        return SCP_LATCHED;
    }
    return SCP_OK;
}

// ---- START 1-3 (after ARMED; nobody waits for it): the sentinel goes live ----
static char gActSwapExc[128];   // SWAP's @catch copies the exception name here; START 1 records it (the launch's one strike)
static void SCStart(void) {
    if (gSCFd < 0) return;   // sentinel off, or events.log could not be opened
    SCEventRaw("activate", "-", "traced=", atomic_load(&gSCTraced));
    if (gActSwapExc[0] && ActStrikeOnce()) { char d[160]; snprintf(d, sizeof d, "exception=%s", gActSwapExc); SCEventRaw("crash", "activate", d, -1); }
    atomic_store(&gSCOn, 1);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, SCWatchdog, NULL);
    atexit_b(^{ SCEventRaw("exit", "-", "atexit", -1); });
    LOG(@"sentinel: activation %d build %s traced=%d scale=x%.0f events=%s (self %#lx-%#lx, %d related images)", gSCAct, gSCBuild,
        atomic_load(&gSCTraced), gSCScale, gSCPath, (unsigned long)gSCSelf.lo, (unsigned long)gSCSelf.hi, atomic_load(&gSCNRelated));
    // Conformance hook (conformance tests): SIMCAM_WD_TEST=
    //   hang        two photo ops that never end (1 s timeout) -> 2 hangs -> in-process latch; next activation latched
    //   pause       a photo op with a 3 s timeout whose work takes 2 s; stop the process from the host for >3 s meanwhile
    //               (kill -STOP <pid>; sleep 10; kill -CONT <pid>) -> no hang
    //   crash       a photo op, then a fault inside our image 1 s later -> "crash photo"; twice -> latched next activation
    //   crash-other abort() with no op inflight -> "crash-other", never counted
    const char *wt = getenv("SIMCAM_WD_TEST");
    if (wt) {
        LOG(@"sentinel: test %s", wt);
        dispatch_queue_t q = dispatch_queue_create("simcam.wdtest", DISPATCH_QUEUE_SERIAL);
        if (!strcmp(wt, "hang")) { SCOpBegin("photo", 1.0); dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), q, ^{ SCOpBegin("photo", 1.0); }); }
        else if (!strcmp(wt, "pause")) dispatch_async(q, ^{ int op = SCOpBegin("photo", 3.0); LOG(@"sentinel: test pause op started, pid %d", getpid()); sleep(2); SCOpEnd(op); LOG(@"sentinel: test pause op ended"); });
        else if (!strcmp(wt, "crash")) dispatch_async(q, ^{ SCOpBegin("photo", 30.0); sleep(1); volatile int *z = (volatile int *)(uintptr_t)atoi("0"); *z = 1; });
        else if (!strcmp(wt, "crash-other")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), q, ^{ abort(); });
    }
}
