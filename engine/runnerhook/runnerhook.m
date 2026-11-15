// SPDX-License-Identifier: Apache-2.0
// simcam runner hook (libsimcamrunnerhook.dylib): inserted into XCTest runner processes by `stuntcam exec` through
// TEST_RUNNER_DYLD_INSERT_LIBRARIES (design §4.2, §4.5), for destinations that are launchd-armed or booted unarmed,
// where Xcode's per-launch insert list replaces the stub's. It never runs in a user app as an app: in a hosted
// test it runs in the test host only to put back what the variable displaced.
//
// 1. Restores what TEST_RUNNER_DYLD_INSERT_LIBRARIES displaced from the runner's own insert list (that variable replaces
//    XCTest's list; for hosted tests that drops libXCTestBundleInject and the test bundle is never injected: xcodebuild hangs,
//    waiting for it). The list comes from the "test-runner" rule of the restore-rules file ($SIMCAM_HOOK_RULES), else the built-in
//    copy below: <test host>/Frameworks/libXCTestBundleInject.dylib (when the host has it and an XCTest marker is set), MTC
//    (unless SIMCAM_MTC=0), RPAC (if PERFC_*). Launch-only libraries (libclang_rt.*, libgmalloc) are never dlopened, nor a
//    basename that is already mapped. Done in the constructor, the same point in launch where the boot-mode loader restores.
// 2. In a UI-test runner (main bundle id *.xctrunner, or an XCTest process without XCInjectBundleInto), makes every app the test launches get stuntcam's stub prepended to its
//    DYLD_INSERT_LIBRARIES and stuntcam's variables added to its environment. Xcode 27.0's XCTest builds each launch's
//    environment in -[XCUIApplication _combinedLaunchEnvironment] (Xcode's target-app environment with the test's
//    launchEnvironment over it; read by launch, activate and launchWithoutAccessibility:), so that getter is swizzled and
//    its result extended; the test's launchEnvironment is never touched. Without that method (another XCTest), -launch and
//    -activate (and launchWithOptions:-style variants if present) are swizzled instead and edit launchEnvironment around the
//    call, putting it back afterwards. Either way the list kept behind the stub is the test's own DYLD_INSERT_LIBRARIES if it
//    set one (never dropped), else Xcode's, launch-only libraries first so a sanitizer runtime still initializes first.
//    Variables the test set itself win. Apple's own apps (com.apple.*) are left alone.
// 3. Inert when the destination is boot-armed (the stub is mapped in this runner: the boot env already arms every app), when
//    $SIMCAM_HOOK_DIR/<udid>.env says boot, or when there is nothing to add (no stub for this sim).
// What to add comes from $SIMCAM_HOOK_DIR/<SIMULATOR_UDID>.env (KEY=VALUE lines written by exec for each destination; first
// line "mode=launchd|unarmed|boot"), else, on a launchd-armed sim exec could not name, the SIMCAM_* variables launchd gives
// this process plus $SIMCAM_HOOK_STUB. Logs to $SIMCAM_HOOK_DIR/<udid>.log: "armed <bundle> via <method>" per launch.
// Any failure: one log line ("failed (…)", which exec's summary repeats), and the launch goes ahead untouched; a hosted
// test whose libXCTestBundleInject can't be restored logs "failed (hosted tests can't inject their bundle …)". SIMCAM=0 -> nothing. SIMCAM_HOOK_DIAG=1 also logs the
// environment, the mapped images and XCUIApplication's launch selectors.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <mach-o/dyld.h>
#import <dlfcn.h>
#import <unistd.h>
#import <fcntl.h>
#import <time.h>
#import <stdatomic.h>
#import <os/lock.h>

static int gfd = -1;   // set once in the constructor, before any hook is installed
static char udid[64];
static void hlog(NSString *s) {
    if (gfd < 0) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    NSString *l = [NSString stringWithFormat:@"%ld.%06ld [hook %d %s] %@\n", (long)ts.tv_sec, ts.tv_nsec / 1000, getpid(), getprogname(), s];
    const char *c = l.UTF8String; if (c) write(gfd, c, strlen(c));
}
static const char *base(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }
static const char *mapped(const char *name) {   // a mapped image with exactly this basename
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) { const char *nm = _dyld_get_image_name(i); if (nm && !strcmp(base(nm), name)) return nm; }
    return NULL;
}
static BOOL launchOnly(NSString *p) { NSString *b = p.lastPathComponent; return [b hasPrefix:@"libclang_rt."] || [b isEqualToString:@"libgmalloc.dylib"]; }

// ---- 1. restore the runner's own list ----------------------------------------------------------------------------------
static BOOL cond(NSString *c, NSDictionary *env) {
    if (!c.length) return YES;
    if ([c hasSuffix:@"*"]) { NSString *p = [c substringToIndex:c.length - 1]; for (NSString *k in env) if ([k hasPrefix:p]) return YES; return NO; }
    NSRange eq = [c rangeOfString:@"="];
    if (eq.location == NSNotFound) return env[c] != nil;
    return [env[[c substringToIndex:eq.location]] isEqualToString:[c substringFromIndex:eq.location + 1]];
}
static NSArray *builtinRunnerLibs(void) {   // = restore-rules/27A266a.json "test-runner"
    return @[@{@"path": @"@testhost/Frameworks/libXCTestBundleInject.dylib", @"if": @"XCInjectBundleInto"},
             @{@"path": @"/usr/lib/libMainThreadChecker.dylib", @"unless": @"SIMCAM_MTC=0"},
             @{@"path": @"/usr/lib/libRPAC.dylib", @"if": @"PERFC_*"}];
}
// @testhost/<rel>: where dyld finds what XCTest names "<built products dir>/<App>.app/<rel>": that file doesn't exist; dyld
// takes the leaf from DYLD_LIBRARY_PATH first (Xcode's platform usr/lib copy), so do the same, then the built and the
// installed host
static NSString *testhost(NSString *rel, NSDictionary *env) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *host = NSBundle.mainBundle.bundlePath ?: @"";
    for (NSString *d in [env[@"DYLD_LIBRARY_PATH"] ?: @"" componentsSeparatedByString:@":"]) {
        NSString *p = [d stringByAppendingPathComponent:rel.lastPathComponent];
        if (d.length && [fm fileExistsAtPath:p]) return p;
    }
    for (NSString *d in [env[@"__XCODE_BUILT_PRODUCTS_DIR_PATHS"] ?: @"" componentsSeparatedByString:@":"]) {
        NSString *p = [[d stringByAppendingPathComponent:host.lastPathComponent] stringByAppendingPathComponent:rel];
        if (d.length && [fm fileExistsAtPath:p]) return p;
    }
    return [host stringByAppendingPathComponent:rel];
}
static NSArray *runnerLibs(NSDictionary *env) {
    NSString *rf = env[@"SIMCAM_HOOK_RULES"];
    if (rf.length) {
        NSData *d = [NSData dataWithContentsOfFile:rf];
        id j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
        if ([j isKindOfClass:NSDictionary.class] && [j[@"rules"] isKindOfClass:NSArray.class])
            for (id r in j[@"rules"])
                if ([r isKindOfClass:NSDictionary.class] && [r[@"name"] isEqual:@"test-runner"] && [r[@"libs"] isKindOfClass:NSArray.class]) {
                    hlog([NSString stringWithFormat:@"rules: %@", rf]); return r[@"libs"];
                }
        hlog([NSString stringWithFormat:@"rules: no test-runner rule in %@; built-in used", rf]);
    }
    return builtinRunnerLibs();
}
static void restoreRunner(NSDictionary *env, BOOL xctest, BOOL runner) {
    if ([env[@"SIMCAM_XCODE_LIBS"] isEqualToString:@"0"]) { hlog(@"restore: SIMCAM_XCODE_LIBS=0"); return; }
    NSString *root = env[@"SIMULATOR_ROOT"] ?: env[@"DYLD_ROOT_PATH"] ?: @"";
    for (id l in runnerLibs(env)) {
        if (![l isKindOfClass:NSDictionary.class] || ![l[@"path"] isKindOfClass:NSString.class]) continue;
        NSString *p = l[@"path"], *c = [l[@"if"] isKindOfClass:NSString.class] ? l[@"if"] : nil, *u = [l[@"unless"] isKindOfClass:NSString.class] ? l[@"unless"] : nil;
        if (!cond(c, env) || (u.length && cond(u, env))) continue;
        if ([p hasPrefix:@"@testhost/"]) { if (!xctest || runner) continue;   // a UI-test runner never had it
            p = testhost([p substringFromIndex:10], env); }
        else if ([p hasPrefix:@"/usr/"] || [p hasPrefix:@"/System/"]) { NSString *q = [root stringByAppendingString:p]; if (root.length && [NSFileManager.defaultManager fileExistsAtPath:q]) p = q; }
        BOOL inject = [l[@"path"] hasPrefix:@"@testhost/"];   // a hosted test's bundle injector: without it the run hangs
        if (![NSFileManager.defaultManager fileExistsAtPath:p]) {
            hlog([NSString stringWithFormat:inject ? @"failed (hosted tests can't inject their bundle; the run may hang): absent %@" : @"restore: absent %@", p]); continue;
        }
        if (launchOnly(p)) { hlog([NSString stringWithFormat:@"restore: skipped (must load at launch) %@", p]); continue; }
        const char *have = mapped(base(p.fileSystemRepresentation));
        if (have) { hlog([NSString stringWithFormat:@"restore: already loaded %s", have]); continue; }
        void *h = dlopen(p.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL);
        if (!h && inject) hlog([NSString stringWithFormat:@"failed (hosted tests can't inject their bundle; the run may hang): %@ %s", p, dlerror()]);
        else hlog([NSString stringWithFormat:@"restore: %@ %s", p, h ? "loaded" : dlerror()]);
    }
}

// ---- 2. the app under test -----------------------------------------------------------------------------------------------
static NSString *gStub;                 // our stub, or nil: inert (set in the constructor, before any swizzle; read-only after)
static NSDictionary *gVars;             // SIMCAM_* etc. for the app (never DYLD_INSERT_LIBRARIES); same
static atomic_bool gFailedOnce;
static void fail(NSString *what) { if (atomic_exchange(&gFailedOnce, true)) return; hlog([NSString stringWithFormat:@"failed (launch left as it was): %@", what]); }
// the original IMPs, one fixed slot per swizzled selector: each is stored (and published) before its method is swizzled, and
// never changed afterwards, so a launch on any thread reads a complete entry without a lock
enum { kMaxHooks = 8 };
static struct { SEL sel; _Atomic(IMP) orig; } gOrig[kMaxHooks];
static atomic_int gOrigN;
static IMP origOf(SEL s) {
    for (int i = 0, n = atomic_load(&gOrigN); i < n; i++) if (gOrig[i].sel == s) return atomic_load(&gOrig[i].orig);
    return NULL;
}
// swizzles m to imp, with its original saved first (a concurrent call to imp finds it); NO if out of slots
static BOOL swizzle(Method m, IMP imp) {
    int i = atomic_load(&gOrigN); if (i >= kMaxHooks) return NO;
    gOrig[i].sel = method_getName(m); atomic_store(&gOrig[i].orig, method_getImplementation(m));
    atomic_store(&gOrigN, i + 1);                    // published before the swizzle
    IMP was = method_setImplementation(m, imp);
    if (was != atomic_load(&gOrig[i].orig)) atomic_store(&gOrig[i].orig, was);   // changed in between by someone else: call theirs
    return YES;
}

// what Xcode itself puts in the app's DYLD_INSERT_LIBRARIES (UITargetAppEnvironmentVariables), as the runner was told
static NSString *xcodeTargetInsert(void) {
    @try {
        Class c = NSClassFromString(@"XCTestConfiguration");
        if (![c respondsToSelector:@selector(activeTestConfiguration)]) return nil;
        id cfg = ((id (*)(id, SEL))objc_msgSend)(c, @selector(activeTestConfiguration));
        if (!cfg || ![cfg respondsToSelector:@selector(targetApplicationEnvironment)]) return nil;
        NSDictionary *e = ((id (*)(id, SEL))objc_msgSend)(cfg, @selector(targetApplicationEnvironment));
        id v = [e isKindOfClass:NSDictionary.class] ? e[@"DYLD_INSERT_LIBRARIES"] : nil;
        return [v isKindOfClass:NSString.class] ? v : nil;
    } @catch (NSException *x) { return nil; }
}
static BOOL ours(NSString *p) { return [p.lastPathComponent hasPrefix:@"libsimcam"]; }
// stub + base, launch-only libraries first, no duplicates, no other copy of ours
static NSString *merged(NSString *b) {
    NSMutableArray *first = [NSMutableArray new], *rest = [NSMutableArray new];
    for (NSString *e in [b componentsSeparatedByString:@":"]) {
        if (!e.length || ours(e) || [first containsObject:e] || [rest containsObject:e]) continue;
        [(launchOnly(e) ? first : rest) addObject:e];
    }
    [first addObject:gStub]; [first addObjectsFromArray:rest];
    return [first componentsJoinedByString:@":"];
}
static NSString *bundleOf(id app) {
    @try { for (NSString *k in @[@"bundleID", @"bundleIdentifier"]) if ([app respondsToSelector:NSSelectorFromString(k)]) { id v = [app valueForKey:k]; if ([v isKindOfClass:NSString.class]) return v; } }
    @catch (NSException *x) {}
    return nil;
}
// returns the launchEnvironment to put back afterwards, or nil if nothing was changed
static NSDictionary *prepare(id app, const char *how) {
    if (!gStub) return nil;
    @try {
        NSString *bid = bundleOf(app);
        if ([bid hasPrefix:@"com.apple."]) return nil;
        NSDictionary *orig = [app valueForKey:@"launchEnvironment"];
        if (orig && ![orig isKindOfClass:NSDictionary.class]) { fail(@"launchEnvironment is not a dictionary"); return nil; }
        NSMutableDictionary *env = orig ? [orig mutableCopy] : [NSMutableDictionary new];
        NSString *own = env[@"DYLD_INSERT_LIBRARIES"];
        NSString *b = own.length ? own : (xcodeTargetInsert() ?: @"");
        env[@"DYLD_INSERT_LIBRARIES"] = merged(b);
        for (NSString *k in gVars) if (!env[k]) env[k] = gVars[k];
        [app setValue:env forKey:@"launchEnvironment"];
        hlog([NSString stringWithFormat:@"armed %@ via %s: DYLD_INSERT_LIBRARIES=%@ (%@)", bid ?: @"?", how, env[@"DYLD_INSERT_LIBRARIES"],
              own.length ? @"merged with the test's own" : b.length ? @"merged with Xcode's list" : @"Xcode's list unknown"]);
        return orig ?: @{};
    } @catch (NSException *x) { fail([NSString stringWithFormat:@"%@: %@", x.name, x.reason]); return nil; }
}
static void putback(id app, NSDictionary *orig) {
    if (!orig) return;
    @try { [app setValue:orig forKey:@"launchEnvironment"]; } @catch (NSException *x) { fail(@"restoring launchEnvironment"); }
}
// -[XCUIApplication _combinedLaunchEnvironment] (Xcode 27.0's XCTest): Xcode's target-app environment with the test's
// launchEnvironment over it, read by every launch path (launch, activate, launchWithoutAccessibility:). Ours goes on top.
static NSString *lastLogged;               // guarded by gLogLock
static unsigned gLoggedGen;                // guarded by gLogLock
static atomic_uint gLaunchGen;             // bumped by each outermost launch/activate call: one "armed" line per launch
static os_unfair_lock gLogLock = OS_UNFAIR_LOCK_INIT;
static id hookedCombined(id self, SEL _cmd) {
    IMP o = origOf(_cmd); if (!o) return nil;   // cannot happen: saved before the swizzle
    id r = ((id (*)(id, SEL))o)(self, _cmd);
    if (!gStub) return r;
    @try {
        if (![r isKindOfClass:NSDictionary.class]) { fail(@"_combinedLaunchEnvironment is not a dictionary"); return r; }
        NSString *bid = bundleOf(self);
        if ([bid hasPrefix:@"com.apple."]) return r;
        NSMutableDictionary *env = [r mutableCopy];
        NSString *b = env[@"DYLD_INSERT_LIBRARIES"] ?: @"";
        NSString *own = [[self valueForKey:@"launchEnvironment"] isKindOfClass:NSDictionary.class] ? [self valueForKey:@"launchEnvironment"][@"DYLD_INSERT_LIBRARIES"] : nil;
        env[@"DYLD_INSERT_LIBRARIES"] = merged(b);
        NSDictionary *tests = [self valueForKey:@"launchEnvironment"];
        for (NSString *k in gVars) if (![tests isKindOfClass:NSDictionary.class] || !tests[k]) env[k] = gVars[k];
        NSString *l = [NSString stringWithFormat:@"armed %@ via _combinedLaunchEnvironment: DYLD_INSERT_LIBRARIES=%@ (%@)", bid ?: @"?", env[@"DYLD_INSERT_LIBRARIES"],
                       own.length ? @"merged with the test's own" : b.length ? @"merged with Xcode's list" : @"no other list"];
        unsigned gen = atomic_load(&gLaunchGen); BOOL say = NO;
        os_unfair_lock_lock(&gLogLock);
        if (gen != gLoggedGen || ![l isEqualToString:lastLogged]) { say = YES; lastLogged = l; gLoggedGen = gen; }
        os_unfair_lock_unlock(&gLogLock);
        if (say) hlog(l);
        return env;
    } @catch (NSException *x) { fail([NSString stringWithFormat:@"%@: %@", x.name, x.reason]); return r; }
}
static __thread int depth;   // launch -> activate (or an internal call) prepares once, at the outermost call
static void hooked0(id self, SEL _cmd) {
    IMP o = origOf(_cmd); NSDictionary *orig = depth++ ? nil : prepare(self, sel_getName(_cmd));
    @try { ((void (*)(id, SEL))o)(self, _cmd); } @finally { depth--; putback(self, orig); }
}
static void hooked1(id self, SEL _cmd, id a) {
    IMP o = origOf(_cmd); NSDictionary *orig = depth++ ? nil : prepare(self, sel_getName(_cmd));
    @try { ((void (*)(id, SEL, id))o)(self, _cmd, a); } @finally { depth--; putback(self, orig); }
}
// with _combinedLaunchEnvironment hooked, the launch methods only count launches (for the log); the environment is not touched
static void counted0(id self, SEL _cmd) { if (!depth++) atomic_fetch_add(&gLaunchGen, 1); @try { ((void (*)(id, SEL))origOf(_cmd))(self, _cmd); } @finally { depth--; } }
static void countedB(id self, SEL _cmd, BOOL a) { if (!depth++) atomic_fetch_add(&gLaunchGen, 1); @try { ((void (*)(id, SEL, BOOL))origOf(_cmd))(self, _cmd, a); } @finally { depth--; } }
static atomic_bool hookedUI;
static void hookXCUI(void) {
    if (atomic_load(&hookedUI)) return;
    Class c = objc_getClass("XCUIApplication"); if (!c) return;
    static atomic_int once; int zero = 0; if (!atomic_compare_exchange_strong(&once, &zero, 1)) return;   // add-image callbacks can come from any thread
    atomic_store(&hookedUI, true);
    if (getenv("SIMCAM_HOOK_DIAG")) {
        unsigned n = 0; Method *ms = class_copyMethodList(c, &n); NSMutableArray *names = [NSMutableArray new];
        for (unsigned i = 0; i < n; i++) { NSString *s = NSStringFromSelector(method_getName(ms[i])); if ([s.lowercaseString containsString:@"launch"] || [s.lowercaseString containsString:@"activate"]) [names addObject:[NSString stringWithFormat:@"%@ %s", s, method_getTypeEncoding(ms[i])]]; }
        free(ms); hlog([NSString stringWithFormat:@"diag: XCUIApplication selectors: %@", [names componentsJoinedByString:@", "]]);
    }
    NSMutableArray *done = [NSMutableArray new];
    Method cm = class_getInstanceMethod(c, NSSelectorFromString(@"_combinedLaunchEnvironment"));
    if (cm && !getenv("SIMCAM_HOOK_PUBLIC") && !strcmp(method_getTypeEncoding(cm) ?: "", "@16@0:8")) {
        swizzle(cm, (IMP)hookedCombined);
        for (NSString *n in @[@"launch", @"activate", @"launchWithoutAccessibility:"]) {
            Method m = class_getInstanceMethod(c, NSSelectorFromString(n)); const char *t = m ? method_getTypeEncoding(m) : NULL;
            IMP imp = !t ? NULL : !strcmp(t, "v16@0:8") ? (IMP)counted0 : !strcmp(t, "v20@0:8B16") ? (IMP)countedB : NULL;
            if (imp) swizzle(m, imp);
        }
        hlog(@"hooked -[XCUIApplication _combinedLaunchEnvironment]");
        return;
    }
    // no such method (another XCTest): the public launch methods, the test's launchEnvironment edited around the call;
    // only void methods with no or one object argument; anything else this XCTest has is left alone
    for (NSString *s in @[@"launch", @"activate", @"launchWithOptions:", @"launchWithConfiguration:", @"activateWithOptions:"]) {
        Method m = class_getInstanceMethod(c, NSSelectorFromString(s)); if (!m) continue;
        const char *t = method_getTypeEncoding(m); int args = (int)method_getNumberOfArguments(m) - 2;
        char rt[8] = ""; method_getReturnType(m, rt, sizeof rt);
        if (strcmp(rt, "v") || args > 1) { hlog([NSString stringWithFormat:@"not hooked (signature %s): %@", t, s]); continue; }
        if (args == 1) { char at[8] = ""; method_getArgumentType(m, 2, at, sizeof at); if (strcmp(at, "@")) { hlog([NSString stringWithFormat:@"not hooked (signature %s): %@", t, s]); continue; } }
        if (swizzle(m, args ? (IMP)hooked1 : (IMP)hooked0)) [done addObject:s];
    }
    hlog([NSString stringWithFormat:@"hooked -[XCUIApplication %@]", [done componentsJoinedByString:@"], -["]]);
}
static void onImage(const struct mach_header *mh, intptr_t slide) { if (!atomic_load(&hookedUI)) @autoreleasepool { @try { hookXCUI(); } @catch (NSException *x) { atomic_store(&hookedUI, true); fail(@"swizzling XCUIApplication"); } } }

// $SIMCAM_HOOK_DIR/<udid>.env: mode=… then KEY=VALUE lines
static NSDictionary *readHookEnv(NSString *dir, NSString **mode) {
    NSString *t = [NSString stringWithContentsOfFile:[dir stringByAppendingFormat:@"/%s.env", udid] encoding:NSUTF8StringEncoding error:nil];
    if (!t) return nil;
    NSMutableDictionary *d = [NSMutableDictionary new];
    for (NSString *l in [t componentsSeparatedByString:@"\n"]) {
        NSRange eq = [l rangeOfString:@"="]; if (!l.length || [l hasPrefix:@"#"] || eq.location == NSNotFound) continue;
        NSString *k = [l substringToIndex:eq.location], *v = [l substringFromIndex:eq.location + 1];
        if ([k isEqualToString:@"mode"]) *mode = v; else d[k] = v;
    }
    return d;
}

static void setup(void) {
    const char *on = getenv("SIMCAM"); if (on && on[0] == '0') return;
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    snprintf(udid, sizeof udid, "%s", [env[@"SIMULATOR_UDID"] UTF8String] ?: "unknown");
    NSString *dir = env[@"SIMCAM_HOOK_DIR"];
    if (dir.length) gfd = open([dir stringByAppendingFormat:@"/%s.log", udid].fileSystemRepresentation, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    char exe[1024]; uint32_t sz = sizeof exe; if (_NSGetExecutablePath(exe, &sz)) exe[0] = 0;
    BOOL xctest = env[@"XCTestConfigurationFilePath"] || env[@"XCTestBundlePath"] || env[@"XCTestSessionIdentifier"] || env[@"XCInjectBundleInto"];
    // a UI-test runner: Xcode names its bundle <test bundle id>.xctrunner; a hosted test's host is an XCTest process with
    // XCInjectBundleInto (the app's own bundle id, whatever its executable is called)
    NSString *bid = NSBundle.mainBundle.bundleIdentifier ?: @"";
    BOOL runner = [bid hasSuffix:@".xctrunner"] || (xctest && !env[@"XCInjectBundleInto"]);
    BOOL boot = mapped("libsimcamstub.dylib") != NULL;   // the boot env reached this process: it arms the app too
    hlog([NSString stringWithFormat:@"loaded in %s (%@, xctest=%d, stub mapped=%d)", exe, runner ? @"UI-test runner" : @"test host", xctest, boot]);
    if (getenv("SIMCAM_HOOK_DIAG")) {
        for (NSString *k in [env.allKeys sortedArrayUsingSelector:@selector(compare:)]) hlog([NSString stringWithFormat:@"diag: env %@=%@", k, env[k]]);
        for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) { const char *nm = _dyld_get_image_name(i); if (nm && !strstr(nm, "/System/Library/") && !strstr(nm, "/usr/lib/system/")) hlog([NSString stringWithFormat:@"diag: image %s", nm]); }
    }
    restoreRunner(env, xctest, runner);   // in boot mode too: the loader restored XCTest's list, which no longer names them
    if (!runner) { hlog(@"not a UI-test runner: nothing to hook"); return; }
    if (boot) { hlog(@"boot-armed (the stub is in this runner): inert"); return; }
    NSString *mode = nil; NSDictionary *v = dir.length ? readHookEnv(dir, &mode) : nil;
    if ([mode isEqualToString:@"boot"]) { hlog(@"boot-armed (per exec): inert"); return; }
    NSString *stub = v[@"DYLD_INSERT_LIBRARIES"];
    if (!v) {   // a destination exec couldn't name: a launchd-armed sim's own variables, if it has them
        NSString *sd = env[@"SIMCAM_SIM_DIR"], *ds = env[@"SIMCAM_HOOK_STUB"];
        if (!sd.length || !ds.length) { hlog(@"no stuntcam settings for this simulator (not armed, not a destination exec named): inert"); return; }
        NSMutableDictionary *m = [NSMutableDictionary new];
        for (NSString *k in env) if ([k hasPrefix:@"SIMCAM_"] && ![k hasPrefix:@"SIMCAM_HOOK"]) m[k] = env[k];
        for (NSString *k in @[@"M2_FAKE", @"M2_SESSION_HOOK", @"M2_NOLOGDICT", @"M2_PLIST", @"M5_IMAGE", @"M5_LOG"]) if (env[k]) m[k] = env[k];
        v = m; stub = ds; mode = @"launchd (runner env)";
    }
    if (!stub.length || ![NSFileManager.defaultManager fileExistsAtPath:stub]) { hlog([NSString stringWithFormat:@"no stub (%@): inert", stub ?: @"none"]); return; }
    NSMutableDictionary *vars = [v mutableCopy]; [vars removeObjectForKey:@"DYLD_INSERT_LIBRARIES"];
    gStub = stub; gVars = vars;
    hlog([NSString stringWithFormat:@"active for %s (%@): stub %@, %lu variables", udid, mode, stub, (unsigned long)vars.count]);
    _dyld_register_func_for_add_image(onImage);
}
__attribute__((constructor)) static void simcam_runnerhook(void) {
    @autoreleasepool { @try { setup(); } @catch (NSException *x) { gStub = nil; fail([NSString stringWithFormat:@"setup: %@: %@", x.name, x.reason]); } }
}
