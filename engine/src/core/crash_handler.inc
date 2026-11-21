// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/safety.inc): the sentinel's crash
// handlers: the image ranges a crash is attributed against, the minimal (PREPARE 1) and full (SWAP 6) signal
// handlers and their chaining (design §5.4).
// ---- crash sentinel ----
typedef struct { uintptr_t lo, hi; } SCRange;
static SCRange gSCSelf, gSCRelated[12];
static _Atomic int gSCNRelated;
static struct sigaction gSCOld[32];   // what the full handler chains to
static struct sigaction gSCPre[32];   // what the minimal (prepare) handler chains to: the handler before activation
static const int kSCSigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP, SIGFPE};
#define SC_NSIGS ((int)(sizeof kSCSigs / sizeof *kSCSigs))
// the image's __TEXT range (PREPARE time, not from a handler)
static BOOL SCTextRange(const struct mach_header *mh, intptr_t slide, SCRange *r) {
    __block BOOL found = NO;
    MachOForEachCommand((const struct mach_header_64 *)mh, ^BOOL(const struct load_command *lc) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
            const struct segment_command_64 *seg = (const void *)lc;
            r->lo = seg->vmaddr + slide;
            r->hi = r->lo + seg->vmsize;
            found = YES;
        }
        return found;
    });
    return found;
}
// Called before the minimal handler goes in (PREPARE 1) and again after PREPARE 2's dlopens. A range is written before
// the count that covers it is published, so a handler never reads a half-written range; ranges already cached stay as
// they are.
static void SCCacheRanges(void) {
    Dl_info di;
    const struct mach_header *self = dladdr((const void *)SCCacheRanges, &di) ? di.dli_fbase : NULL;
    static const char *rel[] = {"/AVFCapture", "/AVFoundation", "/CMCapture", "/CoreMedia", "/CoreVideo", "/IOSurface",
                                "/ImageIO", "/VideoToolbox"};
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header *mh = _dyld_get_image_header(i);
        const char *imageName = _dyld_get_image_name(i);
        if (!mh || !imageName) continue;
        size_t nameLen = strlen(imageName);
        if (mh == self) {
            if (!gSCSelf.hi) SCTextRange(mh, _dyld_get_image_vmaddr_slide(i), &gSCSelf);
            continue;
        }
        for (unsigned j = 0; j < sizeof rel / sizeof *rel; j++) {
            size_t suffixLen = strlen(rel[j]);
            if (nameLen < suffixLen || strcmp(imageName + nameLen - suffixLen, rel[j])) continue;
            int n = atomic_load(&gSCNRelated);
            SCRange r;
            if (n >= 12 || !SCTextRange(mh, _dyld_get_image_vmaddr_slide(i), &r)) break;
            BOOL have = NO;
            for (int k = 0; k < n; k++)
                if (gSCRelated[k].lo == r.lo) have = YES;
            if (!have) {
                gSCRelated[n] = r;
                atomic_store(&gSCNRelated, n + 1);
            }
            break;
        }
    }
}
static uintptr_t SCPC(void *ctx) {
    ucontext_t *uc = ctx;
    if (!uc || !uc->uc_mcontext) return 0;
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
static BOOL SCInRelated(uintptr_t pc) {
    int n = atomic_load(&gSCNRelated);
    for (int i = 0; i < n; i++)
        if (pc >= gSCRelated[i].lo && pc < gSCRelated[i].hi) return YES;
    return NO;
}
// chain: the previous handler if it was one, else the default action
static void SCChain(int sig, siginfo_t *si, void *ctx, struct sigaction *old) {
    if (old && (old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        sigaction(sig, old, NULL);
        old->sa_sigaction(sig, si, ctx);
        return;
    }
    if (old && !(old->sa_flags & SA_SIGINFO) && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        sigaction(sig, old, NULL);
        old->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    // A fault the kernel generated (SEGV/BUS/ILL/FPE/TRAP from an instruction): return, so the instruction runs again
    // under SIG_DFL and the crash report shows the real exception (EXC_BAD_ACCESS ...) on the crashing thread. raise()
    // here would make it EXC_CRASH (SIGSEGV) blamed on thread 0, with the crashing thread's stack empty. Sent signals
    // (abort, kill) are re-sent.
    if (sig != SIGABRT && si && si->si_code != SI_USER && si->si_code != SI_QUEUE) return;
    raise(sig);
}
// PREPARE 1 counts this build's `motion` strikes in the window and keeps the newest one's epoch and the
// latched-motion marker's path next to the events.log in use, so the full handler can write the marker on the second
// strike itself (async-signal-safe: open, write, futimens). Motion is decided at load, so a marker the next launch's
// PREPARE 1 wrote would come one launch late. SIMCAM_OFF_HANDLER_MOTION_MARKER=1 (ledger L60) skips the write.
// Written to a temp file next to it (path precomputed by PREPARE 1), its until set, then
// renamed into place, so the marker never exists with mtime = now (which another process's prune reads as expired).
// SIMCAM_OFF_MARKER_RENAME=1 (ledger L64's mutant) writes in place, as SCWriteMarker does under it.
static int gSCMotionPrior;
static long gSCMotionPriorEpoch;
static char gSCMotionMarkerUse[1100], gSCMotionMarkerTmp[1200];
static BOOL gOffHandlerMotionMarker;
static BOOL gOffMarkerRename;   // SIMCAM_OFF_MARKER_RENAME (L64)
static long gTestSlowMarkerMs;    // SIMCAM_TEST_SLOW_MARKER_MS (pf-prune-race)
static long gTestPrunePauseMs;    // SIMCAM_TEST_PRUNE_PAUSE_MS (pf-prune-kept)
static void SCMotionMarkerFromHandler(void) {   // async-signal-safe only
    if (gSCMotionPrior < 1 || !gSCMotionMarkerUse[0] || gOffHandlerMotionMarker) return;
    const char *writePath = gOffMarkerRename || !gSCMotionMarkerTmp[0] ? gSCMotionMarkerUse : gSCMotionMarkerTmp;
    int fd = open(writePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    // in a signal handler there is nothing safe to do about a failed write; the marker latches by its name and mtime
    // (set below). Its content is what `stuntcam sentinel` and `status` show (the CLI derives the latch's effect from
    // it), so a short write only degrades that report
    (void)write(fd, "motion\n", 7);
    struct timespec ts[2] = {{gSCMotionPriorEpoch + SC_WINDOW_SECONDS, 0},
                              {gSCMotionPriorEpoch + SC_WINDOW_SECONDS, 0}};
    futimens(fd, ts);
    close(fd);
    if (writePath != gSCMotionMarkerUse && rename(writePath, gSCMotionMarkerUse) != 0) unlink(writePath);
}
static void SCOnSignal(int sig, siginfo_t *si, void *ctx) {   // async-signal-safe only
    uintptr_t pc = SCPC(ctx);
    BOOL self = pc >= gSCSelf.lo && pc < gSCSelf.hi;
    // the feature key first: a fault in our image on a thread inside a motion entry point is `motion`, not `provider`
    // or an inflight op's feature, and it never closes the camera. An app handler runs with the key cleared (and an app
    // PC).
    const void *threadFeature =
        atomic_load_explicit(&gSCFeatKeyOn, memory_order_relaxed) ? pthread_getspecific(gSCFeatKey) : NULL;
    const char *op = NULL;
    for (int i = 0; i < SC_MAXOPS; i++) {
        if (atomic_load(&gSCOps[i].used)) {
            op = gSCOps[i].feature;
            break;
        }
    }
    const char *feature = NULL;
    if (self && threadFeature == (const void *)kSCMotion) feature = kSCMotion;
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
static BOOL SCIsHandler(const struct sigaction *a, void *fn) {
    return (a->sa_flags & SA_SIGINFO) && (void *)a->sa_sigaction == fn;
}
static BOOL SCSameAction(const struct sigaction *a, const struct sigaction *b) {
    return a->sa_flags == b->sa_flags &&
           ((a->sa_flags & SA_SIGINFO) ? (void *)a->sa_sigaction == (void *)b->sa_sigaction
                                       : a->sa_handler == b->sa_handler);
}
// install order: read the current handler first, then install ours; if what we replaced differs from the pre-read
// (someone installed in between), chain to that instead. The minimal handler never runs with gSCPre unset.
static void SCInstallMinimal(void) {
    for (int i = 0; i < SC_NSIGS; i++) {
        int sig = kSCSigs[i];
        struct sigaction pre, old, sa = {0};
        sigaction(sig, NULL, &pre);
        gSCPre[sig] = pre;
        sa.sa_sigaction = SCOnSignalPrepare;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &old);
        if (!SCSameAction(&old, &pre)) gSCPre[sig] = old;
    }
    atomic_store(&gSCMinimalOn, 1);
}
// conditional restore: put back the pre-activation handler only where ours is still current. Something that
// installed itself on top (a crash SDK during a slow prepare) is left as it is; it chains to the minimal handler, which
// only chains on. Returns the number of signals restored.
static int SCRestoreConditional(void) {
    if (!atomic_load(&gSCMinimalOn)) return 0;
    int n = 0;
    for (int i = 0; i < SC_NSIGS; i++) {
        int sig = kSCSigs[i];
        struct sigaction cur;
        if (sigaction(sig, NULL, &cur) == 0 && SCIsHandler(&cur, SCOnSignalPrepare)) {
            sigaction(sig, &gSCPre[sig], NULL);
            n++;
        }
    }
    return n;
}
// SWAP 6: the full handler. Where the minimal one is still current it takes over its chain (gSCPre), so the full
// handler never chains through the minimal one; where something else is current it chains to that. One record per
// crash either way.
static void SCInstallFull(void) {
    if (gSCFd < 0) return;
    for (int i = 0; i < SC_NSIGS; i++) {
        int sig = kSCSigs[i];
        struct sigaction cur, sa = {0};
        sigaction(sig, NULL, &cur);
        gSCOld[sig] = SCIsHandler(&cur, SCOnSignalPrepare) ? gSCPre[sig] : cur;
        sa.sa_sigaction = SCOnSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, NULL);
    }
}
