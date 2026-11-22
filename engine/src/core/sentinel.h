// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the crash sentinel's internal interface. core/safety.inc includes it
// first. It defines the sentinel's state (gSentinel), the crash handlers' state (gCrashHandler) and the marker test
// switches, and declares the features, the latches, the sizes the sentinel's fragments share and the functions other
// fragments call.
//
// Who owns what:
//   Load (SentinelLoadInit, before any other engine thread runs) sets the mode, the build, the bundle, the scale, the
//   state paths and what the load-time stat()s found (atLoad). Nothing writes them after load.
//   PREPARE's sentinel step (core/sentinel_activate.inc, on the activation queue) opens the events file and sets
//   gSentinel.events, then publishes its path and the activation number (release) for the record queue. It also
//   fills gCrashHandler.motionMarker and the image ranges, before it installs the minimal handler.
//   The op slots are guarded by gSentinel.ops.lock; the signal handler reads them without the lock. The watchdog
//   thread advances the active clock and counts hangs. live, traced, hangs and latchedNow are atomics.
//   The signal handlers read gCrashHandler and gSentinel only through async-signal-safe loads.
#ifndef SIMCAM_SENTINEL_H
#define SIMCAM_SENTINEL_H

// Buffer sizes for the sentinel's paths: a state directory, a file in one, a temp file in one, and a directory entry's
// name joined to its directory.
#define kSentinelDirPathBytes 1024
#define kSentinelPathBytes 1100
#define kSentinelTempPathBytes 1200
#define kSentinelEntryPathBytes 1400
#define kSentinelMaxOps 64
#define kSentinelMaxRelatedImages 12   // ranges of the related system images (AVFoundation, CoreMedia, ...)
#define kSentinelSignalSlots 32        // the saved actions, indexed by signal number (Darwin's signals are 1 to 31)

// The features a crash or hang counts against: indexes into kFeatureNames. `activate` (a timeout, crash or exception
// during activation) is a feature like the others: two strikes latch the camera. `motion` (a crash in the CoreMotion
// stand-in, attributed through the thread-specific feature key) latches CoreMotion only: it never closes the camera,
// it only writes latched-motion-<build>.
enum {
    kFeatureSession = 0,
    kFeaturePhoto,
    kFeatureMovie,
    kFeatureProvider,
    kFeatureEngine,
    kFeatureActivate,
    kFeatureMotion,
    kFeatureCount
};

// The features' names, as the events file, the markers and the CLI spell them.
static const char *kFeatureNames[kFeatureCount] = {
    [kFeatureSession] = "session",
    [kFeaturePhoto] = "photo",
    [kFeatureMovie] = "movie",
    [kFeatureProvider] = "provider",
    [kFeatureEngine] = "engine",
    [kFeatureActivate] = "activate",
    [kFeatureMotion] = "motion",
};

enum { kSentinelModeOff = 0, kSentinelModeOn = 1, kSentinelModeClear = 2 };   // SIMCAM_SENTINEL=0, unset, =clear
enum { kStateDirState = 0, kStateDirTmp, kStateDirCount };   // the state dir, and the container-tmp fallback

// One op slot (SentinelOpBegin, SentinelOpEnd).
typedef struct {
    _Atomic int used;      // the slot holds an inflight op
    const char *feature;   // what a hang or crash in this op counts against (kFeatureNames)
    uint64_t deadline;     // on the active clock (gSentinel.ops.activeClockNs)
    uint64_t timeout;      // the op's timeout (ns, sanitizer-scaled)
    int expired;           // the watchdog has recorded this op's hang
} SentinelOp;

// An image's __TEXT range.
typedef struct {
    uintptr_t lo;   // the first address
    uintptr_t hi;   // one past the last address
} SentinelImageRange;

// PREPARE's latch decision (core/sentinel_activate.inc), which the marker update (core/markers.inc) writes out.
#define kSentinelLatchFeaturesCap 200   // the latched features' names (the latched-<build> marker's content)
#define kSentinelLatchCountsCap 100     // their strike counts, for the latch line
typedef struct {
    BOOL camera;                                // a camera-closing feature latched: this launch has no camera
    char features[kSentinelLatchFeaturesCap];   // those features' names, space-separated
    char counts[kSentinelLatchCountsCap];       // their strike counts, comma-separated
    long until;                                 // when the last of those latches ends (epoch s)
    BOOL coreMotion;                            // motion latched: CoreMotion stays stock from the next launch (the
                                                // name keeps the public export's leak check quiet)
    long motionUntil;                           // when the motion latch ends (epoch s)
} SentinelLatch;

struct Sentinel {
    // Set at load.
    int mode;          // kSentinelModeOff, kSentinelModeOn or kSentinelModeClear (SIMCAM_SENTINEL)
    char build[32];    // the name of the content-hash directory the dylib is in ("-" until load)
    char bundle[256];  // the app's bundle id ("unknown" if it has none)
    double scale;      // 10 with TSan or ASan loaded, else 1: op timeouts are scaled by it
    _Atomic int live;     // the sentinel is live (START): ops are recorded from here on
    _Atomic int traced;   // a debugger is attached (P_TRACED): crashes and hangs are recorded but never counted

    // The two candidate state directories (kStateDir*) and the marker paths in each, set at load.
    struct {
        char eventsDirs[kStateDirCount][kSentinelDirPathBytes];      // <state>/<bundle>
        char latchMarker[kStateDirCount][kSentinelPathBytes];        // <dir>/latched-<build>
        char preflightFail[kStateDirCount][kSentinelPathBytes];      // <state>/preflight-fail-<build>
        char motionMarker[kStateDirCount][kSentinelPathBytes];       // <dir>/latched-motion-<build>
    } paths;

    // What the load-time stat()s found.
    struct {
        char latchMarker[kSentinelPathBytes];         // the valid latched-<build> marker ("" = none)
        char motionStockMarker[kSentinelPathBytes];   // the marker that made CoreMotion stock ("" = the stand-in)
        time_t motionStockUntil;                      // its until (st_mtime) at that load-time stat
        ino_t motionStockInode;   // and its inode: the stock line reads the features only from this one
        // what the stock line says if the marker is gone by then: "camera", "motion" or "preflight failed"
        const char *motionStockKind;
        BOOL cleared;   // SIMCAM_SENTINEL=clear wrote its clear line at load
    } atLoad;

    // This activation's events file, set by PREPARE's sentinel step.
    struct {
        int fd;                 // the events file's descriptor (O_APPEND); -1 when the sentinel is off
        int activationNumber;   // this activation's number
        char path[kSentinelDirPathBytes];
        // which paths.eventsDirs entry events.log opened in (-1 before). This build's markers go only there.
        int dirIndex;
        // Published by the sentinel step (release) for the record queue.
        _Atomic(const char *) pathPublished;
        _Atomic int activationNumberPublished;
    } events;

    // The op slots, their lock and the active clock (the watchdog advances it).
    struct {
        SentinelOp slots[kSentinelMaxOps];
        os_unfair_lock lock;               // zero is OS_UNFAIR_LOCK_INIT
        _Atomic uint64_t activeClockNs;
    } ops;

    _Atomic int hangs[kFeatureCount];        // the hangs of each feature in this process
    _Atomic int latchedNow[kFeatureCount];   // the features latched in this process (SentinelLatched)

    // The thread-specific feature key (created at load unless SIMCAM_OFF_MOTION_KEY=1).
    struct {
        pthread_key_t key;
        _Atomic int on;   // the key exists
    } featureKey;

    // When a latched session last got a runtime error (CLOCK_MONOTONIC ns; SentinelSessionErrorAllowed).
    _Atomic uint64_t sessionErrorAtNs;
    char latchMessage[600];   // the sentinel step's latch line, printed through the record queue
};

struct CrashHandler {
    // The image ranges a crash is attributed against, cached at PREPARE (never computed in a handler).
    SentinelImageRange selfRange;
    SentinelImageRange relatedRanges[kSentinelMaxRelatedImages];
    _Atomic int relatedCount;

    struct sigaction chainedByFull[kSentinelSignalSlots];   // what the full handler chains to
    // what the minimal (PREPARE) handler chains to: the handler that was there before activation
    struct sigaction chainedByMinimal[kSentinelSignalSlots];
    _Atomic int minimalOn;              // the minimal handler is installed
    _Atomic int prepareCrashRecorded;   // the minimal handler recorded its one `crash activate`

    // What the full handler needs to write the latched-motion marker itself (set by the sentinel step).
    struct {
        int priorStrikes;         // this build's motion strikes in the window before this launch
        long newestStrikeEpoch;   // the newest one's epoch: the marker's until is this plus the window
        char path[kSentinelPathBytes];
        char tempPath[kSentinelTempPathBytes];
    } motionMarker;
};

// ---- the sentinel's state

// The sentinel's state and the crash handlers' (struct Sentinel and struct CrashHandler say who owns each field).
static struct Sentinel gSentinel = {
    .mode = kSentinelModeOn,
    .build = "-",
    .bundle = "unknown",
    .scale = 1,
    .events.fd = -1,
    .events.dirIndex = -1,
};
static struct CrashHandler gCrashHandler;

// The marker switches, parsed once at load by core/markers.inc's SentinelParseTestEnv.
static BOOL gOffHandlerMotionMarker;   // SIMCAM_OFF_HANDLER_MOTION_MARKER
static BOOL gOffMarkerRename;          // SIMCAM_OFF_MARKER_RENAME
static long gTestSlowMarkerMs;         // SIMCAM_TEST_SLOW_MARKER_MS (pf-prune-race)
static long gTestPrunePauseMs;         // SIMCAM_TEST_PRUNE_PAUSE_MS (pf-prune-kept)

// ---- latches

// A latched feature fails loudly, never silently and never through stock: it error-completes with
// kSentinelLatchStatus, which apps get as AVFoundation's generic -11800 with this OSStatus underneath.
//   photo    every photo not yet encoded fails
//   movie    active recordings stop with it, and new ones fail at once
//   session  running sessions get a runtime error and stop; a latched startRunning gets at most one runtime error per
//            kSentinelSessionErrorIntervalNs (SentinelSessionErrorAllowed), and in between it is stopped without
//            one
// Before any session exists, a latch means stock: the activation-time latch keeps the gate closed.
// kSentinelLatchStatus is also the default status of the control file's photo-error and movie-error test events.
#define kSentinelLatchStatus (-16800)
// In this process, kSentinelStrikesToLatch hangs of one feature latch it (SentinelWatchdogHang). At activation,
// kSentinelStrikesToLatch crash or hang events of one feature with this build, within the last
// kSentinelWindowActivations activations and kSentinelWindowSeconds, latch it: the gate stays closed (no camera) and
// one stderr line says why (core/sentinel_activate.inc).
#define kSentinelStrikesToLatch 2
#define kSentinelWindowSeconds 86400    // 24 h: the latch window, and how long a marker is valid
#define kSentinelWindowActivations 20   // the latch window's other bound: the last 20 activations
// the window as the latch lines print it: 24 h
#define kSentinelWindowHours (kSentinelWindowSeconds / 3600)
#define kSentinelEventLineBytes 256     // one events-file line as SentinelWriteEvent writes it

// A latched session gets at most one runtime error per 5 s, whoever posts it, so an app that restarts its session on
// every runtime error can't spin (SentinelSessionErrorAllowed). SessInjectRuntimeError(kSentinelLatchAuto), the
// session latch's stop, decides on the pump queue, once it finds a running session, whether to carry the error: a
// latch with no session running doesn't use up the window.
#define kSentinelLatchAuto INT_MIN
#define kSentinelSessionErrorIntervalNs (5 * NSEC_PER_SEC)   // at most one latch runtime error per this interval

// ---- the events file and the feature key

// The fields of an events-file line split on spaces, as the sentinel step reads them back. A line needs
// kEventMinFields of them (the detail may be missing).
enum {
    kEventFieldEpoch = 0,
    kEventFieldActivation,
    kEventFieldKind,
    kEventFieldFeature,
    kEventFieldBuild,
    kEventFieldDetail
};
#define kEventMinFields 5

// The thread-specific feature key's value inside a motion entry point (core/safety.inc explains the key).
static const char kMotionFeatureKey[] = "motion";

// `feature` (NULL: none) is this thread's feature until the enclosing scope ends; the value before it is restored on
// every exit path.
#define SENTINEL_FEATURE_SCOPE(feature)                                                   \
    void *_scFeatureSaved __attribute__((cleanup(SentinelFeatureLeaveCleanup), unused)) = \
        SentinelFeatureEnter(feature)
// a motion entry point: `motion` on this thread until the enclosing scope ends
#define SENTINEL_MOTION_SCOPE SENTINEL_FEATURE_SCOPE(kMotionFeatureKey)

// PREPARE's sentinel step's results (SentinelPrepare).
enum { kSentinelPrepareOk = 0, kSentinelPrepareOff, kSentinelPrepareLatched };

// ---- functions other fragments call

// The features, ops and latches (core/safety.inc), for the other sentinel fragments, the providers, core/control.inc
// and core/lazy.inc.
static int SentinelFeatureIndex(const char *feature);
static BOOL SentinelClosesCamera(int featureIndex);
static BOOL SentinelSessionErrorAllowed(void);
static int SentinelOpBegin(const char *feature, double timeoutSec);
static void SentinelOpEnd(int slot);
static BOOL SentinelLatched(const char *feature);
static BOOL ProcessIsTraced(void);
static void *SentinelWatchdog(void *arg);

// The feature key (core/safety.inc), for client/motion.inc and core/lazy.inc.
static void SentinelFeatureKeyInit(void);
static inline void *SentinelFeatureEnter(const char *feature);
static inline void SentinelFeatureLeave(void *saved);
static inline void SentinelFeatureLeaveCleanup(void **savedSlot);
static inline void *SentinelFeatureClearUnless(BOOL keep);
static inline void SentinelFeatureRestoreUnless(BOOL keep, void *saved);

// The async-signal-safe event writer (core/safety.inc), for core/crash_handler.inc, core/trace.inc and the rest.
static int SentinelPutString(char *buf, int len, int cap, const char *text);
static int SentinelPutUnsigned(char *buf, int len, int cap, unsigned long value);
static int SentinelFormatEvent(char *buf, int cap, long act, const char *kind, const char *feature, const char *detail,
                               long num);
static void SentinelWriteEvent(const char *kind, const char *feature, const char *detail, long num);

// The crash handlers (core/crash_handler.inc), for core/sentinel_activate.inc and core/lazy.inc.
static void SentinelCacheRanges(void);
static int SentinelRestoreConditional(void);
static void SentinelInstallMinimal(void);
static void SentinelInstallFull(void);

// The load-time state and the markers (core/markers.inc).
static void SentinelLoadInit(void);
static BOOL SentinelLatchMarkerValidAtLoad(void);
static BOOL SentinelMotionStockAtLoad(void);
static void SentinelClearAtLoad(void);
static void SentinelPreflightMarker(BOOL passed, const char *what);
static void SentinelCountStrikes(NSArray *past, NSString *build, long now, int act, int counts[kFeatureCount],
                                 long until[kFeatureCount], long *newest);
static void SentinelMarkersUpdate(NSArray *past, long now, const SentinelLatch *latch);
static int SentinelMkdirP(const char *path);

// The events file (core/events_file.inc) and the activation steps (core/sentinel_activate.inc).
static void SentinelTrim(void);
static int SentinelRecordEvent(const char *kind, const char *feature, const char *detail);
static int SentinelPrepare(void);
static void SentinelStart(void);

#endif
