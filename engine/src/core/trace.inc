// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/sentinel_activate.inc): the op trace ring
// that `stuntcam trace` reads: each photo's steps from the shutter to the delegate, with host-clock times.
//
// Provides: TraceRecord, kTraceOp* (providers/pump.inc, providers/photo.inc, client/photo.inc)
//           TraceWallAt, TraceRecordAt, TraceRecordAtWall (providers/photo.inc); TraceSettingsID (client/photo.inc)
//           TraceArm, TraceArmFile (core/lazy.inc); TraceCrash (core/crash_handler.inc); TraceLatch (core/safety.inc)
// Owns:     nothing shared: the ring and the trace file's paths (gTrace, core/trace.h) are only touched here.
// Uses:     core/safety.inc, core/markers.inc
//
// Ring. A fixed array of kTraceRingSize (64) entries per process. TraceRecord(op, settingsID, arg) takes the next index
// with one fetch_add and fills that slot. Every field is a relaxed atomic, and a per-slot sequence number (0 while the
// slot is being written, index + 1 once done) makes the reader's copy a seqlock: a reader that sees the number change
// doesn't retry, it skips the slot. There is no lock, no allocation and no syscall apart from the two clock reads, so
// TraceRecord can be called on any thread, and the reader is async-signal-safe. Two writers on one slot at once (64 ops
// recorded while one write is in progress) can mix fields; the sequence number still names the later one.
//
// Ops (the name is what the file and the CLI show; sid is the AVCapturePhotoSettings uniqueID, the Fig SettingsID):
//   activate          the camera armed (ActivationRun, after ARMED)
//   session-start     sess_StartRunning posted DidStartRunning (arg = configuration id)
//   session-stop      sess_StopRunning entered
//   shutter           -[AVCapturePhotoOutput capturePhotoWithSettings:delegate:] called by the app (client hook)
//   prepare           Fig slot 13, IrisStillImageSinkPrepareToCapture (sess_IrisPrepare)
//   slot12            Fig slot 12, IrisStillImageSinkCaptureImage (sess_IrisCapture entry): AVF hands the request to
//                     the session (the "slot-12 mark" a photo's timestamp is measured against)
//   will-capture      IrisWillCaptureStillImage posted (pump queue)
//   did-capture       the source frame taken, IrisDidCaptureStillImage posted (pump queue)
//   encode-start      the photo queue starts this photo's encode
//   encoded           encode done (arg = bytes)
//   timestamp         the PresentationTimestamp the photo carries (AVCapturePhoto.timestamp), assigned when the source
//                     frame is taken (pump queue, right after will-capture, as a device stamps the sensor frame).
//                     Its mach time is that instant and arg is the same in host-clock nanoseconds. An error completion
//                     delivers no photo, so none carries it.
//   still-complete    IrisStillImageComplete posted (arg = ErrorStatus; a latch or the photo-error hook gives non-zero)
//   did-finish        AVF ran the delegate's didFinishCaptureForResolvedSettings: (the photo-done mark)
//   movie-start       FileSinkStartRecording posted DidStartRecording (sid = movie settings id; a failed writer posts
//                     movie-stop instead)
//   movie-stop        DidStopRecording posted (arg = status)
//   latch-<feature>   the watchdog latched a feature in-process (two hangs)
//   crash             the crash handler ran (arg = signal); written from the handler itself
// Each op carries the host clock twice: CLOCK_REALTIME (ns since 1970) and mach_absolute_time() (the host-time clock
// that CMClockGetHostTimeClock and AVCapturePhoto.timestamp use; the file gives the timebase).
//
// File. <state dir>/<bundle>/trace-<pid>.txt, in the directory events.log is in (gSentinel.events.dirIndex:
// SIMCAM_STATE_DIR, else the container-tmp fallback); with the sentinel off, the first of the two that can be written.
// The file is rewritten whole (a temp file, then a rename, so a reader never sees half a file) by a utility queue at
// most once per kTraceFlushMs after an op, at exit, and by the crash handler. Nothing is recorded or written before the
// camera arms, so an app that never uses the camera writes no trace. The newest kTraceKeepFiles files per bundle are
// kept (pruned when a process writes its first). Robustness:
// - The directory is picked when the camera arms (TraceArmFile, on the activation queue right after ActivationRun
//   releases its waiters), not at the first flush, so a crash in the first kTraceFlushMs still writes a trace
//   (SIMCAM_OFF_TRACE_DIR_AT_ARM=1 picks it at the first flush).
// - A write that finds the directory gone or unwritable (open, write or rename failing) picks again, once per process,
//   in the same order (a removed state dir is made again), and logs that once. The paths live in two slots and
//   the crash handler reads the current slot's index, so a re-pick never changes a path it may be reading.
// - Only the first thread in the crash handler writes the file (gTrace.crashWriting: 0 idle, 1 writing). Another one
//   records its crash op and waits for that write (bounded), so two crashes at once never share the buffer. The
//   flag goes back to 0 after the write, so a later crash that reaches this handler (SentinelChain restores the
//   previous handler for the signal it chains) writes again. With SIMCAM_OFF_TRACE_CRASH_CLAIM=1 there is no CAS: every
//   thread formats into the one buffer and writes the one temp file, 20 ms apart.
// - A write that fails because the directory is gone, replaced or unwritable (ENOENT, ENOTDIR, EACCES, EPERM, EROFS)
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
#include "trace.h"
#include "sentinel.h"

// The trace's state. core/trace.h describes each field and who owns it.
static struct Trace gTrace = {.pathSlot = -1};

#define kTraceOpLineBytes 160         // TraceFormat stops adding ops when less than one op line's room is left
#define kTraceSlotFirst 0
#define kTraceSlotRepick 1
#define kTracePruneMaxFiles 64        // other processes' traces TracePrune considers
#define kTraceFileNameBytes 64
#define kTraceNamePrefixLen 6                  // strlen("trace-")
#define kTraceNameSuffixLen 4                  // strlen(".txt")
#define kTraceMinNameLen 11                    // "trace-" + one pid digit + ".txt"
#define kTraceTempPrefixLen 7                  // strlen(".trace-")
#define kTraceStaleTempSeconds 60              // a temp file this old was left by a process that died mid-write
#define kTraceCrashWaitSliceNs 1000000         // a crashing thread that finds another one writing waits in 1 ms slices
#define kTraceCrashWaitSlices 500              // up to 500 of them
#define kTraceUnclaimedWriteDelayNs 20000000   // SIMCAM_OFF_TRACE_CRASH_CLAIM's 20 ms between format and write
static const long kTraceFlushMs = 150;
static const int kTraceKeepFiles = 8;

static const char *const kTraceOpNames[kTraceOpCount] = {
    [kTraceOpNone] = "?",
    [kTraceOpActivate] = "activate",
    [kTraceOpSessionStart] = "session-start",
    [kTraceOpSessionStop] = "session-stop",
    [kTraceOpShutter] = "shutter",
    [kTraceOpPrepare] = "prepare",
    [kTraceOpSlot12] = "slot12",
    [kTraceOpWillCapture] = "will-capture",
    [kTraceOpDidCapture] = "did-capture",
    [kTraceOpEncodeStart] = "encode-start",
    [kTraceOpEncoded] = "encoded",
    [kTraceOpTimestamp] = "timestamp",
    [kTraceOpStillComplete] = "still-complete",
    [kTraceOpDidFinish] = "did-finish",
    [kTraceOpMovieStart] = "movie-start",
    [kTraceOpMovieStop] = "movie-stop",
    [kTraceOpLatch] = "latch",
    [kTraceOpCrash] = "crash",
};

// SIMCAM_OFF_TRACE_CRASH_CLAIM=1, read at arm time (never getenv in the handler)
static int gOffTraceCrashClaim;

static inline uint64_t TraceWallNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

// one op into the ring: atomics only (async-signal-safe); no flush
static void TracePut(int op, int64_t settingsID, int64_t arg, uint64_t mach, uint64_t wall) {
    uint64_t index = atomic_fetch_add_explicit(&gTrace.recorded, 1, memory_order_relaxed);
    TraceEntry *entry = &gTrace.ring[index % kTraceRingSize];
    atomic_store_explicit(&entry->seq, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&entry->wall, wall, memory_order_relaxed);
    atomic_store_explicit(&entry->mach, mach, memory_order_relaxed);
    atomic_store_explicit(&entry->settingsID, settingsID, memory_order_relaxed);
    atomic_store_explicit(&entry->arg, arg, memory_order_relaxed);
    atomic_store_explicit(&entry->op, op, memory_order_relaxed);
    atomic_store_explicit(&entry->seq, index + 1, memory_order_release);
}

typedef struct {
    uint64_t wall;
    uint64_t mach;
    int64_t settingsID;
    int64_t arg;
    int op;
} TraceSnapshot;

static BOOL TraceGet(uint64_t index, TraceSnapshot *out) {   // async-signal-safe
    TraceEntry *entry = &gTrace.ring[index % kTraceRingSize];
    uint64_t seqBefore = atomic_load_explicit(&entry->seq, memory_order_acquire);
    if (seqBefore != index + 1) {
        return NO;
    }
    out->wall = atomic_load_explicit(&entry->wall, memory_order_relaxed);
    out->mach = atomic_load_explicit(&entry->mach, memory_order_relaxed);
    out->settingsID = atomic_load_explicit(&entry->settingsID, memory_order_relaxed);
    out->arg = atomic_load_explicit(&entry->arg, memory_order_relaxed);
    out->op = atomic_load_explicit(&entry->op, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&entry->seq, memory_order_relaxed) == seqBefore;
}

// ---- the file (async-signal-safe formatting: no stdio, no malloc) ----
static int TracePutSigned(char *buf, int len, int cap, int64_t value) {   // a signed decimal
    if (value < 0) {
        len = SentinelPutString(buf, len, cap, "-");
        return SentinelPutUnsigned(buf, len, cap, (unsigned long)(-(value + 1)) + 1);
    }
    return SentinelPutUnsigned(buf, len, cap, (unsigned long)value);
}

static int TraceFormat(char *buf, int cap) {
    int len = 0;
    len = SentinelPutString(buf, len, cap, "stuntcam-trace 1\npid ");
    len = SentinelPutUnsigned(buf, len, cap, (unsigned long)getpid());
    len = SentinelPutString(buf, len, cap, "\nbundle ");
    len = SentinelPutString(buf, len, cap, gSentinel.bundle);
    len = SentinelPutString(buf, len, cap, "\nbuild ");
    len = SentinelPutString(buf, len, cap, gSentinel.build);
    len = SentinelPutString(buf, len, cap, "\ntimebase ");
    len = SentinelPutUnsigned(buf, len, cap, gTrace.timebase.numer);
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutUnsigned(buf, len, cap, gTrace.timebase.denom);
    len = SentinelPutString(buf, len, cap, "\nwritten ");
    len = SentinelPutUnsigned(buf, len, cap, (unsigned long)TraceWallNs());
    len = SentinelPutString(buf, len, cap, " ");
    len = SentinelPutUnsigned(buf, len, cap, (unsigned long)mach_absolute_time());
    uint64_t next = atomic_load_explicit(&gTrace.recorded, memory_order_acquire);
    len = SentinelPutString(buf, len, cap, "\nrecorded ");
    len = SentinelPutUnsigned(buf, len, cap, (unsigned long)next);
    len = SentinelPutString(buf, len, cap, "\n");

    for (uint64_t i = next > kTraceRingSize ? next - kTraceRingSize : 0; i < next && len < cap - kTraceOpLineBytes;
         i++) {
        TraceSnapshot snap;
        // skip a slot being written, or overwritten meanwhile
        if (!TraceGet(i, &snap) || snap.op <= kTraceOpNone || snap.op >= kTraceOpCount) {
            continue;
        }
        len = SentinelPutString(buf, len, cap, "op ");
        len = SentinelPutUnsigned(buf, len, cap, (unsigned long)i);
        len = SentinelPutString(buf, len, cap, " ");
        len = SentinelPutUnsigned(buf, len, cap, (unsigned long)snap.wall);
        len = SentinelPutString(buf, len, cap, " ");
        len = SentinelPutUnsigned(buf, len, cap, (unsigned long)snap.mach);
        len = SentinelPutString(buf, len, cap, " ");
        len = SentinelPutString(buf, len, cap, kTraceOpNames[snap.op]);
        if (snap.op == kTraceOpLatch && snap.arg >= 0 && snap.arg < kFeatureCount) {
            len = SentinelPutString(buf, len, cap, "-");
            len = SentinelPutString(buf, len, cap, kFeatureNames[snap.arg]);
        }
        len = SentinelPutString(buf, len, cap, " ");
        len = TracePutSigned(buf, len, cap, snap.settingsID);
        len = SentinelPutString(buf, len, cap, " ");
        len = TracePutSigned(buf, len, cap, snap.arg);
        len = SentinelPutString(buf, len, cap, "\n");
    }

    len = SentinelPutString(buf, len, cap, "end\n");
    return len;
}

// Writes len bytes of buf to tmp, then renames it over path. Async-signal-safe. Returns 0, or the errno of the step
// that failed.
static int TraceWriteBuf(const char *buf, int len, const char *tmp, const char *path) {
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return errno ? errno : EIO;
    }

    for (int offset = 0; offset < len;) {
        ssize_t written = write(fd, buf + offset, len - offset);
        if (written <= 0) {
            int err = written < 0 && errno ? errno : EIO;
            close(fd);
            unlink(tmp);
            return err;
        }
        offset += (int)written;
    }

    close(fd);

    if (rename(tmp, path) != 0) {
        int err = errno ? errno : EIO;
        unlink(tmp);
        return err;
    }

    return 0;
}

// the whole ring, formatted into buf, written as above
static int TraceWriteTo(char *buf, int cap, const char *tmp, const char *path) {
    return TraceWriteBuf(buf, TraceFormat(buf, cap), tmp, path);
}

// Other processes' traces: the newest kTraceKeepFiles - 1 stay (this process's trace is about to be written), and temp
// files over a minute old go.
typedef struct {
    char name[kTraceFileNameBytes];
    long mtime;
} TraceFile;

static void TracePrune(const char *dir) {
    DIR *dirStream = opendir(dir);
    if (!dirStream) {
        return;
    }
    struct dirent *entry;
    TraceFile files[kTracePruneMaxFiles];
    int nFiles = 0;
    long now = time(NULL);
    char own[kTraceFileNameBytes];
    snprintf(own, sizeof own, "trace-%d.txt", getpid());

    while ((entry = readdir(dirStream))) {
        char path[kSentinelEntryPathBytes];
        struct stat st;
        if (entry->d_name[0] == '.' && !strncmp(entry->d_name, ".trace-", kTraceTempPrefixLen) &&
            strstr(entry->d_name, ".tmp")) {
            snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
            if (stat(path, &st) == 0 && st.st_ctime < now - kTraceStaleTempSeconds) {
                unlink(path);
            }
            continue;
        }
        size_t nameLen = strlen(entry->d_name);
        BOOL traceName = !strncmp(entry->d_name, "trace-", kTraceNamePrefixLen) && nameLen >= kTraceMinNameLen &&
                         !strcmp(entry->d_name + nameLen - kTraceNameSuffixLen, ".txt");
        if (!traceName || !strcmp(entry->d_name, own) || nameLen >= sizeof files[0].name) {
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (nFiles < kTracePruneMaxFiles) {
            snprintf(files[nFiles].name, sizeof files[nFiles].name, "%s", entry->d_name);
            files[nFiles].mtime = st.st_mtime;
            nFiles++;
        }
    }

    closedir(dirStream);

    // newest first; past kTraceKeepFiles - 1, unlink
    for (int i = 1; i < nFiles; i++) {
        for (int j = i; j > 0 && files[j].mtime > files[j - 1].mtime; j--) {
            TraceFile held = files[j];
            files[j] = files[j - 1];
            files[j - 1] = held;
        }
    }

    for (int i = kTraceKeepFiles - 1; i < nFiles; i++) {
        char path[kSentinelEntryPathBytes];
        snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
        unlink(path);
    }
}

// Picks a directory into slot `slot` (on the trace queue): events.log's (gSentinel.events.dirIndex), else the state
// dir, else the container-tmp fallback; the first that exists or can be made and takes a test file. Publishes the slot.
// Returns the gSentinel.paths.eventsDirs index, or -1.
static int TracePickInto(int slot) {
    int order[3] = {gSentinel.events.dirIndex, kStateDirState, kStateDirTmp};

    for (int k = 0; k < 3; k++) {
        int dirIndex = order[k];
        if (dirIndex < 0 || dirIndex > kStateDirTmp || !gSentinel.paths.eventsDirs[dirIndex][0] ||
            SentinelMkdirP(gSentinel.paths.eventsDirs[dirIndex])) {
            continue;
        }
        char tmp[kTracePathBytes];
        snprintf(tmp, sizeof tmp, "%s/.trace-%d.tmp", gSentinel.paths.eventsDirs[dirIndex], getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            continue;
        }
        close(fd);
        unlink(tmp);
        TracePaths *paths = &gTrace.paths[slot];
        snprintf(paths->dir, sizeof paths->dir, "%s", gSentinel.paths.eventsDirs[dirIndex]);
        snprintf(paths->path, sizeof paths->path, "%s/trace-%d.txt", paths->dir, getpid());
        snprintf(paths->tmp, sizeof paths->tmp, "%s/.trace-%d.tmp", paths->dir, getpid());
        snprintf(paths->sigTmp, sizeof paths->sigTmp, "%s/.trace-%d.sig.tmp", paths->dir, getpid());
        TracePrune(paths->dir);
        atomic_store_explicit(&gTrace.pathSlot, slot, memory_order_release);
        return dirIndex;
    }

    return -1;
}

static void TracePickF(void *ctx) {   // on TraceQueue: the first pick (TraceArmFile, or the first flush)
    if (atomic_load_explicit(&gTrace.pathSlot, memory_order_acquire) >= 0) {
        return;
    }
    int dirIndex = TracePickInto(kTraceSlotFirst);
    if (dirIndex >= 0) {
        LOG(@"trace: %s (%s)", gTrace.paths[kTraceSlotFirst].path,
            dirIndex == kStateDirState ? "state dir" : "container-tmp fallback");
        return;
    }
    static int warned;
    if (!warned++) {
        LOG(@"trace: cannot write a trace file in %s or %s",
            gSentinel.paths.eventsDirs[kStateDirState][0] ? gSentinel.paths.eventsDirs[kStateDirState] : "-",
            gSentinel.paths.eventsDirs[kStateDirTmp]);
    }
}

static dispatch_queue_t TraceQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create(
            "simcam.trace", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    return queue;
}

// a write error that means the directory is gone, replaced or unwritable (another pick may help)
static BOOL TraceDirUnusable(int err) {
    return err == ENOENT || err == ENOTDIR || err == EACCES || err == EPERM || err == EROFS;
}

static void TraceFlushF(void *ctx) {   // on TraceQueue
    static char buf[kTraceBufBytes];
    atomic_store(&gTrace.flushScheduled, 0);   // first: an op recorded while this writes schedules the next write
    TracePickF(NULL);
    int slot = atomic_load_explicit(&gTrace.pathSlot, memory_order_acquire);
    if (slot < 0) {
        return;
    }
    int err = TraceWriteTo(buf, sizeof buf, gTrace.paths[slot].tmp, gTrace.paths[slot].path);
    if (!err || gTrace.repicked) {
        return;
    }

    // not a directory gone, replaced or unwritable: another pick wouldn't help
    if (!TraceDirUnusable(err)) {
        static int noted;
        if (!noted++) {
            LOG(@"trace: writing %s failed (%s); not re-picking (only a missing or read-only directory is); later "
                @"writes retry there",
                gTrace.paths[slot].path, strerror(err));
        }
        return;
    }

    // The directory went away or became unwritable: pick again, once (into the re-pick slot), and write there.
    // SIMCAM_OFF_TRACE_REPICK=1 gives up silently.
    gTrace.repicked = 1;
    if (!TraceRepickOn()) {
        return;
    }
    int dirIndex = TracePickInto(kTraceSlotRepick);

    if (dirIndex < 0) {
        LOG(@"trace: writing %s failed (%s): its directory is gone or read-only and no other one takes a file; the "
            @"trace stops here",
            gTrace.paths[slot].path, strerror(err));
        return;
    }

    int repickErr =
        TraceWriteTo(buf, sizeof buf, gTrace.paths[kTraceSlotRepick].tmp, gTrace.paths[kTraceSlotRepick].path);
    LOG(@"trace: writing %s failed (%s): its directory is gone or read-only; re-picked once, the trace is now %s "
        @"(%s)%s%s",
        gTrace.paths[slot].path, strerror(err), gTrace.paths[kTraceSlotRepick].path,
        dirIndex == kStateDirState ? "state dir, made again" : "container-tmp fallback",
        repickErr ? "; that write failed too: " : "", repickErr ? strerror(repickErr) : "");
}

static void TraceKick(void) {
    if (atomic_exchange(&gTrace.flushScheduled, 1)) {
        return;
    }
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, kTraceFlushMs * NSEC_PER_MSEC), TraceQueue(), NULL, TraceFlushF);
}

static void TraceRecord(int op, int64_t settingsID, int64_t arg) {
    if (!atomic_load_explicit(&gTrace.on, memory_order_acquire)) {
        return;
    }
    TracePut(op, settingsID, arg, mach_absolute_time(), TraceWallNs());
    TraceKick();
}

// The realtime of an earlier (or later) mach instant: now's realtime moved by the mach difference. Works with the
// trace off.
static uint64_t TraceWallAt(uint64_t mach) {
    mach_timebase_info_data_t timebase = gTrace.timebase;
    if (!timebase.denom) {
        mach_timebase_info(&timebase);
    }
    if (!timebase.denom) {
        timebase.numer = 1;
        timebase.denom = 1;
    }
    uint64_t now = mach_absolute_time();
    uint64_t wall = TraceWallNs();
    if (mach <= now) {
        return wall - (now - mach) * timebase.numer / timebase.denom;
    }
    return wall + (mach - now) * timebase.numer / timebase.denom;
}

// An op at an earlier mach instant whose realtime the caller computed (TraceWallAt), so the caller can use the same
// value elsewhere (the photo's EXIF time).
static void TraceRecordAtWall(int op, int64_t settingsID, int64_t arg, uint64_t mach, uint64_t wall) {
    if (!atomic_load_explicit(&gTrace.on, memory_order_acquire)) {
        return;
    }
    TracePut(op, settingsID, arg, mach, wall);
    TraceKick();
}

// an op at an earlier mach instant (the photo's timestamp): its realtime is moved back by the same amount
static void TraceRecordAt(int op, int64_t settingsID, int64_t arg, uint64_t mach) {
    if (!atomic_load_explicit(&gTrace.on, memory_order_acquire)) {
        return;
    }
    TraceRecordAtWall(op, settingsID, arg, mach, TraceWallAt(mach));
}

static int64_t TraceSettingsID(id settings) {
    static SEL sel;
    if (!sel) {
        sel = sel_registerName("uniqueID");
    }
    if (!settings || ![settings respondsToSelector:sel]) {
        return -1;
    }
    return ((int64_t (*)(id, SEL))objc_msgSend)(settings, sel);
}

// Called by ActivationRun right after ARMED (on the activation thread): recording starts, the activate op is recorded
// and the exit flush is registered.
static void TraceArm(void) {
    const char *traceSetting = getenv(ENV_TRACE);
    if (traceSetting && traceSetting[0] == '0') {
        LOG(@"trace: off (SIMCAM_TRACE=0)");
        return;
    }
    mach_timebase_info(&gTrace.timebase);
    if (!gTrace.timebase.denom) {
        gTrace.timebase.numer = 1;
        gTrace.timebase.denom = 1;
    }
    gOffTraceCrashClaim = !TraceCrashClaimOn();
    if (gOffTraceCrashClaim) {
        LOG(@"SIMCAM_OFF_TRACE_CRASH_CLAIM: every crashing thread writes the trace, from one buffer (L70's mutant)");
    }
    atomic_store_explicit(&gTrace.on, 1, memory_order_release);
    TraceRecord(kTraceOpActivate, -1, 0);
    atexit_b(^{
        dispatch_sync_f(TraceQueue(), NULL, TraceFlushF);
    });
}

// Called by ActivationRun after it released its waiters: picks the trace file's directory, so the crash handler can
// write from the first op on. It runs on the activation queue (nobody waits for it; inline in the test modes),
// synchronously on the trace queue, where every pick happens. SIMCAM_OFF_TRACE_DIR_AT_ARM=1 leaves the pick to the
// first flush.
static void TraceArmFile(void) {
    if (!atomic_load_explicit(&gTrace.on, memory_order_acquire)) {
        return;
    }
    if (!TraceDirAtArmOn()) {
        LOG(@"SIMCAM_OFF_TRACE_DIR_AT_ARM: the trace directory is picked at the first flush");
        return;
    }
    dispatch_sync_f(TraceQueue(), NULL, TracePickF);
}

// The crash handler's part (async-signal-safe): the crash op, then the whole ring, from its own buffer and temp file.
// The first thread here writes; one that comes while it writes waits for that write (up to about 500 ms), then returns
// to the chain. The flag goes back to 0 after the write, so a later crash (after a fault the chain recovered from)
// writes again.
static void TraceCrash(int sig) {
    if (!atomic_load_explicit(&gTrace.on, memory_order_acquire)) {
        return;
    }
    TracePut(kTraceOpCrash, -1, sig, mach_absolute_time(), TraceWallNs());
    int saved = errno;   // errno as the chained handler expects it
    int idle = 0;

    // SIMCAM_OFF_TRACE_CRASH_CLAIM: no CAS. Format, wait 20 ms, write: two threads share the buffer and the temp file.
    if (gOffTraceCrashClaim) {
        int slot = atomic_load_explicit(&gTrace.pathSlot, memory_order_acquire);
        if (slot >= 0) {
            int len = TraceFormat(gTrace.crashBuffer, sizeof gTrace.crashBuffer);
            struct timespec delay = {0, kTraceUnclaimedWriteDelayNs};
            nanosleep(&delay, NULL);
            (void)TraceWriteBuf(gTrace.crashBuffer, len, gTrace.paths[slot].sigTmp, gTrace.paths[slot].path);
        }
        errno = saved;
        return;
    }

    if (!atomic_compare_exchange_strong(&gTrace.crashWriting, &idle, 1)) {
        struct timespec slice = {0, kTraceCrashWaitSliceNs};
        for (int i = 0; i < kTraceCrashWaitSlices && atomic_load(&gTrace.crashWriting) == 1; i++) {
            nanosleep(&slice, NULL);
        }
        errno = saved;
        return;
    }

    int slot = atomic_load_explicit(&gTrace.pathSlot, memory_order_acquire);
    if (slot >= 0) {
        (void)TraceWriteTo(gTrace.crashBuffer, sizeof gTrace.crashBuffer, gTrace.paths[slot].sigTmp,
                           gTrace.paths[slot].path);
    }
    atomic_store(&gTrace.crashWriting, 0);   // a later crash that reaches this handler writes again
    errno = saved;
}

static void TraceLatch(int featureIndex) { TraceRecord(kTraceOpLatch, -1, featureIndex); }
