// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/sentinel_activate.inc): the op trace ring
// that `stuntcam trace` reads (design §4.5, §9's shutter mark).
//
// Ring. A fixed array of TR_N (64) entries per process. TRRecord(op, settingsID, arg) takes the next index with one
// fetch_add and fills that slot: every field is a relaxed atomic, and a per-slot sequence number (0 while the slot is
// being written, index + 1 once done) makes the reader's copy a seqlock (a reader that sees the number change retries
// nothing; it skips the slot). No lock, no allocation, no syscall apart from the two clock reads: callable on any
// thread, and the reader is async-signal-safe. Two writers on one slot at once (64 ops recorded while one write is in
// progress) can mix fields; the sequence number still names the later one.
//
// Ops (the name is what the file and the CLI show; sid = the AVCapturePhotoSettings uniqueID / Fig SettingsID):
//   activate          the camera armed (ActRun, after ARMED)
//   session-start     sess_StartRunning posted DidStartRunning (arg = configuration id)
//   session-stop      sess_StopRunning entered
//   shutter           -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] called by the app (client hook)
//   prepare           Fig slot 13, IrisStillImageSinkPrepareToCapture (sess_IrisPrepare)
//   slot12            Fig slot 12, IrisStillImageSinkCaptureImage (sess_IrisCapture entry): AVF hands the request to
//                     the session; the design's "slot-12 mark" (§9)
//   will-capture      IrisWillCaptureStillImage posted (pump queue)
//   did-capture       the source frame taken, IrisDidCaptureStillImage posted (pump queue)
//   encode-start      the photo queue starts this photo's encode
//   encoded           encode done (arg = bytes)
//   timestamp         the PresentationTimestamp the photo carries (AVCapturePhoto.timestamp), assigned when the source
//                     frame is taken (pump queue, right after will-capture; L68): its mach time is that instant, arg =
//                     the same in host-clock nanoseconds. An error completion delivers no photo, so none carries it.
//   still-complete    IrisStillImageComplete posted (arg = ErrorStatus; a latch or the photo-error hook gives non-zero)
//   did-finish        AVF ran the delegate's didFinishCaptureForResolvedSettings: (the photo-done mark; ledger L66)
//   movie-start       FileSinkStartRecording posted DidStartRecording (sid = movie settings id; a failed writer:
//                     movie-stop)
//   movie-stop        DidStopRecording posted (arg = status)
//   latch-<feature>   the watchdog latched a feature in-process (two hangs)
//   crash             the crash handler ran (arg = signal); written from the handler itself
// Each op carries the host clock twice: CLOCK_REALTIME (ns since 1970) and mach_absolute_time() (the host-time clock
// that CMClockGetHostTimeClock and AVCapturePhoto.timestamp use; the file gives the timebase).
//
// File. <state dir>/<bundle>/trace-<pid>.txt, the directory events.log is in (gSCIdx: SIMCAM_STATE_DIR, else the
// container-tmp fallback, design §4.3); with the sentinel off, the first of the two that can be written. Rewritten
// whole (temp file + rename, so a reader never sees half a file) by a utility queue at most once per kTRFlushMs after
// an op, at exit, and by the crash handler. Nothing is recorded or written before the camera arms, so an app that never
// uses the camera writes none. The newest kTRKeep files per bundle are kept (pruned when a process writes its first).
// Robustness:
// - the directory is picked when the camera arms (TRArmFile, on the activation queue right after ActRun releases its
//   waiters), not at the first flush, so a crash in the first kTRFlushMs still writes a trace
//   (SIMCAM_OFF_TRACE_DIR_AT_ARM=1, ledger L69's mutant, picks it at the first flush);
// - a write that finds the directory gone or unwritable (open/write/rename failing) picks again, once per process, in
//   the same order (a removed state dir is made again), and logs that once (ledger L71); the paths live in two slots
//   and the crash handler reads the current slot's index, so a re-pick never changes a path it may be reading;
// - only the first thread in the crash handler writes the file (gTRCrash: 0 idle, 1 writing); another one records its
//   crash op and waits for that write (bounded), so two crashes at once never share the buffer (ledger L70). The flag
//   goes back to 0 after the write, so a later crash that reaches this handler (SCChain restores the previous handler
//   for the signal it chains) writes again. SIMCAM_OFF_TRACE_CRASH_CLAIM=1 (ledger L70's mutant): no CAS, every thread
//   formats into the one buffer and writes the one temp file, 20 ms apart;
// - a write that fails because the directory is gone, replaced or unwritable (ENOENT, ENOTDIR, EACCES, EPERM, EROFS)
//   re-picks; any other error (ENOSPC, EIO, ...) is logged once and doesn't spend the re-pick.
// Format (text, one record a line):
//   stuntcam-trace 1
//   pid <pid>
//   bundle <bundle id>
//   build <engine build>
//   timebase <numer> <denom>                 mach ticks * numer / denom = ns
//   written <realtime ns> <mach>             when this copy was written
//   recorded <n>                             ops recorded since the process started (the ring keeps the last 64)
//   op <index> <realtime ns> <mach> <name> <sid> <arg>     oldest first; sid -1 = none
//   end
// SIMCAM_TRACE=0 turns it off (nothing recorded, no shutter hook, no file).
#include <mach/mach_time.h>

#define TR_N 64
#define TR_BUFSZ 12288
static const long kTRFlushMs = 150;
static const int kTRKeep = 8;
enum {
    TR_NONE = 0, TR_ACTIVATE, TR_SESSION_START, TR_SESSION_STOP, TR_SHUTTER, TR_PREPARE, TR_SLOT12, TR_WILL_CAPTURE,
    TR_DID_CAPTURE, TR_ENCODE_START, TR_ENCODED, TR_TIMESTAMP, TR_STILL_COMPLETE, TR_DID_FINISH, TR_MOVIE_START,
    TR_MOVIE_STOP, TR_LATCH, TR_CRASH, TR_NOPS
};
static const char *const kTRName[TR_NOPS] = {
    "?", "activate", "session-start", "session-stop", "shutter", "prepare", "slot12", "will-capture", "did-capture",
    "encode-start", "encoded", "timestamp", "still-complete", "did-finish", "movie-start", "movie-stop", "latch",
    "crash"};
typedef struct { _Atomic uint64_t seq, wall, mach; _Atomic int64_t sid, arg; _Atomic int op; } TREnt;
static TREnt gTR[TR_N];
static _Atomic uint64_t gTRNext;          // ops recorded (the next op's index)
static _Atomic int gTROn;                 // set (release) once the camera armed and SIMCAM_TRACE isn't 0
static _Atomic int gTRKicked;             // a flush is scheduled
typedef struct { char dir[1100], path[1200], tmp[1200], sigTmp[1200]; } TRPaths;
static TRPaths gTRP[2];   // slot 0: the first pick; slot 1: the one re-pick (never rewritten once published)
// the slot in use (release once its paths are set): the handler and the flushes write there
static _Atomic int gTRCur = -1;
static int gTRRepicked;                   // the one re-pick is spent (trace queue only)
static _Atomic int gTRCrash;   // the crash handler's write: 0 idle, 1 a thread is writing (back to 0 after it)
// SIMCAM_OFF_TRACE_CRASH_CLAIM=1 (L70's mutant), read at arm time (never getenv in the handler)
static int gTROldCrash;
static mach_timebase_info_data_t gTRTB;
static char gTRSigBuf[TR_BUFSZ];   // the crash handler's buffer (never shared with the queue's or the exit flush's)

static inline uint64_t TRWallNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}
// one op into the ring: atomics only (async-signal-safe); no flush
static void TRPut(int op, int64_t sid, int64_t arg, uint64_t mach, uint64_t wall) {
    uint64_t i = atomic_fetch_add_explicit(&gTRNext, 1, memory_order_relaxed);
    TREnt *e = &gTR[i % TR_N];
    atomic_store_explicit(&e->seq, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->wall, wall, memory_order_relaxed);
    atomic_store_explicit(&e->mach, mach, memory_order_relaxed);
    atomic_store_explicit(&e->sid, sid, memory_order_relaxed);
    atomic_store_explicit(&e->arg, arg, memory_order_relaxed);
    atomic_store_explicit(&e->op, op, memory_order_relaxed);
    atomic_store_explicit(&e->seq, i + 1, memory_order_release);
}
typedef struct { uint64_t wall, mach; int64_t sid, arg; int op; } TRSnap;
static BOOL TRGet(uint64_t i, TRSnap *o) {   // async-signal-safe
    TREnt *e = &gTR[i % TR_N];
    uint64_t seqBefore = atomic_load_explicit(&e->seq, memory_order_acquire);
    if (seqBefore != i + 1) return NO;
    o->wall = atomic_load_explicit(&e->wall, memory_order_relaxed);
    o->mach = atomic_load_explicit(&e->mach, memory_order_relaxed);
    o->sid = atomic_load_explicit(&e->sid, memory_order_relaxed);
    o->arg = atomic_load_explicit(&e->arg, memory_order_relaxed);
    o->op = atomic_load_explicit(&e->op, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&e->seq, memory_order_relaxed) == seqBefore;
}

// ---- the file (async-signal-safe formatting: no stdio, no malloc) ----
static int TRPutI(char *b, int n, int cap, int64_t v) {   // a signed decimal
    if (v < 0) {
        n = SCPutS(b, n, cap, "-");
        return SCPutU(b, n, cap, (unsigned long)(-(v + 1)) + 1);
    }
    return SCPutU(b, n, cap, (unsigned long)v);
}
static int TRFormat(char *b, int cap) {
    int n = 0;
    n = SCPutS(b, n, cap, "stuntcam-trace 1\npid ");
    n = SCPutU(b, n, cap, (unsigned long)getpid());
    n = SCPutS(b, n, cap, "\nbundle ");
    n = SCPutS(b, n, cap, gSCBundle);
    n = SCPutS(b, n, cap, "\nbuild ");
    n = SCPutS(b, n, cap, gSCBuild);
    n = SCPutS(b, n, cap, "\ntimebase ");
    n = SCPutU(b, n, cap, gTRTB.numer);
    n = SCPutS(b, n, cap, " ");
    n = SCPutU(b, n, cap, gTRTB.denom);
    n = SCPutS(b, n, cap, "\nwritten ");
    n = SCPutU(b, n, cap, (unsigned long)TRWallNs());
    n = SCPutS(b, n, cap, " ");
    n = SCPutU(b, n, cap, (unsigned long)mach_absolute_time());
    uint64_t next = atomic_load_explicit(&gTRNext, memory_order_acquire);
    n = SCPutS(b, n, cap, "\nrecorded ");
    n = SCPutU(b, n, cap, (unsigned long)next);
    n = SCPutS(b, n, cap, "\n");
    for (uint64_t i = next > TR_N ? next - TR_N : 0; i < next && n < cap - 160; i++) {
        TRSnap s;
        if (!TRGet(i, &s) || s.op <= TR_NONE || s.op >= TR_NOPS) continue;   // being written, or overwritten meanwhile
        n = SCPutS(b, n, cap, "op ");
        n = SCPutU(b, n, cap, (unsigned long)i);
        n = SCPutS(b, n, cap, " ");
        n = SCPutU(b, n, cap, (unsigned long)s.wall);
        n = SCPutS(b, n, cap, " ");
        n = SCPutU(b, n, cap, (unsigned long)s.mach);
        n = SCPutS(b, n, cap, " ");
        n = SCPutS(b, n, cap, kTRName[s.op]);
        if (s.op == TR_LATCH && s.arg >= 0 && s.arg < SC_NFEAT) {
            n = SCPutS(b, n, cap, "-");
            n = SCPutS(b, n, cap, gSCFeatures[s.arg]);
        }
        n = SCPutS(b, n, cap, " ");
        n = TRPutI(b, n, cap, s.sid);
        n = SCPutS(b, n, cap, " ");
        n = TRPutI(b, n, cap, s.arg);
        n = SCPutS(b, n, cap, "\n");
    }
    n = SCPutS(b, n, cap, "end\n");
    return n;
}
// write n bytes of buf to tmp, then rename it over path. Async-signal-safe. Returns 0, or the errno of the step that
// failed.
static int TRWriteBuf(const char *buf, int n, const char *tmp, const char *path) {
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return errno ? errno : EIO;
    for (int off = 0; off < n;) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w <= 0) {
            int e = w < 0 && errno ? errno : EIO;
            close(fd);
            unlink(tmp);
            return e;
        }
        off += (int)w;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        int e = errno ? errno : EIO;
        unlink(tmp);
        return e;
    }
    return 0;
}
// the whole ring, formatted into buf, written as above
static int TRWriteTo(char *buf, int cap, const char *tmp, const char *path) {
    return TRWriteBuf(buf, TRFormat(buf, cap), tmp, path);
}
// other processes' traces: the newest kTRKeep - 1 stay (ours is about to be written), and temp files over a minute old
// go
typedef struct { char name[64]; long mt; } TRFile;
static void TRPrune(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    TRFile files[64];
    int nFiles = 0;
    long now = time(NULL);
    char own[64];
    snprintf(own, sizeof own, "trace-%d.txt", getpid());
    while ((e = readdir(d))) {
        char p[1400];
        struct stat st;
        if (e->d_name[0] == '.' && !strncmp(e->d_name, ".trace-", 7) && strstr(e->d_name, ".tmp")) {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (stat(p, &st) == 0 && st.st_ctime < now - 60) unlink(p);
            continue;
        }
        size_t l = strlen(e->d_name);
        if (strncmp(e->d_name, "trace-", 6) || l < 11 || strcmp(e->d_name + l - 4, ".txt") || !strcmp(e->d_name, own) ||
            l >= sizeof files[0].name)
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (nFiles < 64) {
            snprintf(files[nFiles].name, sizeof files[nFiles].name, "%s", e->d_name);
            files[nFiles].mt = st.st_mtime;
            nFiles++;
        }
    }
    closedir(d);
    // newest first; past kTRKeep - 1, unlink
    for (int i = 1; i < nFiles; i++) {
        for (int j = i; j > 0 && files[j].mt > files[j - 1].mt; j--) {
            TRFile t = files[j];
            files[j] = files[j - 1];
            files[j - 1] = t;
        }
    }
    for (int i = kTRKeep - 1; i < nFiles; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, files[i].name);
        unlink(p);
    }
}
// pick a directory into slot `slot` (on the trace queue): events.log's (gSCIdx), else the state dir, else the
// container-tmp fallback; the first that exists or can be made and takes a test file. Publishes the slot. Returns the
// gSCDir index or -1.
static int TRPickInto(int slot) {
    int order[3] = {gSCIdx, 0, 1};
    for (int k = 0; k < 3; k++) {
        int i = order[k];
        if (i < 0 || i > 1 || !gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
        char tmp[1200];
        snprintf(tmp, sizeof tmp, "%s/.trace-%d.tmp", gSCDir[i], getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        close(fd);
        unlink(tmp);
        TRPaths *p = &gTRP[slot];
        snprintf(p->dir, sizeof p->dir, "%s", gSCDir[i]);
        snprintf(p->path, sizeof p->path, "%s/trace-%d.txt", p->dir, getpid());
        snprintf(p->tmp, sizeof p->tmp, "%s/.trace-%d.tmp", p->dir, getpid());
        snprintf(p->sigTmp, sizeof p->sigTmp, "%s/.trace-%d.sig.tmp", p->dir, getpid());
        TRPrune(p->dir);
        atomic_store_explicit(&gTRCur, slot, memory_order_release);
        return i;
    }
    return -1;
}
static void TRPickF(void *ctx) {   // on TRQ: the first pick (TRArmFile, or the first flush)
    if (atomic_load_explicit(&gTRCur, memory_order_acquire) >= 0) return;
    int i = TRPickInto(0);
    if (i >= 0) {
        LOG(@"trace: %s (%s)", gTRP[0].path, i == 0 ? "state dir" : "container-tmp fallback");
        return;
    }
    static int warned;
    if (!warned++) LOG(@"trace: cannot write a trace file in %s or %s", gSCDir[0][0] ? gSCDir[0] : "-", gSCDir[1]);
}
static dispatch_queue_t TRQ(void) {
    static dispatch_queue_t q;
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        q = dispatch_queue_create("simcam.trace",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    return q;
}
static void TRFlushF(void *ctx) {   // on TRQ
    static char buf[TR_BUFSZ];
    atomic_store(&gTRKicked, 0);   // first: an op recorded while this writes schedules the next write
    TRPickF(NULL);
    int cur = atomic_load_explicit(&gTRCur, memory_order_acquire);
    if (cur < 0) return;
    int e = TRWriteTo(buf, sizeof buf, gTRP[cur].tmp, gTRP[cur].path);
    if (!e || gTRRepicked) return;
    // not a directory gone, replaced or unwritable: another pick wouldn't help
    if (e != ENOENT && e != ENOTDIR && e != EACCES && e != EPERM && e != EROFS) {
        static int noted;
        if (!noted++)
            LOG(@"trace: writing %s failed (%s); not re-picking (only a missing or read-only directory is); later "
                @"writes retry there",
                gTRP[cur].path, strerror(e));
        return;
    }
    // the directory went away or became unwritable: pick again, once (slot 1), and write there.
    // SIMCAM_OFF_TRACE_REPICK=1 (ledger L71's mutant): give up silently
    gTRRepicked = 1;
    if (getenv(ENV_OFF_TRACE_REPICK)) return;
    int i = TRPickInto(1);
    int e2 = i >= 0 ? TRWriteTo(buf, sizeof buf, gTRP[1].tmp, gTRP[1].path) : 0;
    if (i < 0)
        LOG(@"trace: writing %s failed (%s): its directory is gone or read-only and no other one takes a file; the "
            @"trace stops here",
            gTRP[cur].path, strerror(e));
    else
        LOG(@"trace: writing %s failed (%s): its directory is gone or read-only; re-picked once, the trace is now %s "
            @"(%s)%s%s",
            gTRP[cur].path, strerror(e), gTRP[1].path, i == 0 ? "state dir, made again" : "container-tmp fallback",
            e2 ? "; that write failed too: " : "", e2 ? strerror(e2) : "");
}
static void TRKick(void) {
    if (atomic_exchange(&gTRKicked, 1)) return;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, kTRFlushMs * NSEC_PER_MSEC), TRQ(), NULL, TRFlushF);
}
static void TRRecord(int op, int64_t sid, int64_t arg) {
    if (!atomic_load_explicit(&gTROn, memory_order_acquire)) return;
    TRPut(op, sid, arg, mach_absolute_time(), TRWallNs());
    TRKick();
}
// the realtime of an earlier (or later) mach instant: now's realtime moved by the mach difference. Works with the trace
// off.
static uint64_t TRWallAt(uint64_t mach) {
    mach_timebase_info_data_t tb = gTRTB;
    if (!tb.denom) mach_timebase_info(&tb);
    if (!tb.denom) {
        tb.numer = 1;
        tb.denom = 1;
    }
    uint64_t now = mach_absolute_time(), wall = TRWallNs();
    return mach <= now ? wall - (now - mach) * tb.numer / tb.denom : wall + (mach - now) * tb.numer / tb.denom;
}
// an op at an earlier mach instant whose realtime the caller computed (TRWallAt), so it can use the same value
// elsewhere (the photo's EXIF time)
static void TRRecordAtWall(int op, int64_t sid, int64_t arg, uint64_t mach, uint64_t wall) {
    if (!atomic_load_explicit(&gTROn, memory_order_acquire)) return;
    TRPut(op, sid, arg, mach, wall);
    TRKick();
}
// an op at an earlier mach instant (the photo's timestamp): its realtime is moved back by the same amount
static void TRRecordAt(int op, int64_t sid, int64_t arg, uint64_t mach) {
    if (!atomic_load_explicit(&gTROn, memory_order_acquire)) return;
    TRRecordAtWall(op, sid, arg, mach, TRWallAt(mach));
}
static int64_t TRSettingsID(id settings) {
    static SEL sel;
    if (!sel) sel = sel_registerName("uniqueID");
    return settings && [settings respondsToSelector:sel] ? ((int64_t (*)(id, SEL))objc_msgSend)(settings, sel) : -1;
}
// ActRun, right after ARMED (activation thread): recording starts; the activate op; the exit flush
static void TRArm(void) {
    const char *e = getenv(ENV_TRACE);
    if (e && e[0] == '0') {
        LOG(@"trace: off (SIMCAM_TRACE=0)");
        return;
    }
    mach_timebase_info(&gTRTB);
    if (!gTRTB.denom) {
        gTRTB.numer = 1;
        gTRTB.denom = 1;
    }
    gTROldCrash = getenv(ENV_OFF_TRACE_CRASH_CLAIM) != NULL;
    if (gTROldCrash)
        LOG(@"SIMCAM_OFF_TRACE_CRASH_CLAIM: every crashing thread writes the trace, from one buffer (L70's mutant)");
    atomic_store_explicit(&gTROn, 1, memory_order_release);
    TRRecord(TR_ACTIVATE, -1, 0);
    atexit_b(^{
        dispatch_sync_f(TRQ(), NULL, TRFlushF);
    });
}
// ActRun, after it released its waiters: the trace file's directory, so the crash handler can write from the first
// op on. On the activation queue (nobody waits for it; inline in the test modes), synchronously on the trace queue,
// where every pick happens. SIMCAM_OFF_TRACE_DIR_AT_ARM=1 (ledger L69's mutant) leaves it to the first flush.
static void TRArmFile(void) {
    if (!atomic_load_explicit(&gTROn, memory_order_acquire)) return;
    if (getenv(ENV_OFF_TRACE_DIR_AT_ARM)) {
        LOG(@"SIMCAM_OFF_TRACE_DIR_AT_ARM: the trace directory is picked at the first flush");
        return;
    }
    dispatch_sync_f(TRQ(), NULL, TRPickF);
}
// the crash handler (async-signal-safe): the crash op, then the whole ring, from its own buffer and temp file. The
// first thread here writes; one that comes while it writes waits for that (up to ~500 ms), then returns to the chain.
// The flag goes back to 0 after the write, so a later crash (after a fault the chain recovered from) writes again.
static void TRCrash(int sig) {
    if (!atomic_load_explicit(&gTROn, memory_order_acquire)) return;
    TRPut(TR_CRASH, -1, sig, mach_absolute_time(), TRWallNs());
    int saved = errno, idle = 0;   // errno as the chained handler expects it
    // SIMCAM_OFF_TRACE_CRASH_CLAIM (L70's mutant): no CAS; format, 20 ms, write: two threads share the buffer and
    // temp file
    if (gTROldCrash) {
        int cur = atomic_load_explicit(&gTRCur, memory_order_acquire);
        if (cur >= 0) {
            int n = TRFormat(gTRSigBuf, sizeof gTRSigBuf);
            struct timespec d = {0, 20000000};
            nanosleep(&d, NULL);
            (void)TRWriteBuf(gTRSigBuf, n, gTRP[cur].sigTmp, gTRP[cur].path);
        }
        errno = saved;
        return;
    }
    if (!atomic_compare_exchange_strong(&gTRCrash, &idle, 1)) {
        struct timespec ms = {0, 1000000};
        for (int i = 0; i < 500 && atomic_load(&gTRCrash) == 1; i++) nanosleep(&ms, NULL);
        errno = saved;
        return;
    }
    int cur = atomic_load_explicit(&gTRCur, memory_order_acquire);
    if (cur >= 0) (void)TRWriteTo(gTRSigBuf, sizeof gTRSigBuf, gTRP[cur].sigTmp, gTRP[cur].path);
    atomic_store(&gTRCrash, 0);   // a later crash that reaches this handler writes again
    errno = saved;
}
static void TRLatch(int fi) { TRRecord(TR_LATCH, -1, fi); }
