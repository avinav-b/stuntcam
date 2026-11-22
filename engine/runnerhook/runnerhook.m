// SPDX-License-Identifier: Apache-2.0
// simcam runner hook (libsimcamrunnerhook.dylib).
//
// Who loads it: dyld, at launch. `stuntcam exec` inserts it into XCTest runner processes through
// TEST_RUNNER_DYLD_INSERT_LIBRARIES, for destinations that are launchd-armed or booted unarmed, where Xcode's
// per-launch insert list replaces the stub's. It never runs in a user app as an app: in a hosted test it runs in the
// test host only to put back what that variable displaced.
//
// What it provides: one constructor, simcam_runnerhook(), and no functions for other code. It does three things:
// 1. It restores what TEST_RUNNER_DYLD_INSERT_LIBRARIES displaced from the runner's own insert list. That variable
//    replaces XCTest's list. For hosted tests that drops libXCTestBundleInject, so the test bundle is never injected
//    and xcodebuild hangs, waiting for it. The list comes from the "test-runner" rule of the restore-rules file
//    ($SIMCAM_HOOK_RULES), else from the built-in copy below: <test host>/Frameworks/libXCTestBundleInject.dylib
//    (when the host has it and an XCTest marker is set), MTC (unless SIMCAM_MTC=0) and RPAC (if a PERFC_* variable is
//    set). Launch-only libraries (libclang_rt.*, libgmalloc) are never dlopened, nor is a library whose basename is
//    already mapped. This happens in the constructor, the same point in the launch where the boot-mode loader
//    restores.
// 2. In a UI-test runner (main bundle id *.xctrunner, or an XCTest process without XCInjectBundleInto), it makes
//    every app the test launches get stuntcam's stub prepended to its DYLD_INSERT_LIBRARIES and stuntcam's variables
//    added to its environment. Xcode 27.0's XCTest builds each launch's environment in -[XCUIApplication
//    _combinedLaunchEnvironment]: Xcode's target-app environment with the test's launchEnvironment over it, read by
//    launch, activate and launchWithoutAccessibility:. So that getter is swizzled and its result extended; the test's
//    launchEnvironment is never touched. Without that method (another XCTest), -launch and -activate (and
//    launchWithOptions:-style variants if present) are swizzled instead. They edit launchEnvironment around the call
//    and put it back afterwards. Either way, the list kept behind the stub is the test's own DYLD_INSERT_LIBRARIES if
//    it set one (it is never dropped), else Xcode's, with launch-only libraries first so that a sanitizer runtime
//    still initializes first. Variables the test set itself win. Apple's own apps (com.apple.*) are left alone.
// 3. It stays inert when the destination is boot-armed (the stub is mapped in this runner, because the boot env
//    already arms every app), when $SIMCAM_HOOK_DIR/<udid>.env says boot, or when there is nothing to add (no stub
//    for this simulator).
// What to add comes from $SIMCAM_HOOK_DIR/<SIMULATOR_UDID>.env: KEY=VALUE lines that exec writes for each destination,
// the first line "mode=launchd|unarmed|boot". On a launchd-armed simulator that exec could not name, it comes from the
// SIMCAM_* variables launchd gives this process, plus $SIMCAM_HOOK_STUB. The log is $SIMCAM_HOOK_DIR/<udid>.log, with
// one "armed <bundle> via <method>" line per launch.
// On any failure it logs one line, "failed (…)", which exec's summary repeats, and the launch goes ahead untouched. A
// hosted test whose libXCTestBundleInject can't be restored logs "failed (hosted tests can't inject their bundle …)".
// SIMCAM=0 turns it off. SIMCAM_HOOK_DIAG=1 also logs the environment, the mapped images and XCUIApplication's launch
// selectors.
//
// State: file-scope globals. Set in the constructor before any hook is installed, and only read afterwards: gLogFd,
// gUdid, gStub and gVars (the stub path and the variables to add; a nil gStub means inert). Shared by launches on any
// thread: the saved original IMPs (gOrig, gOrigCount; each slot is published before its swizzle and never changes
// afterwards), the atomics gFailedOnce, gHookedUI and gLaunchGen (a launch counter), and gLastArmedLine and
// gLastArmedGen (guarded by gLogLock). gLaunchDepth is per thread.
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

// the "@testhost/" path prefix of the test-runner rule (see testHostPath())
#define TESTHOST_PREFIX @"@testhost/"
enum { TESTHOST_PREFIX_LEN = 10 };
_Static_assert(sizeof "@testhost/" - 1 == TESTHOST_PREFIX_LEN, "TESTHOST_PREFIX_LEN is the length of TESTHOST_PREFIX");

enum {
    kUdidMax = 64,           // the simulator UDID buffer (a UDID is 36 characters)
    kExePathMax = 1024,      // this executable's path, for the "loaded in" line
    kTypeEncodingMax = 8,    // one return or argument type encoding ("v", "@")
    kHiddenArgCount = 2,     // self and _cmd: a method's first explicit argument has this index
};

static int gLogFd = -1;   // set once in the constructor, before any hook is installed
static char gUdid[kUdidMax];

static void hookLog(NSString *message) {
    if (gLogFd < 0) {
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    // seconds and microseconds
    NSString *line = [NSString stringWithFormat:@"%ld.%06ld [hook %d %s] %@\n", (long)now.tv_sec, now.tv_nsec / 1000,
                                               getpid(), getprogname(), message];
    const char *utf8 = line.UTF8String;
    if (utf8) {
        (void)write(gLogFd, utf8, strlen(utf8));   // a log line that can't be written is dropped
    }
}

static const char *baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// A mapped image with exactly this basename, else NULL.
static const char *mappedImage(const char *name) {
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; i++) {
        const char *imageName = _dyld_get_image_name(i);
        if (imageName && !strcmp(baseName(imageName), name)) {
            return imageName;
        }
    }
    return NULL;
}

// A launch-only library (a sanitizer runtime or Guard Malloc) is never dlopened. The loader's launch_only() makes the
// same test.
static BOOL launchOnly(NSString *path) {
    NSString *leaf = path.lastPathComponent;
    return [leaf hasPrefix:@"libclang_rt."] || [leaf isEqualToString:@"libgmalloc.dylib"];
}

// ---- 1. restore the runner's own list -------------------------------------------------------------------------------
// Tests a condition in the restore-rules "if"/"unless" syntax: VAR=value (equal), PREFIX* (any variable with that
// prefix) or VAR (set). The loader's cond_holds() (engine/loader/loader.c) matches the same syntax against environ; the
// two must stay in step.
static BOOL condHolds(NSString *condition, NSDictionary *env) {
    if (!condition.length) {
        return YES;
    }
    if ([condition hasSuffix:@"*"]) {
        NSString *prefix = [condition substringToIndex:condition.length - 1];
        for (NSString *name in env) {
            if ([name hasPrefix:prefix]) {
                return YES;
            }
        }
        return NO;
    }
    NSRange eq = [condition rangeOfString:@"="];
    if (eq.location == NSNotFound) {
        return env[condition] != nil;
    }
    return [env[[condition substringToIndex:eq.location]]
        isEqualToString:[condition substringFromIndex:eq.location + 1]];
}

// The built-in copy of restore-rules/27A266a.json's "test-runner" rule, used when $SIMCAM_HOOK_RULES has none. Keep it
// in step with that file, as the loader's builtin_xcode and builtin_ui are with its "xcode-run" and "ui-test-target"
// rules.
static NSArray *builtinRunnerLibs(void) {
    return @[@{@"path": @"@testhost/Frameworks/libXCTestBundleInject.dylib", @"if": @"XCInjectBundleInto"},
             @{@"path": @"/usr/lib/libMainThreadChecker.dylib", @"unless": @"SIMCAM_MTC=0"},
             @{@"path": @"/usr/lib/libRPAC.dylib", @"if": @"PERFC_*"}];
}

// Resolves @testhost/<rel>. XCTest names "<built products dir>/<App>.app/<rel>", but that file doesn't exist; dyld
// takes the leaf from DYLD_LIBRARY_PATH first (Xcode's platform usr/lib copy). So this does the same, then tries the
// built host, then the installed host.
static NSString *testHostPath(NSString *rel, NSDictionary *env) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *host = NSBundle.mainBundle.bundlePath ?: @"";
    for (NSString *dir in [env[@"DYLD_LIBRARY_PATH"] ?: @"" componentsSeparatedByString:@":"]) {
        NSString *path = [dir stringByAppendingPathComponent:rel.lastPathComponent];
        if (dir.length && [files fileExistsAtPath:path]) {
            return path;
        }
    }
    for (NSString *dir in [env[@"__XCODE_BUILT_PRODUCTS_DIR_PATHS"] ?: @"" componentsSeparatedByString:@":"]) {
        NSString *path =
            [[dir stringByAppendingPathComponent:host.lastPathComponent] stringByAppendingPathComponent:rel];
        if (dir.length && [files fileExistsAtPath:path]) {
            return path;
        }
    }
    return [host stringByAppendingPathComponent:rel];
}

// The rules file's "test-runner" rule, with a list of libs.
static BOOL isTestRunnerRule(id rule) {
    return [rule isKindOfClass:NSDictionary.class] && [rule[@"name"] isEqual:@"test-runner"] &&
           [rule[@"libs"] isKindOfClass:NSArray.class];
}

static NSArray *runnerLibs(NSDictionary *env) {
    NSString *rulesPath = env[@"SIMCAM_HOOK_RULES"];
    if (rulesPath.length) {
        NSData *data = [NSData dataWithContentsOfFile:rulesPath];
        id json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if ([json isKindOfClass:NSDictionary.class] && [json[@"rules"] isKindOfClass:NSArray.class]) {
            for (id rule in json[@"rules"]) {
                if (isTestRunnerRule(rule)) {
                    hookLog([NSString stringWithFormat:@"rules: %@", rulesPath]);
                    return rule[@"libs"];
                }
            }
        }
        hookLog([NSString stringWithFormat:@"rules: no test-runner rule in %@; built-in used", rulesPath]);
    }
    return builtinRunnerLibs();
}

// Restores one lib of the test-runner rule: checks its conditions, resolves its path (@testhost/ and the runtime root),
// then dlopens it unless it is absent, launch-only or already mapped.
static void restoreRunnerLib(id lib, NSDictionary *env, NSString *root, BOOL xctest, BOOL runner) {
    if (![lib isKindOfClass:NSDictionary.class] || ![lib[@"path"] isKindOfClass:NSString.class]) {
        return;
    }
    NSString *path = lib[@"path"];
    NSString *ifCond = [lib[@"if"] isKindOfClass:NSString.class] ? lib[@"if"] : nil;
    NSString *unlessCond = [lib[@"unless"] isKindOfClass:NSString.class] ? lib[@"unless"] : nil;
    if (!condHolds(ifCond, env) || (unlessCond.length && condHolds(unlessCond, env))) {
        return;
    }

    if ([path hasPrefix:TESTHOST_PREFIX]) {
        if (!xctest || runner) {
            return;   // a UI-test runner never had it
        }
        path = testHostPath([path substringFromIndex:TESTHOST_PREFIX_LEN], env);
    } else if ([path hasPrefix:@"/usr/"] || [path hasPrefix:@"/System/"]) {
        NSString *rooted = [root stringByAppendingString:path];
        if (root.length && [NSFileManager.defaultManager fileExistsAtPath:rooted]) {
            path = rooted;
        }
    }

    // a hosted test's bundle injector: without it the run hangs
    BOOL inject = [lib[@"path"] hasPrefix:TESTHOST_PREFIX];

    if (![NSFileManager.defaultManager fileExistsAtPath:path]) {
        if (inject) {
            hookLog([NSString
                stringWithFormat:@"failed (hosted tests can't inject their bundle; the run may hang): absent %@",
                                 path]);
        } else {
            hookLog([NSString stringWithFormat:@"restore: absent %@", path]);
        }
        return;
    }

    if (launchOnly(path)) {
        hookLog([NSString stringWithFormat:@"restore: skipped (must load at launch) %@", path]);
        return;
    }

    const char *loaded = mappedImage(baseName(path.fileSystemRepresentation));

    if (loaded) {
        hookLog([NSString stringWithFormat:@"restore: already loaded %s", loaded]);
        return;
    }

    void *handle = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL);

    if (!handle && inject) {
        hookLog([NSString stringWithFormat:@"failed (hosted tests can't inject their bundle; the run may hang): %@ %s",
                                           path, dlerror()]);
    } else {
        hookLog([NSString stringWithFormat:@"restore: %@ %s", path, handle ? "loaded" : dlerror()]);
    }
}

static void restoreRunner(NSDictionary *env, BOOL xctest, BOOL runner) {
    if ([env[@"SIMCAM_XCODE_LIBS"] isEqualToString:@"0"]) {
        hookLog(@"restore: SIMCAM_XCODE_LIBS=0");
        return;
    }
    NSString *root = env[@"SIMULATOR_ROOT"] ?: env[@"DYLD_ROOT_PATH"] ?: @"";
    for (id lib in runnerLibs(env)) {
        restoreRunnerLib(lib, env, root, xctest, runner);
    }
}

// ---- 2. the app under test ------------------------------------------------------------------------------------------
// gStub is our stub, or nil for inert. It and gVars are set in the constructor, before any swizzle, and only read
// afterwards.
static NSString *gStub;
static NSDictionary *gVars;   // SIMCAM_* and the like, for the app (never DYLD_INSERT_LIBRARIES)
static atomic_bool gFailedOnce;

// Logs a failure, once per process.
static void fail(NSString *what) {
    if (atomic_exchange(&gFailedOnce, true)) {
        return;
    }
    hookLog([NSString stringWithFormat:@"failed (launch left as it was): %@", what]);
}

// The original IMPs, one fixed slot per swizzled selector. Each slot is stored (and published) before its method is
// swizzled, and never changed afterwards, so a launch on any thread reads a complete entry without a lock.
enum { kMaxHooks = 8 };
static struct {
    SEL sel;
    _Atomic(IMP) orig;
} gOrig[kMaxHooks];
static atomic_int gOrigCount;

static IMP originalImp(SEL sel) {
    for (int i = 0, count = atomic_load(&gOrigCount); i < count; i++) {
        if (gOrig[i].sel == sel) {
            return atomic_load(&gOrig[i].orig);
        }
    }
    return NULL;
}

// Swizzles method to imp, saving its original first so that a concurrent call to imp finds it. NO if out of slots.
static BOOL swizzle(Method method, IMP imp) {
    int slot = atomic_load(&gOrigCount);
    if (slot >= kMaxHooks) {
        return NO;
    }
    gOrig[slot].sel = method_getName(method);
    atomic_store(&gOrig[slot].orig, method_getImplementation(method));
    atomic_store(&gOrigCount, slot + 1);   // published before the swizzle
    IMP replaced = method_setImplementation(method, imp);
    // Someone else changed the method between the save and the swizzle: call their IMP instead.
    if (replaced != atomic_load(&gOrig[slot].orig)) {
        atomic_store(&gOrig[slot].orig, replaced);
    }
    return YES;
}

// What Xcode itself puts in the app's DYLD_INSERT_LIBRARIES (UITargetAppEnvironmentVariables), as the runner was told.
static NSString *xcodeTargetInsert(void) {
    @try {
        Class configClass = NSClassFromString(@"XCTestConfiguration");
        if (![configClass respondsToSelector:@selector(activeTestConfiguration)]) {
            return nil;
        }
        id config = ((id (*)(id, SEL))objc_msgSend)(configClass, @selector(activeTestConfiguration));
        if (!config || ![config respondsToSelector:@selector(targetApplicationEnvironment)]) {
            return nil;
        }
        NSDictionary *targetEnv = ((id (*)(id, SEL))objc_msgSend)(config, @selector(targetApplicationEnvironment));
        id value = [targetEnv isKindOfClass:NSDictionary.class] ? targetEnv[@"DYLD_INSERT_LIBRARIES"] : nil;
        return [value isKindOfClass:NSString.class] ? value : nil;
    } @catch (NSException *exception) {
        return nil;
    }
}

static BOOL isOurLibrary(NSString *path) {
    return [path.lastPathComponent hasPrefix:@"libsimcam"];
}

// The stub followed by baseList: launch-only libraries first, no duplicates, and no other copy of our libraries.
static NSString *mergedInsertList(NSString *baseList) {
    NSMutableArray *first = [NSMutableArray new];
    NSMutableArray *rest = [NSMutableArray new];
    for (NSString *entry in [baseList componentsSeparatedByString:@":"]) {
        if (!entry.length || isOurLibrary(entry)) {
            continue;
        }
        if ([first containsObject:entry] || [rest containsObject:entry]) {
            continue;   // a duplicate
        }
        [(launchOnly(entry) ? first : rest) addObject:entry];
    }
    [first addObject:gStub];
    [first addObjectsFromArray:rest];
    return [first componentsJoinedByString:@":"];
}

static NSString *bundleOf(id app) {
    @try {
        for (NSString *key in @[@"bundleID", @"bundleIdentifier"]) {
            if ([app respondsToSelector:NSSelectorFromString(key)]) {
                id value = [app valueForKey:key];
                if ([value isKindOfClass:NSString.class]) {
                    return value;
                }
            }
        }
    } @catch (NSException *exception) {
    }
    return nil;
}

// Adds ours to the app's launchEnvironment. Returns the launchEnvironment to put back afterwards, or nil if nothing
// was changed.
static NSDictionary *prepareLaunchEnvironment(id app, const char *how) {
    if (!gStub) {
        return nil;
    }

    @try {
        NSString *bundleId = bundleOf(app);
        if ([bundleId hasPrefix:@"com.apple."]) {
            return nil;
        }
        NSDictionary *original = [app valueForKey:@"launchEnvironment"];
        if (original && ![original isKindOfClass:NSDictionary.class]) {
            fail(@"launchEnvironment is not a dictionary");
            return nil;
        }
        NSMutableDictionary *env = original ? [original mutableCopy] : [NSMutableDictionary new];
        NSString *ownList = env[@"DYLD_INSERT_LIBRARIES"];
        NSString *baseList = ownList.length ? ownList : (xcodeTargetInsert() ?: @"");
        env[@"DYLD_INSERT_LIBRARIES"] = mergedInsertList(baseList);
        for (NSString *key in gVars) {
            if (!env[key]) {
                env[key] = gVars[key];
            }
        }
        [app setValue:env forKey:@"launchEnvironment"];
        NSString *listNote;   // where the list kept behind the stub came from
        if (ownList.length) {
            listNote = @"merged with the test's own";
        } else if (baseList.length) {
            listNote = @"merged with Xcode's list";
        } else {
            listNote = @"Xcode's list unknown";
        }
        hookLog([NSString stringWithFormat:@"armed %@ via %s: DYLD_INSERT_LIBRARIES=%@ (%@)", bundleId ?: @"?", how,
                                           env[@"DYLD_INSERT_LIBRARIES"], listNote]);
        return original ?: @{};
    } @catch (NSException *exception) {
        fail([NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
        return nil;
    }
}

static void putBackLaunchEnvironment(id app, NSDictionary *original) {
    if (!original) {
        return;
    }
    @try {
        [app setValue:original forKey:@"launchEnvironment"];
    } @catch (NSException *exception) {
        fail(@"restoring launchEnvironment");
    }
}

// -[XCUIApplication _combinedLaunchEnvironment] (Xcode 27.0's XCTest) returns Xcode's target-app environment with the
// test's launchEnvironment over it, and every launch path reads it (launch, activate, launchWithoutAccessibility:).
// Ours goes on top.
static NSString *gLastArmedLine;   // guarded by gLogLock
static unsigned gLastArmedGen;     // guarded by gLogLock
static atomic_uint gLaunchGen;     // bumped by each outermost launch/activate call: one "armed" line per launch
static os_unfair_lock gLogLock = OS_UNFAIR_LOCK_INIT;

static id hookedCombined(id self, SEL _cmd) {
    IMP original = originalImp(_cmd);
    if (!original) {
        return nil;   // cannot happen: it is saved before the swizzle
    }
    id result = ((id (*)(id, SEL))original)(self, _cmd);
    if (!gStub) {
        return result;
    }

    @try {
        if (![result isKindOfClass:NSDictionary.class]) {
            fail(@"_combinedLaunchEnvironment is not a dictionary");
            return result;
        }
        NSString *bundleId = bundleOf(self);
        if ([bundleId hasPrefix:@"com.apple."]) {
            return result;
        }
        NSMutableDictionary *env = [result mutableCopy];
        NSString *combinedList = env[@"DYLD_INSERT_LIBRARIES"] ?: @"";
        NSString *ownList = [[self valueForKey:@"launchEnvironment"] isKindOfClass:NSDictionary.class]
                                ? [self valueForKey:@"launchEnvironment"][@"DYLD_INSERT_LIBRARIES"]
                                : nil;
        env[@"DYLD_INSERT_LIBRARIES"] = mergedInsertList(combinedList);
        NSDictionary *testEnv = [self valueForKey:@"launchEnvironment"];
        for (NSString *key in gVars) {
            if (![testEnv isKindOfClass:NSDictionary.class] || !testEnv[key]) {
                env[key] = gVars[key];
            }
        }
        NSString *listNote;   // where the list kept behind the stub came from
        if (ownList.length) {
            listNote = @"merged with the test's own";
        } else if (combinedList.length) {
            listNote = @"merged with Xcode's list";
        } else {
            listNote = @"no other list";
        }
        NSString *line = [NSString
            stringWithFormat:@"armed %@ via _combinedLaunchEnvironment: DYLD_INSERT_LIBRARIES=%@ (%@)",
                             bundleId ?: @"?", env[@"DYLD_INSERT_LIBRARIES"], listNote];
        unsigned gen = atomic_load(&gLaunchGen);
        BOOL shouldLog = NO;
        os_unfair_lock_lock(&gLogLock);
        if (gen != gLastArmedGen || ![line isEqualToString:gLastArmedLine]) {
            shouldLog = YES;
            gLastArmedLine = line;
            gLastArmedGen = gen;
        }
        os_unfair_lock_unlock(&gLogLock);
        if (shouldLog) {
            hookLog(line);
        }
        return env;
    } @catch (NSException *exception) {
        fail([NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]);
        return result;
    }
}

// How deep this thread is in hooked launch calls. launch calling activate (or an internal call) prepares once, at the
// outermost call.
static __thread int gLaunchDepth;

static void hookedLaunch(id self, SEL _cmd) {
    IMP original = originalImp(_cmd);
    NSDictionary *saved = gLaunchDepth++ ? nil : prepareLaunchEnvironment(self, sel_getName(_cmd));
    @try {
        ((void (*)(id, SEL))original)(self, _cmd);
    } @finally {
        gLaunchDepth--;
        putBackLaunchEnvironment(self, saved);
    }
}

static void hookedLaunchWithObject(id self, SEL _cmd, id arg) {
    IMP original = originalImp(_cmd);
    NSDictionary *saved = gLaunchDepth++ ? nil : prepareLaunchEnvironment(self, sel_getName(_cmd));
    @try {
        ((void (*)(id, SEL, id))original)(self, _cmd, arg);
    } @finally {
        gLaunchDepth--;
        putBackLaunchEnvironment(self, saved);
    }
}

// With _combinedLaunchEnvironment hooked, the launch methods only count launches (for the log); they don't touch the
// environment.
static void countedLaunch(id self, SEL _cmd) {
    if (!gLaunchDepth++) {
        atomic_fetch_add(&gLaunchGen, 1);
    }
    @try {
        ((void (*)(id, SEL))originalImp(_cmd))(self, _cmd);
    } @finally {
        gLaunchDepth--;
    }
}

static void countedLaunchWithBool(id self, SEL _cmd, BOOL flag) {
    if (!gLaunchDepth++) {
        atomic_fetch_add(&gLaunchGen, 1);
    }
    @try {
        ((void (*)(id, SEL, BOOL))originalImp(_cmd))(self, _cmd, flag);
    } @finally {
        gLaunchDepth--;
    }
}

// SIMCAM_HOOK_DIAG: logs XCUIApplication's launch and activate selectors with their type encodings.
static void logLaunchSelectors(Class cls) {
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    NSMutableArray *names = [NSMutableArray new];
    for (unsigned i = 0; i < count; i++) {
        NSString *name = NSStringFromSelector(method_getName(methods[i]));
        if ([name.lowercaseString containsString:@"launch"] || [name.lowercaseString containsString:@"activate"]) {
            [names addObject:[NSString stringWithFormat:@"%@ %s", name, method_getTypeEncoding(methods[i])]];
        }
    }
    free(methods);
    hookLog([NSString stringWithFormat:@"diag: XCUIApplication selectors: %@", [names componentsJoinedByString:@", "]]);
}

// The counting replacement for a launch method with these type encodings: -(void)m, or -(void)m:(BOOL)flag on a
// 64-bit target. NULL for any other method, which is left alone.
static IMP countingImpFor(const char *types) {
    if (!types) {
        return NULL;
    }
    if (!strcmp(types, "v16@0:8")) {
        return (IMP)countedLaunch;
    }
    if (!strcmp(types, "v20@0:8B16")) {
        return (IMP)countedLaunchWithBool;
    }
    return NULL;
}

// Xcode 27.0's XCTest: _combinedLaunchEnvironment carries ours. launch, activate and launchWithoutAccessibility: only
// count launches (for the log).
static void hookCombinedLaunchEnvironment(Class cls, Method combined) {
    swizzle(combined, (IMP)hookedCombined);
    for (NSString *name in @[@"launch", @"activate", @"launchWithoutAccessibility:"]) {
        Method method = class_getInstanceMethod(cls, NSSelectorFromString(name));
        const char *types = method ? method_getTypeEncoding(method) : NULL;
        IMP imp = countingImpFor(types);
        if (imp) {
            swizzle(method, imp);
        }
    }
    hookLog(@"hooked -[XCUIApplication _combinedLaunchEnvironment]");
}

// Without _combinedLaunchEnvironment (another XCTest): the public launch methods, with the test's launchEnvironment
// edited around the call. Only void methods with no argument or one object argument are hooked; anything else this
// XCTest has is left alone.
static void hookPublicLaunchMethods(Class cls) {
    NSMutableArray *done = [NSMutableArray new];
    for (NSString *name in
         @[@"launch", @"activate", @"launchWithOptions:", @"launchWithConfiguration:", @"activateWithOptions:"]) {
        Method method = class_getInstanceMethod(cls, NSSelectorFromString(name));
        if (!method) {
            continue;
        }
        const char *types = method_getTypeEncoding(method);
        int argCount = (int)method_getNumberOfArguments(method) - kHiddenArgCount;
        char returnType[kTypeEncodingMax] = "";
        method_getReturnType(method, returnType, sizeof returnType);
        if (strcmp(returnType, "v") || argCount > 1) {
            hookLog([NSString stringWithFormat:@"not hooked (signature %s): %@", types, name]);
            continue;
        }
        if (argCount == 1) {
            char argType[kTypeEncodingMax] = "";
            method_getArgumentType(method, kHiddenArgCount, argType, sizeof argType);
            if (strcmp(argType, "@")) {
                hookLog([NSString stringWithFormat:@"not hooked (signature %s): %@", types, name]);
                continue;
            }
        }
        if (swizzle(method, argCount ? (IMP)hookedLaunchWithObject : (IMP)hookedLaunch)) {
            [done addObject:name];
        }
    }

    hookLog([NSString stringWithFormat:@"hooked -[XCUIApplication %@]", [done componentsJoinedByString:@"], -["]]);
}

static atomic_bool gHookedUI;

static void hookXCUI(void) {
    if (atomic_load(&gHookedUI)) {
        return;
    }
    Class cls = objc_getClass("XCUIApplication");
    if (!cls) {
        return;
    }
    static atomic_int hookClaimed;   // add-image callbacks can come from any thread
    int expected = 0;
    if (!atomic_compare_exchange_strong(&hookClaimed, &expected, 1)) {
        return;
    }
    atomic_store(&gHookedUI, true);
    if (getenv("SIMCAM_HOOK_DIAG")) {
        logLaunchSelectors(cls);
    }
    Method combined = class_getInstanceMethod(cls, NSSelectorFromString(@"_combinedLaunchEnvironment"));
    if (combined && !getenv("SIMCAM_HOOK_PUBLIC") && !strcmp(method_getTypeEncoding(combined) ?: "", "@16@0:8")) {
        hookCombinedLaunchEnvironment(cls, combined);
        return;
    }
    hookPublicLaunchMethods(cls);
}

static void onImageAdded(const struct mach_header *header, intptr_t slide) {
    if (atomic_load(&gHookedUI)) {
        return;
    }
    @autoreleasepool {
        @try {
            hookXCUI();
        } @catch (NSException *exception) {
            atomic_store(&gHookedUI, true);
            fail(@"swizzling XCUIApplication");
        }
    }
}

// Reads $SIMCAM_HOOK_DIR/<udid>.env: a mode=… line, then KEY=VALUE lines.
static NSDictionary *readHookEnv(NSString *dir, NSString **mode) {
    NSString *text = [NSString stringWithContentsOfFile:[dir stringByAppendingFormat:@"/%s.env", gUdid]
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    if (!text) {
        return nil;
    }
    NSMutableDictionary *vars = [NSMutableDictionary new];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (!line.length || [line hasPrefix:@"#"] || eq.location == NSNotFound) {
            continue;
        }
        NSString *key = [line substringToIndex:eq.location];
        NSString *value = [line substringFromIndex:eq.location + 1];
        if ([key isEqualToString:@"mode"]) {
            *mode = value;
        } else {
            vars[key] = value;
        }
    }
    return vars;
}

// SIMCAM_HOOK_DIAG: logs the environment and the mapped images (outside the OS's own directories).
static void logEnvAndImages(NSDictionary *env) {
    for (NSString *key in [env.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        hookLog([NSString stringWithFormat:@"diag: env %@=%@", key, env[key]]);
    }
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; i++) {
        const char *imageName = _dyld_get_image_name(i);
        if (imageName && !strstr(imageName, "/System/Library/") && !strstr(imageName, "/usr/lib/system/")) {
            hookLog([NSString stringWithFormat:@"diag: image %s", imageName]);
        }
    }
}

// On a launchd-armed simulator: the variables launchd gave this runner for the app. That is SIMCAM_* except
// SIMCAM_HOOK*, plus the six variables the CLI sets when arming under their stuntcam 1.1.0 names too: a simulator armed
// by 1.1.0 has only those, and the engine reads both (core/env.inc). Returns nil unless both SIMCAM_SIM_DIR and
// SIMCAM_HOOK_STUB are set.
static NSDictionary *launchdRunnerVars(NSDictionary *env) {
    NSString *simDir = env[@"SIMCAM_SIM_DIR"];
    NSString *stub = env[@"SIMCAM_HOOK_STUB"];
    if (!simDir.length || !stub.length) {
        return nil;
    }
    NSMutableDictionary *vars = [NSMutableDictionary new];
    for (NSString *key in env) {
        if ([key hasPrefix:@"SIMCAM_"] && ![key hasPrefix:@"SIMCAM_HOOK"]) {
            vars[key] = env[key];
        }
    }
    for (NSString *key in @[@"SIMCAM_FIG_SOURCES", @"SIMCAM_FIG_SESSION", @"SIMCAM_OFF_KEY_LOG", @"SIMCAM_PROFILE",
                            @"SIMCAM_FIXTURE_IMAGE", @"SIMCAM_ENGINE_LOG",
                            @"M2_FAKE", @"M2_SESSION_HOOK", @"M2_NOLOGDICT", @"M2_PLIST", @"M5_IMAGE", @"M5_LOG"]) {
        if (env[key]) {
            vars[key] = env[key];
        }
    }
    return vars;
}

static void setup(void) {
    const char *enabled = getenv("SIMCAM");
    if (enabled && enabled[0] == '0') {
        return;
    }
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    snprintf(gUdid, sizeof gUdid, "%s", [env[@"SIMULATOR_UDID"] UTF8String] ?: "unknown");
    NSString *dir = env[@"SIMCAM_HOOK_DIR"];

    if (dir.length) {
        gLogFd = open([dir stringByAppendingFormat:@"/%s.log", gUdid].fileSystemRepresentation,
                      O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    }

    char exe[kExePathMax];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size)) {
        exe[0] = 0;
    }
    BOOL xctest = env[@"XCTestConfigurationFilePath"] || env[@"XCTestBundlePath"] || env[@"XCTestSessionIdentifier"] ||
                  env[@"XCInjectBundleInto"];

    // A UI-test runner: Xcode names its bundle <test bundle id>.xctrunner. A hosted test's host is an XCTest process
    // with XCInjectBundleInto (the app's own bundle id, whatever its executable is called).
    NSString *bundleId = NSBundle.mainBundle.bundleIdentifier ?: @"";
    BOOL runner = [bundleId hasSuffix:@".xctrunner"] || (xctest && !env[@"XCInjectBundleInto"]);

    // The boot env reached this process, so it arms the app too.
    BOOL boot = mappedImage("libsimcamstub.dylib") != NULL;
    hookLog([NSString stringWithFormat:@"loaded in %s (%@, xctest=%d, stub mapped=%d)", exe,
                                       runner ? @"UI-test runner" : @"test host", xctest, boot]);
    if (getenv("SIMCAM_HOOK_DIAG")) {
        logEnvAndImages(env);
    }

    // In boot mode too: the loader restored XCTest's list, which no longer names these libraries.
    restoreRunner(env, xctest, runner);

    if (!runner) {
        hookLog(@"not a UI-test runner: nothing to hook");
        return;
    }

    if (boot) {
        hookLog(@"boot-armed (the stub is in this runner): inert");
        return;
    }

    NSString *mode = nil;
    NSDictionary *settings = dir.length ? readHookEnv(dir, &mode) : nil;

    if ([mode isEqualToString:@"boot"]) {
        hookLog(@"boot-armed (per exec): inert");
        return;
    }

    NSString *stub = settings[@"DYLD_INSERT_LIBRARIES"];

    if (!settings) {   // a destination exec couldn't name: a launchd-armed simulator's own variables, if it has them
        settings = launchdRunnerVars(env);
        if (!settings) {
            hookLog(@"no stuntcam settings for this simulator (not armed, not a destination exec named): inert");
            return;
        }
        stub = env[@"SIMCAM_HOOK_STUB"];
        mode = @"launchd (runner env)";
    }

    if (!stub.length || ![NSFileManager.defaultManager fileExistsAtPath:stub]) {
        hookLog([NSString stringWithFormat:@"no stub (%@): inert", stub ?: @"none"]);
        return;
    }

    NSMutableDictionary *vars = [settings mutableCopy];
    [vars removeObjectForKey:@"DYLD_INSERT_LIBRARIES"];
    gStub = stub;
    gVars = vars;
    hookLog([NSString stringWithFormat:@"active for %s (%@): stub %@, %lu variables", gUdid, mode, stub,
                                       (unsigned long)vars.count]);
    _dyld_register_func_for_add_image(onImageAdded);
}

__attribute__((constructor)) static void simcam_runnerhook(void) {
    @autoreleasepool {
        @try {
            setup();
        } @catch (NSException *exception) {
            gStub = nil;
            fail([NSString stringWithFormat:@"setup: %@: %@", exception.name, exception.reason]);
        }
    }
}
