// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): process start, one engine per process, the log,
// PREPARE's lookups step and the source factories' resolve step.
//
// Provides: IsPrimaryEngine, OpenLog, ActivationLoadFrameworks, ActivationResolveSourceHooks (core/lazy.inc)
//           ProcessStartTime (providers/video.inc, providers/scene.inc, core/lazy.inc)
//           simcam_engine_v1, an exported symbol other engine images look up
// Owns:     nothing shared (SIMCAM_LAZY's setting is in core/activation.h; core/lazy.inc sets it).
// Uses:     fig/sources.inc (fills its function pointers, profile and original IMPs), core/act.inc
//
// Every hook goes through one gate, the activation state machine in core/lazy.inc: with SIMCAM=0 nothing happens at
// all, and after a preflight failure nothing is installed.
#include <sys/sysctl.h>
#include "activation.h"
#include "init.h"
#include "../fig/sources.h"
#define kLogPathPidRoom 16   // room left in the log path for the pid that replaces %p

static double ProcessStartTime(void) {   // CFAbsoluteTime of process start (kinfo_proc.p_starttime)
    static double startTime;
    if (startTime) {
        return startTime;
    }
    struct kinfo_proc procInfo;
    size_t len = sizeof procInfo;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &procInfo, &len, NULL, 0) == 0) {
        startTime = procInfo.kp_proc.p_starttime.tv_sec + procInfo.kp_proc.p_starttime.tv_usec / 1e6 -
                    kCFAbsoluteTimeIntervalSince1970;
    }
    return startTime;
}

// Lazy arming is on by default (gActivation.lazy): nothing is installed at load but the traps (core/lazy.inc) and the
// CoreMotion stand-in's hooks, and the first trapped call runs the bounded activation (core/lazy.inc). SIMCAM_LAZY=0
// activates at load.

// One engine per process: two would hook the same methods twice. Every engine exports simcam_engine_v1, and
// dlsym(RTLD_DEFAULT) returns the first loaded image's copy. So a later engine (another build, or a chained or
// per-launch copy) sees it is not first, says so once on stderr and stays inert, like SIMCAM=0 (no traps, no
// CoreMotion stand-in, gate closed).
__attribute__((visibility("default"))) void simcam_engine_v1(void) {}

static BOOL IsPrimaryEngine(void) {
    static int isPrimary = -1;
    if (isPrimary < 0) {
        void *first = dlsym(RTLD_DEFAULT, "simcam_engine_v1");
        isPrimary = !first || first == (void *)simcam_engine_v1;
        if (!isPrimary) {
            Dl_info ownInfo = {0};
            Dl_info firstInfo = {0};
            dladdr((void *)simcam_engine_v1, &ownInfo);
            dladdr(first, &firstInfo);
            fprintf(stderr, "simcam: duplicate engine ignored (%s); already loaded: %s\n", ownInfo.dli_fname,
                    firstInfo.dli_fname);
        }
    }
    return isPrimary;
}

// The per-pid log (SIMCAM_ENGINE_LOG, where %p becomes the pid) is opened once, by PREPARE or by the record queue,
// whichever comes first. It is never opened at load, so apps that never use the camera write no log file.
static void OpenLog(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *pattern = EnvConfig(ENV_ENGINE_LOG, ENV_LEGACY_ENGINE_LOG);
        if (!pattern) {
            return;
        }
        char path[1024];
        size_t len = 0;
        for (const char *cursor = pattern; *cursor && len < sizeof path - kLogPathPidRoom; cursor++) {
            if (cursor[0] == '%' && cursor[1] == 'p') {
                len += snprintf(path + len, sizeof path - len, "%d", getpid());
                cursor++;
            } else {
                path[len++] = *cursor;
            }
        }
        path[len] = 0;
        // published once; LOG loads it atomically (TSan)
        atomic_store_explicit(&gLogF, fopen(path, "a"), memory_order_release);
    });
}

// SIMCAM_DIAG_DUMP_KEYS=<file> prints "symbol = value" for each CFString symbol named in the file (in the lookups
// step; it only reads and prints).
static void ActivationDumpKeys(void) {
    const char *keysPath = getenv(ENV_DIAG_DUMP_KEYS);
    if (!keysPath) {
        return;
    }
    void *cmCapture = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    NSString *keysText = [NSString stringWithContentsOfFile:@(keysPath) encoding:NSUTF8StringEncoding error:nil];
    for (NSString *name in [keysText componentsSeparatedByString:@"\n"]) {
        if (!name.length) {
            continue;
        }
        CFStringRef *symbol = dlsym(cmCapture, name.UTF8String);
        id value = symbol ? (__bridge id)*symbol : nil;
        fprintf(stderr, "KEY %s = %s\n", name.UTF8String, symbol ? [[value description] UTF8String] : "(nosym)");
    }
}

// PREPARE's lookups step: every dlopen activation needs (CoreMedia, CMCapture, AVFCapture), the dlsyms, the profile
// plist and the log.
static void ActivationLoadFrameworks(void) {
    gKeysSeen = [NSMutableSet set];
    OpenLog();
    void *coreMedia = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
    void *cmCapture = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
    pCMDerivedObjectCreate = dlsym(coreMedia, "CMDerivedObjectCreate");
    pCMBaseObjectGetDerivedStorage = dlsym(coreMedia, "CMBaseObjectGetDerivedStorage");
    pFigCaptureSourceGetClassID = dlsym(cmCapture, "FigCaptureSourceGetClassID");
    pFigCaptureDeviceGetNotificationCenter = dlsym(cmCapture, "FigCaptureDeviceGetNotificationCenter");
    pCMNotificationCenterPostNotification = dlsym(coreMedia, "CMNotificationCenterPostNotification");
    const char *plist = EnvConfig(ENV_PROFILE, ENV_LEGACY_PROFILE);
    if (plist && EnvConfig(ENV_FIG_SOURCES, ENV_LEGACY_FIG_SOURCES)) {
        gOverride = [NSDictionary dictionaryWithContentsOfFile:@(plist)];
    }
    if (EnvConfig(ENV_FIG_SOURCES, ENV_LEGACY_FIG_SOURCES)) {
        LOG(@"syms create=%p storage=%p classID=%p plist=%s keys=%@", pCMDerivedObjectCreate,
            pCMBaseObjectGetDerivedStorage, pFigCaptureSourceGetClassID, plist ?: "-", gOverride.allKeys);
    }
    ActivationDumpKeys();
}

// The resolve step for the source factories (SWAP installs them after the session hook: audio, then video, last). The
// microphone is chosen by feature detection, not by OS version. AVFoundation on iOS 17.5 skips an audio source whose
// AttributesDictionary is empty (the stock 17.5 simulator source's is), and lists a device only if -isConnected (NO in
// the 17.5 simulator). The decision isn't made at activation (that would need a probe of the stock audio sources
// through AVFoundation, not a C or Fig call) but in the audio hook's first armed call (fig/sources.inc,
// hook_audioNewFigCaptureSources). isConnected is hooked unconditionally and answers YES only for our source.
// SIMCAM_FAKE_MIC=1 forces the fake mic; =0 keeps the stock one.
static BOOL ActivationResolveSourceHooks(void) {
    if (!EnvConfig(ENV_FIG_SOURCES, ENV_LEGACY_FIG_SOURCES)) {
        return YES;
    }
    BOOL ok = ActivationHookResolve(kHookGroupVideo, "video sources", objc_getClass("AVCaptureFigVideoDevice"),
                                    "_newFigCaptureSources", YES, NO, (IMP)hook_videoNewFigCaptureSources, NULL, YES);
    Class audioDevice = objc_getClass("AVCaptureFigAudioDevice");
    ok = ok && ActivationHookResolve(kHookGroupAudio, "audio sources", audioDevice, "_newFigCaptureSources", YES, NO,
                                     (IMP)hook_audioNewFigCaptureSources, &gOrigAudioSources, YES);
    ok = ok && ActivationHookResolve(kHookGroupAudio, "mic isConnected", audioDevice, "isConnected", NO, NO,
                                     (IMP)hook_audioIsConnected, &gOrigIsConnected, YES);
    return ok;
}
