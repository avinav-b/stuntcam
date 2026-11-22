// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the op trace's internal interface. It declares the ops other fragments
// record, the trace's state (gTrace), and the functions the sentinel calls before core/trace.inc.
//
// Who owns what:
//   The ring is written from any thread with atomics only (TraceRecord). A per-slot sequence number makes a reader's
//   copy a seqlock, so the reader is async-signal-safe.
//   The trace file's paths are picked on the trace queue and published (release) through pathSlot; a slot is never
//   rewritten once published, so the crash handler can read the current one at any time.
//   repicked is touched only on the trace queue. crashWriting is the crash handler's claim on crashBuffer.
#ifndef SIMCAM_TRACE_H
#define SIMCAM_TRACE_H
#include <mach/mach_time.h>

// The ops, in the order of kTraceOpNames (core/trace.inc), which spells them as the file and the CLI show them.
enum {
    kTraceOpNone = 0,
    kTraceOpActivate,
    kTraceOpSessionStart,
    kTraceOpSessionStop,
    kTraceOpShutter,
    kTraceOpPrepare,
    kTraceOpSlot12,
    kTraceOpWillCapture,
    kTraceOpDidCapture,
    kTraceOpEncodeStart,
    kTraceOpEncoded,
    kTraceOpTimestamp,
    kTraceOpStillComplete,
    kTraceOpDidFinish,
    kTraceOpMovieStart,
    kTraceOpMovieStop,
    kTraceOpLatch,
    kTraceOpCrash,
    kTraceOpCount
};

#define kTraceRingSize 64
#define kTraceBufBytes 12288
#define kTracePathBytes 1200   // a file in the trace directory
#define kTracePathSlots 2      // gTrace.paths: the first pick, and the one re-pick

typedef struct {
    _Atomic uint64_t seq;   // 0 while being written, index + 1 once done
    _Atomic uint64_t wall;
    _Atomic uint64_t mach;
    _Atomic int64_t settingsID;
    _Atomic int64_t arg;
    _Atomic int op;
} TraceEntry;

// The trace file's paths in one directory.
typedef struct {
    char dir[kSentinelPathBytes];
    char path[kTracePathBytes];
    char tmp[kTracePathBytes];
    char sigTmp[kTracePathBytes];
} TracePaths;

struct Trace {
    // The ring (any thread, atomics only).
    TraceEntry ring[kTraceRingSize];
    _Atomic uint64_t recorded;     // ops recorded (the next op's index)
    _Atomic int on;                // set (release) once the camera armed and SIMCAM_TRACE isn't 0
    _Atomic int flushScheduled;    // a flush is scheduled

    // slot 0: the first pick; slot 1: the one re-pick (never rewritten once published)
    TracePaths paths[kTracePathSlots];
    // the slot in use (release once its paths are set; -1 before the first pick): the handler and the flushes write
    // there
    _Atomic int pathSlot;
    int repicked;   // the one re-pick is spent (trace queue only)

    // The crash handler's claim on the write and its own buffer; the timebase every writer prints.
    _Atomic int crashWriting;   // 0 idle, 1 a thread is writing (back to 0 after it)
    mach_timebase_info_data_t timebase;
    char crashBuffer[kTraceBufBytes];   // never shared with the queue's or the exit flush's
};

// ---- functions other fragments call

static void TraceCrash(int sig);            // core/crash_handler.inc (async-signal-safe)
static void TraceLatch(int featureIndex);   // core/safety.inc

// Recording an op (providers/pump.inc, providers/photo.inc, client/photo.inc).
static void TraceRecord(int op, int64_t settingsID, int64_t arg);
static void TraceRecordAt(int op, int64_t settingsID, int64_t arg, uint64_t mach);
static void TraceRecordAtWall(int op, int64_t settingsID, int64_t arg, uint64_t mach, uint64_t wall);
static uint64_t TraceWallAt(uint64_t mach);
static int64_t TraceSettingsID(id settings);

// Arming the trace at activation (core/lazy.inc).
static void TraceArm(void);
static void TraceArmFile(void);

#endif
