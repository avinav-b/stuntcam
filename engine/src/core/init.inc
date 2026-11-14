// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): process start, one engine per process, the log, PREPARE 2
// (lookups) and the source factories' resolve step. m6 (Q5): one gate for every hook; W4: the gate is the activation state
// machine in core/lazy.inc (SIMCAM=0 -> nothing at all; a preflight failure -> nothing is installed).
#include <sys/sysctl.h>
static double M6ProcStart(void) {   // CFAbsoluteTime of process start (kinfo_proc.p_starttime)
    static double t; if (t) return t;
    struct kinfo_proc kp; size_t len = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0) t = kp.kp_proc.p_starttime.tv_sec + kp.kp_proc.p_starttime.tv_usec / 1e6 - kCFAbsoluteTimeIntervalSince1970;
    return t;
}
// m11 Q3c lazy arming (variant c), default on: nothing is installed at load but the traps (core/lazy.inc) and the CoreMotion
// stand-in's hooks; the first trapped call runs the bounded activation (W4: core/lazy.inc). SIMCAM_LAZY=0 activates at load.
static int gM11Lazy = -1;
// w1 (U14, design §4.1 item 5): one engine per process. Every engine exports simcam_engine_v1; dlsym(RTLD_DEFAULT) returns the
// first loaded image's copy, so a later engine (another build, a chained or per-launch copy) sees it is not first, says so
// once on stderr and stays inert, like SIMCAM=0 (no traps, no CoreMotion stand-in, gate closed).
__attribute__((visibility("default"))) void simcam_engine_v1(void) {}
static BOOL M12PrimaryEngine(void) {
    static int r = -1;
    if (r < 0) {
        void *first = dlsym(RTLD_DEFAULT, "simcam_engine_v1");
        r = !first || first == (void *)simcam_engine_v1;
        if (!r) { Dl_info me = {0}, them = {0}; dladdr((void *)simcam_engine_v1, &me); dladdr(first, &them);
                  fprintf(stderr, "simcam: duplicate engine ignored (%s); already loaded: %s\n", me.dli_fname, them.dli_fname); }
    }
    return r;
}
// the per-pid log (M5_LOG, %p -> pid): opened once, by PREPARE 2 or by the record queue, whichever comes first (never at load,
// so apps that never use the camera write no log file)
static void ActOpenLog(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        const char *e = getenv("M5_LOG"); if (!e) return;
        char p[1024]; size_t n = 0;
        for (const char *c = e; *c && n < sizeof p - 16; c++) { if (c[0] == '%' && c[1] == 'p') { n += snprintf(p + n, sizeof p - n, "%d", getpid()); c++; } else p[n++] = *c; }
        p[n] = 0; atomic_store_explicit(&gLogF, fopen(p, "a"), memory_order_release);   // published once; LOG loads it atomically (e46)
    });
}
// M2_DUMPKEYS=<file>: print "symbol = value" for each CFString symbol name in file (PREPARE 2: reads and prints only)
static void m2dumpkeys(void) {
    const char *f = getenv("M2_DUMPKEYS");
    if (!f) return;
    void *cmc = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    NSString *s = [NSString stringWithContentsOfFile:@(f) encoding:NSUTF8StringEncoding error:nil];
    for (NSString *name in [s componentsSeparatedByString:@"\n"]) {
        if (!name.length) continue;
        CFStringRef *p = dlsym(cmc, name.UTF8String);
        id v = p ? (__bridge id)*p : nil;
        fprintf(stderr, "KEY %s = %s\n", name.UTF8String, p ? [[v description] UTF8String] : "(nosym)");
    }
}
// PREPARE 2 (lookups): every dlopen activation needs (CoreMedia, CMCapture, AVFCapture), the dlsyms, the profile plist, the log
static void M2Lookups(void) {
    gKeysSeen = [NSMutableSet set];
    ActOpenLog();
    void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
    void *cmc = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
    pCMDerivedObjectCreate = dlsym(cm, "CMDerivedObjectCreate");
    pCMBaseObjectGetDerivedStorage = dlsym(cm, "CMBaseObjectGetDerivedStorage");
    pFigCaptureSourceGetClassID = dlsym(cmc, "FigCaptureSourceGetClassID");
    pFigCaptureDeviceGetNotificationCenter = dlsym(cmc, "FigCaptureDeviceGetNotificationCenter");
    pCMNotificationCenterPostNotification = dlsym(cm, "CMNotificationCenterPostNotification");
    const char *plist = getenv("M2_PLIST");
    if (plist && getenv("M2_FAKE")) gOverride = [NSDictionary dictionaryWithContentsOfFile:@(plist)];
    if (getenv("M2_FAKE")) LOG(@"syms create=%p storage=%p classID=%p plist=%s keys=%@", pCMDerivedObjectCreate, pCMBaseObjectGetDerivedStorage, pFigCaptureSourceGetClassID, plist ?: "-", gOverride.allKeys);
    m2dumpkeys();
}
// PREPARE 4: the source factories (installed in SWAP 4-5: audio, then video, last). m10: microphone by feature detection, not
// OS version. AVF (17.5) skips an audio source whose AttributesDictionary is empty (the stock 17.5 sim source is), and lists a
// device only if -isConnected (NO in the 17.5 sim). W4: the decision moved from activation (a probe of the stock audio sources
// through AVF, not a C/Fig call) into the audio hook's first armed call (fig/sources.inc, m16_audioSources); isConnected is
// hooked unconditionally and answers YES only for our source. M6_FAKE_MIC=1 forces the fake mic, =0 keeps the stock one.
static BOOL M2FigResolve(void) {
    if (!getenv("M2_FAKE")) return YES;
    BOOL ok = ActHookResolve(HG_VIDEO, "video sources", objc_getClass("AVCaptureFigVideoDevice"), "_newFigCaptureSources", YES, NO, (IMP)m2_newFigCaptureSources, NULL, YES);
    Class AD = objc_getClass("AVCaptureFigAudioDevice");
    ok = ok && ActHookResolve(HG_AUDIO, "audio sources", AD, "_newFigCaptureSources", YES, NO, (IMP)m16_audioSources, &gOrigAudioSources, YES);
    ok = ok && ActHookResolve(HG_AUDIO, "mic isConnected", AD, "isConnected", NO, NO, (IMP)m10_micIsConnected, &gOrigIsConnected, YES);
    return ok;
}
