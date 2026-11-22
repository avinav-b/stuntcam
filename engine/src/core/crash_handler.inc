// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/safety.inc): the sentinel's crash
// handlers: the image ranges a crash is attributed against, the minimal signal handler (installed by PREPARE's
// sentinel step) and the full one (installed by SWAP), and their chaining.
//
// Provides: SentinelInstallMinimal (core/sentinel_activate.inc), SentinelInstallFull (core/lazy.inc)
//           SentinelCacheRanges, SentinelRestoreConditional (both)
// Owns:     gCrashHandler, the crash handlers' state (core/sentinel_activate.inc fills the motion marker's fields), and
//           the marker switches gOffHandlerMotionMarker, gOffMarkerRename, gTestSlowMarkerMs, gTestPrunePauseMs (set
//           by core/markers.inc), all defined in core/sentinel.h
// Uses:     core/act.inc, core/preflight.inc, core/safety.inc, core/trace.inc
#include "activation.h"
#include "sentinel.h"
#include "trace.h"
#include "preflight.h"

// ---- crash sentinel ----
// Each is a valid signal a handler may catch, so sigaction can't fail for them: its result is ignored ((void)) below.
static const int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
#define kCrashSignalCount ((int)(sizeof kCrashSignals / sizeof *kCrashSignals))

// the image's __TEXT range (read at PREPARE, never from a handler)
static BOOL SentinelTextRange(const struct mach_header *header, intptr_t slide, SentinelImageRange *range) {
    __block BOOL found = NO;
    MachOForEachCommand((const struct mach_header_64 *)header, ^BOOL(const struct load_command *command) {
        if (command->cmd == LC_SEGMENT_64 &&
            !strcmp(((const struct segment_command_64 *)command)->segname, "__TEXT")) {
            const struct segment_command_64 *segment = (const void *)command;
            range->lo = segment->vmaddr + slide;
            range->hi = range->lo + segment->vmsize;
            found = YES;
        }
        return found;
    });
    return found;
}

// Called before the minimal handler goes in (the sentinel step) and again after the lookups' dlopens. A range is
// written before the count that covers it is published, so a handler never reads a half-written range. Ranges already
// cached stay as they are.
static void SentinelCacheRanges(void) {
    Dl_info selfInfo;
    const struct mach_header *ownHeader =
        dladdr((const void *)SentinelCacheRanges, &selfInfo) ? selfInfo.dli_fbase : NULL;
    static const char *relatedSuffixes[] = {"/AVFCapture", "/AVFoundation", "/CMCapture", "/CoreMedia",
                                            "/CoreVideo",  "/IOSurface",    "/ImageIO",   "/VideoToolbox"};
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header *header = _dyld_get_image_header(i);
        const char *imageName = _dyld_get_image_name(i);
        if (!header || !imageName) {
            continue;
        }
        size_t nameLen = strlen(imageName);
        if (header == ownHeader) {
            if (!gCrashHandler.selfRange.hi) {
                SentinelTextRange(header, _dyld_get_image_vmaddr_slide(i), &gCrashHandler.selfRange);
            }
            continue;
        }
        for (unsigned j = 0; j < sizeof relatedSuffixes / sizeof *relatedSuffixes; j++) {
            size_t suffixLen = strlen(relatedSuffixes[j]);
            if (nameLen < suffixLen || strcmp(imageName + nameLen - suffixLen, relatedSuffixes[j])) {
                continue;
            }
            int count = atomic_load(&gCrashHandler.relatedCount);
            SentinelImageRange range;
            if (count >= kSentinelMaxRelatedImages ||
                !SentinelTextRange(header, _dyld_get_image_vmaddr_slide(i), &range)) {
                break;
            }
            BOOL alreadyCached = NO;
            for (int k = 0; k < count; k++) {
                if (gCrashHandler.relatedRanges[k].lo == range.lo) {
                    alreadyCached = YES;
                }
            }
            if (!alreadyCached) {
                gCrashHandler.relatedRanges[count] = range;
                atomic_store(&gCrashHandler.relatedCount, count + 1);
            }
            break;
        }
    }
}

static uintptr_t SentinelSignalPC(void *ctx) {
    ucontext_t *context = ctx;
    if (!context || !context->uc_mcontext) {
        return 0;
    }
#if defined(__arm64__)
  #ifdef __darwin_arm_thread_state64_get_pc
    return (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
  #else
    return (uintptr_t)context->uc_mcontext->__ss.__pc;
  #endif
#elif defined(__x86_64__)
    return (uintptr_t)context->uc_mcontext->__ss.__rip;
#else
    return 0;
#endif
}

static BOOL SentinelInRelatedImage(uintptr_t pc) {
    int count = atomic_load(&gCrashHandler.relatedCount);
    for (int i = 0; i < count; i++) {
        if (pc >= gCrashHandler.relatedRanges[i].lo && pc < gCrashHandler.relatedRanges[i].hi) {
            return YES;
        }
    }
    return NO;
}

// Chain to the previous handler if there was one, else take the default action.
static void SentinelChain(int sig, siginfo_t *info, void *ctx, struct sigaction *previous) {
    if (previous && (previous->sa_flags & SA_SIGINFO) && previous->sa_sigaction) {
        (void)sigaction(sig, previous, NULL);
        previous->sa_sigaction(sig, info, ctx);
        return;
    }

    if (previous && !(previous->sa_flags & SA_SIGINFO) && previous->sa_handler != SIG_DFL &&
        previous->sa_handler != SIG_IGN) {
        (void)sigaction(sig, previous, NULL);
        previous->sa_handler(sig);
        return;
    }

    signal(sig, SIG_DFL);

    // For a fault the kernel generated (SEGV, BUS, ILL, FPE or TRAP from an instruction) the handler returns, so the
    // instruction runs again under SIG_DFL and the crash report shows the real exception (EXC_BAD_ACCESS ...) on the
    // crashing thread. raise() here would make it EXC_CRASH (SIGSEGV) blamed on thread 0, with the crashing thread's
    // stack empty. Sent signals (abort, kill) are re-sent.
    if (sig != SIGABRT && info && info->si_code != SI_USER && info->si_code != SI_QUEUE) {
        return;
    }
    raise(sig);
}

// The full handler writes the latched-motion marker itself on the second `motion` strike (async-signal-safe: open,
// write, futimens), from what the sentinel step left in gCrashHandler.motionMarker: motion is decided at load, so a
// marker written by the next launch would come one launch late. It writes a temp file, sets its until, then renames it
// into place, so the marker never exists with mtime = now (which another process's prune would read as expired).
// SIMCAM_OFF_HANDLER_MOTION_MARKER=1 skips the write; SIMCAM_OFF_MARKER_RENAME=1 writes in place.
#define kSentinelMotionMarkerTextLen 7   // strlen("motion\n"), the marker's content

static void SentinelMotionMarkerFromHandler(void) {   // async-signal-safe only
    if (gCrashHandler.motionMarker.priorStrikes < 1 || !gCrashHandler.motionMarker.path[0] || gOffHandlerMotionMarker) {
        return;
    }
    BOOL inPlace = gOffMarkerRename || !gCrashHandler.motionMarker.tempPath[0];
    const char *writePath = inPlace ? gCrashHandler.motionMarker.path : gCrashHandler.motionMarker.tempPath;
    int fd = open(writePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }
    // In a signal handler nothing safe can be done about a failed write. The marker latches by its name and mtime (set
    // below). Its content is what `stuntcam sentinel` and `status` show (the CLI derives the latch's effect from it),
    // so a short write only degrades that report.
    (void)write(fd, "motion\n", kSentinelMotionMarkerTextLen);
    struct timespec ts[2] = {{gCrashHandler.motionMarker.newestStrikeEpoch + kSentinelWindowSeconds, 0},
                              {gCrashHandler.motionMarker.newestStrikeEpoch + kSentinelWindowSeconds, 0}};
    // a marker whose until couldn't be set reads as expired: the latch is lost early, never held too long
    (void)futimens(fd, ts);
    close(fd);
    if (writePath != gCrashHandler.motionMarker.path && rename(writePath, gCrashHandler.motionMarker.path) != 0) {
        unlink(writePath);
    }
}

// The full handler, chained for SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP and SIGFPE, attributes a crash to a feature,
// async-signal-safely, against the image ranges cached at activation. A PC in our image is the inflight op's feature,
// else "provider". With an op inflight, a PC in a related image (AVFCapture, AVFoundation, CMCapture, CoreMedia,
// CoreVideo, IOSurface, ImageIO, VideoToolbox) is the op's feature too. Anything else is "crash-other", never counted.
// The handler appends one line to the events file, then chains to the previous handler.
static void SentinelOnSignal(int sig, siginfo_t *info, void *ctx) {   // async-signal-safe only
    uintptr_t pc = SentinelSignalPC(ctx);
    BOOL inOwnImage = pc >= gCrashHandler.selfRange.lo && pc < gCrashHandler.selfRange.hi;

    // The feature key comes first: a fault in our image on a thread inside a motion entry point is `motion`, not
    // `provider` or an inflight op's feature, and it never closes the camera. An app handler runs with the key cleared
    // (and has an app PC).
    const void *threadFeature = atomic_load_explicit(&gSentinel.featureKey.on, memory_order_relaxed)
                                    ? pthread_getspecific(gSentinel.featureKey.key)
                                    : NULL;
    const char *op = NULL;

    for (int i = 0; i < kSentinelMaxOps; i++) {
        if (atomic_load(&gSentinel.ops.slots[i].used)) {
            op = gSentinel.ops.slots[i].feature;
            break;
        }
    }

    const char *feature = NULL;
    if (inOwnImage && threadFeature == (const void *)kMotionFeatureKey) {
        feature = kMotionFeatureKey;
    } else if (inOwnImage) {
        feature = op ? op : "provider";
    } else if (op && SentinelInRelatedImage(pc)) {
        feature = op;
    }

    // under a debugger a crash is never counted: it may be the user stopping in lldb
    BOOL traced = atomic_load(&gSentinel.traced);
    const char *kind;
    if (!feature) {
        kind = "crash-other";
    } else if (traced) {
        kind = "crash-traced";
    } else {
        kind = "crash";
    }
    SentinelWriteEvent(kind, feature, "signal=", sig);
    if (feature == kMotionFeatureKey && !traced) {
        SentinelMotionMarkerFromHandler();
    }
    TraceCrash(sig);   // the op ring to trace-<pid>.txt (async-signal-safe)
    SentinelChain(sig, info, ctx, (sig > 0 && sig < kSentinelSignalSlots) ? &gCrashHandler.chainedByFull[sig] : NULL);
}

// The minimal handler, installed by PREPARE's sentinel step. While PREPARE runs, PREPARE counts as an inflight
// `activate` op: a fault on any thread whose PC is in our image or a related image is `crash activate`. Any other
// fault, or any fault once gActivation.preparing is clear, only chains to the handler that was there before activation.

static void SentinelOnSignalPrepare(int sig, siginfo_t *info, void *ctx) {   // async-signal-safe only
    if (atomic_load(&gActivation.preparing)) {
        uintptr_t pc = SentinelSignalPC(ctx);
        BOOL inCameraCode =
            (pc >= gCrashHandler.selfRange.lo && pc < gCrashHandler.selfRange.hi) || SentinelInRelatedImage(pc);
        if (inCameraCode && !atomic_exchange(&gCrashHandler.prepareCrashRecorded, 1)) {
            SentinelWriteEvent(atomic_load(&gSentinel.traced) ? "crash-traced" : "crash", "activate", "signal=", sig);
        }
    }
    SentinelChain(sig, info, ctx,
                  (sig > 0 && sig < kSentinelSignalSlots) ? &gCrashHandler.chainedByMinimal[sig] : NULL);
}

static BOOL SentinelIsHandler(const struct sigaction *action, void *handler) {
    return (action->sa_flags & SA_SIGINFO) && (void *)action->sa_sigaction == handler;
}

static BOOL SentinelSameAction(const struct sigaction *first, const struct sigaction *second) {
    if (first->sa_flags != second->sa_flags) {
        return NO;
    }
    if (first->sa_flags & SA_SIGINFO) {
        return (void *)first->sa_sigaction == (void *)second->sa_sigaction;
    }
    return first->sa_handler == second->sa_handler;
}

// The install order: read the current handler first, then install ours. If what we replaced differs from what was read
// (someone installed a handler in between), chain to that instead. The minimal handler never runs with
// gCrashHandler.chainedByMinimal unset.
static void SentinelInstallMinimal(void) {
    for (int i = 0; i < kCrashSignalCount; i++) {
        int sig = kCrashSignals[i];
        struct sigaction before;
        struct sigaction replaced;
        struct sigaction action = {0};
        (void)sigaction(sig, NULL, &before);
        gCrashHandler.chainedByMinimal[sig] = before;
        action.sa_sigaction = SentinelOnSignalPrepare;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        (void)sigaction(sig, &action, &replaced);
        if (!SentinelSameAction(&replaced, &before)) {
            gCrashHandler.chainedByMinimal[sig] = replaced;
        }
    }
    atomic_store(&gCrashHandler.minimalOn, 1);
}

// The conditional restore: put back the pre-activation handler only where ours is still current. Something that
// installed itself on top (a crash SDK during a slow PREPARE) is left as it is; it chains to the minimal handler, which
// only chains on. Returns the number of signals restored.
static int SentinelRestoreConditional(void) {
    if (!atomic_load(&gCrashHandler.minimalOn)) {
        return 0;
    }
    int restored = 0;
    for (int i = 0; i < kCrashSignalCount; i++) {
        int sig = kCrashSignals[i];
        struct sigaction current;
        if (sigaction(sig, NULL, &current) == 0 && SentinelIsHandler(&current, SentinelOnSignalPrepare)) {
            (void)sigaction(sig, &gCrashHandler.chainedByMinimal[sig], NULL);
            restored++;
        }
    }
    return restored;
}

// SWAP installs the full handler after the hooks. Where the minimal one is still current, the full handler takes over
// its chain (gCrashHandler.chainedByMinimal), so it never chains through the minimal one; where something else is
// current, it chains to that. Either way a crash is recorded once.
static void SentinelInstallFull(void) {
    if (gSentinel.events.fd < 0) {
        return;
    }
    for (int i = 0; i < kCrashSignalCount; i++) {
        int sig = kCrashSignals[i];
        struct sigaction current;
        struct sigaction action = {0};
        (void)sigaction(sig, NULL, &current);
        gCrashHandler.chainedByFull[sig] =
            SentinelIsHandler(&current, SentinelOnSignalPrepare) ? gCrashHandler.chainedByMinimal[sig] : current;
        action.sa_sigaction = SentinelOnSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        (void)sigaction(sig, &action, NULL);
    }
}
