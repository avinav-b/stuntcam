// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/crash_handler.inc): the sentinel's
// load-time state and its markers (latched-<build>, latched-motion-<build>, preflight-fail-<build>): paths, the
// load-time stat()s, writes and prunes.
//
// Provides: SentinelLoadInit, SentinelLatchMarkerValidAtLoad, SentinelMotionStockAtLoad, SentinelClearAtLoad,
//           SentinelPreflightMarker (core/lazy.inc)
//           SentinelCountStrikes, SentinelMarkersUpdate (core/sentinel_activate.inc)
//           SentinelMkdirP (core/events_file.inc, core/sentinel_activate.inc, core/trace.inc)
// Owns:     nothing shared (it sets the sentinel's load-time fields, core/sentinel.h, at load).
// Uses:     core/act.inc, core/safety.inc, core/crash_handler.inc
#include "sentinel.h"
#include "activation.h"

// ---- load-time state and markers ----
// A marker's mtime is its until. A marker past its until is ignored, and the next PREPARE deletes it.
//   <state>/<bundle>/latched-<build>         the latched camera features. Read at load: the camera starts STOCK, with
//                                            no PREPARE, and CoreMotion is stock.
//   <state>/<bundle>/latched-motion-<build>  `motion`. Read at load by motion only; the camera is unaffected.
//   <state>/preflight-fail-<build>           a real preflight failure, valid for 24 h. Read at load by motion only
//                                            (the camera always runs its own preflight); a passing preflight unlinks
//                                            it.
// Every location exists twice: under SIMCAM_STATE_DIR and under the container-tmp fallback (for iOS 17.5, where the
// state dir can't be written).
// The modes, the two locations (kStateDir*) and the paths in each are declared in core/sentinel.h, in gSentinel.paths;
// what the load-time stat()s found is in gSentinel.atLoad.
#define kSentinelMarkerTempPathBytes 1300    // a marker's temp file: its directory plus ".<name>.tmp-<pid>"
#define kSentinelLatchedPrefixLen 8          // strlen("latched-")
#define kSentinelLatchedMotionPrefixLen 15   // strlen("latched-motion-")
#define kSentinelPreflightFailPrefixLen 15   // strlen("preflight-fail-")
#define kSentinelStaleTempSeconds 60         // a marker temp file this old was left by a process that died mid-write

// the sentinel's off switches and test faults, read once at load (SentinelLoadInit)
static void SentinelParseTestEnv(void) {
    gOffHandlerMotionMarker = getenv(ENV_OFF_HANDLER_MOTION_MARKER) != NULL;
    gOffMarkerRename = getenv(ENV_OFF_MARKER_RENAME) != NULL;
    const char *slowMarker = getenv(ENV_TEST_SLOW_MARKER_MS);
    if (slowMarker) {
        gTestSlowMarkerMs = atol(slowMarker);
    }
    const char *prunePause = getenv(ENV_TEST_PRUNE_PAUSE_MS);
    if (prunePause) {
        gTestPrunePauseMs = atol(prunePause);
    }
}

static int SentinelMkdirP(const char *path) {
    char partial[kSentinelDirPathBytes];
    snprintf(partial, sizeof partial, "%s", path);
    for (char *cursor = partial + 1; *cursor; cursor++) {
        if (*cursor == '/') {
            *cursor = 0;
            (void)mkdir(partial, 0755);   // usually there already; a real failure shows when the file is opened
            *cursor = '/';
        }
    }
    return (mkdir(partial, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static void SentinelTmpRoot(char *out, size_t cap) {   // <container tmp>/simcam-state
    const char *tmpDir = getenv("TMPDIR");
    char root[kSentinelDirPathBytes];
    if (tmpDir && *tmpDir) {
        snprintf(root, sizeof root, "%s", tmpDir);
    } else {
        snprintf(root, sizeof root, "%s", NSTemporaryDirectory().fileSystemRepresentation);
    }
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') {
        root[--len] = 0;
    }
    snprintf(out, cap, "%s/simcam-state", root);
}

// at load: no I/O apart from the marker stat()s (and the explicit clear below)
static void SentinelLoadInit(void) {
    // SIMCAM_SENTINEL=0 turns off the ops, the watchdog and the sentinel; SIMCAM_SENTINEL=clear appends a clear line,
    // which forgets the past (core/sentinel_activate.inc writes it)
    const char *mode = getenv(ENV_SENTINEL);
    if (mode && mode[0] == '0') {
        gSentinel.mode = kSentinelModeOff;
    } else if (mode && !strcmp(mode, "clear")) {
        gSentinel.mode = kSentinelModeClear;
    } else {
        gSentinel.mode = kSentinelModeOn;
    }
    // the sanitizers slow everything down: op timeouts are ten times longer under them
    gSentinel.scale = (dlsym(RTLD_DEFAULT, "__tsan_init") || dlsym(RTLD_DEFAULT, "__asan_init")) ? 10 : 1;
    Dl_info selfInfo;

    // the build is the name of the content-hash directory our dylib is in
    if (dladdr((const void *)SentinelLoadInit, &selfInfo) && selfInfo.dli_fname) {
        const char *dirEnd = strrchr(selfInfo.dli_fname, '/');
        if (dirEnd) {
            const char *dirStart = dirEnd;
            while (dirStart > selfInfo.dli_fname && dirStart[-1] != '/') {
                dirStart--;
            }
            snprintf(gSentinel.build, sizeof gSentinel.build, "%.*s", (int)(dirEnd - dirStart), dirStart);
        }
    }

    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFStringRef bundleID = mainBundle ? CFBundleGetIdentifier(mainBundle) : NULL;
    if (bundleID) {
        CFStringGetCString(bundleID, gSentinel.bundle, sizeof gSentinel.bundle, kCFStringEncodingUTF8);
    }
    const char *stateDir = getenv(ENV_STATE_DIR);
    char tmpRoot[kSentinelDirPathBytes];
    SentinelTmpRoot(tmpRoot, sizeof tmpRoot);

    if (stateDir && *stateDir) {
        snprintf(gSentinel.paths.eventsDirs[kStateDirState], sizeof gSentinel.paths.eventsDirs[kStateDirState], "%s/%s",
                 stateDir, gSentinel.bundle);
        snprintf(gSentinel.paths.preflightFail[kStateDirState], sizeof gSentinel.paths.preflightFail[kStateDirState],
                 "%s/preflight-fail-%s", stateDir, gSentinel.build);
    }

    snprintf(gSentinel.paths.eventsDirs[kStateDirTmp], sizeof gSentinel.paths.eventsDirs[kStateDirTmp], "%s/%s",
             tmpRoot, gSentinel.bundle);
    snprintf(gSentinel.paths.preflightFail[kStateDirTmp], sizeof gSentinel.paths.preflightFail[kStateDirTmp],
             "%s/preflight-fail-%s", tmpRoot, gSentinel.build);
    for (int i = 0; i < kStateDirCount; i++) {
        if (gSentinel.paths.eventsDirs[i][0]) {
            snprintf(gSentinel.paths.latchMarker[i], sizeof gSentinel.paths.latchMarker[i], "%s/latched-%s",
                     gSentinel.paths.eventsDirs[i], gSentinel.build);
            snprintf(gSentinel.paths.motionMarker[i], sizeof gSentinel.paths.motionMarker[i], "%s/latched-motion-%s",
                     gSentinel.paths.eventsDirs[i], gSentinel.build);
        }
    }

    SentinelParseTestEnv();
}

static BOOL SentinelLatchMarkerValidAtLoad(void) {
    if (gSentinel.mode != kSentinelModeOn) {
        return NO;
    }
    time_t now = time(NULL);
    for (int i = 0; i < kStateDirCount; i++) {
        struct stat st;
        if (gSentinel.paths.latchMarker[i][0] && stat(gSentinel.paths.latchMarker[i], &st) == 0 && st.st_mtime > now) {
            snprintf(gSentinel.atLoad.latchMarker, sizeof gSentinel.atLoad.latchMarker, "%s",
                     gSentinel.paths.latchMarker[i]);
            return YES;
        }
    }
    return NO;
}

// CoreMotion is decided once, at load, from markers only. Any valid one of this build's latched-<build> (a camera
// latch), latched-motion-<build> or preflight-fail-<build>, in either location, with the sentinel on and no clear,
// makes CoreMotion stock for the process. Markers this process writes later can't change it (a camera activation that
// fails in this launch keeps the stand-in for the launch). SIMCAM_OFF_MOTION_MARKERS=1 ignores them.
static BOOL SentinelMotionStockAtLoad(void) {
    if (gSentinel.mode != kSentinelModeOn || !MotionMarkersOn()) {
        return NO;
    }
    time_t now = time(NULL);

    // two of each kind, one per location: candidates 0-1 are camera latches, 2-3 motion latches, 4-5 preflight failures
    const char *candidates[6] = {gSentinel.paths.latchMarker[0],   gSentinel.paths.latchMarker[1],
                                 gSentinel.paths.motionMarker[0],  gSentinel.paths.motionMarker[1],
                                 gSentinel.paths.preflightFail[0], gSentinel.paths.preflightFail[1]};
    for (int i = 0; i < 6; i++) {
        struct stat st;
        if (candidates[i][0] && stat(candidates[i], &st) == 0 && st.st_mtime > now) {
            snprintf(gSentinel.atLoad.motionStockMarker, sizeof gSentinel.atLoad.motionStockMarker, "%s",
                     candidates[i]);
            // kept for the stock line: by the first motion call, a passing preflight or the sentinel step may have
            // unlinked the marker
            gSentinel.atLoad.motionStockUntil = st.st_mtime;
            gSentinel.atLoad.motionStockInode = st.st_ino;
            if (i < 2) {
                gSentinel.atLoad.motionStockKind = "camera";
            } else if (i < 4) {
                gSentinel.atLoad.motionStockKind = "motion";
            } else {
                gSentinel.atLoad.motionStockKind = "preflight failed";
            }
            return YES;
        }
    }

    return NO;
}

// unlinks every latched-* marker in dir, except keepBuild's when keepBuild is set
static void SentinelUnlinkLatchMarkers(const char *dir, const char *keepBuild) {
    DIR *dirStream = opendir(dir);
    if (!dirStream) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dirStream))) {
        if (strncmp(entry->d_name, "latched-", kSentinelLatchedPrefixLen)) {
            continue;
        }
        if (keepBuild && !strcmp(entry->d_name + kSentinelLatchedPrefixLen, keepBuild)) {
            continue;
        }
        char path[kSentinelEntryPathBytes];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        unlink(path);
    }
    closedir(dirStream);
}

// SIMCAM_SENTINEL=clear at load is an explicit user action, so I/O at load is acceptable. It appends the clear line
// (also for an app that never runs PREPARE, such as a motion-only app) and unlinks the bundle's markers and this
// build's preflight-fail marker.
static void SentinelClearAtLoad(void) {
    if (gSentinel.mode != kSentinelModeClear) {
        return;
    }

    for (int i = 0; i < kStateDirCount; i++) {
        if (!gSentinel.paths.eventsDirs[i][0] || SentinelMkdirP(gSentinel.paths.eventsDirs[i])) {
            continue;
        }
        char path[kSentinelPathBytes];
        snprintf(path, sizeof path, "%s/events.log", gSentinel.paths.eventsDirs[i]);
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            continue;
        }
        char line[kSentinelEventLineBytes];
        int len = SentinelFormatEvent(line, sizeof line, 0, "clear", "-", "SIMCAM_SENTINEL=clear", -1);
        // Best effort: a failed write loses the clear line (the sentinel step then reads the older records). The
        // markers are unlinked below either way.
        (void)write(fd, line, len);
        close(fd);
        gSentinel.atLoad.cleared = YES;
        break;
    }

    for (int i = 0; i < kStateDirCount; i++) {
        if (gSentinel.paths.eventsDirs[i][0]) {
            SentinelUnlinkLatchMarkers(gSentinel.paths.eventsDirs[i], NULL);
        }
        if (gSentinel.paths.preflightFail[i][0]) {
            unlink(gSentinel.paths.preflightFail[i]);
        }
    }
}

// A marker is written to "<dir>/.<name>.tmp-<pid>", its mtime is set to the until, and then it is renamed into place.
// Written in place, it would exist with mtime = now between open(O_TRUNC) and futimens, and another process's prune
// (the sentinel step's st_mtime <= now) could unlink it. Temp names start with '.', so no prune, load-time stat or CLI
// listing matches them. SIMCAM_OFF_MARKER_RENAME=1 writes in place. SIMCAM_TEST_SLOW_MARKER_MS=<n> pauses n ms after
// the content is written, before the until is set (the pf-prune-race test widens the window with it).
static void SentinelSlowMarker(const char *path) {
    if (gTestSlowMarkerMs <= 0) {
        return;
    }
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_SLOW_MARKER_MS: marker write paused %ld ms (%s)\n", getpid(),
            gTestSlowMarkerMs, path);
    SlicedSleepMs(gTestSlowMarkerMs);
}

static BOOL SentinelWriteMarker(const char *path, const char *content, time_t until) {
    char dir[kSentinelPathBytes];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash) {
        *slash = 0;
        SentinelMkdirP(dir);
    }

    char tmp[kSentinelMarkerTempPathBytes];
    const char *base = slash ? slash + 1 : path;
    if (gOffMarkerRename) {
        snprintf(tmp, sizeof tmp, "%s", path);
    } else {
        snprintf(tmp, sizeof tmp, "%s/.%s.tmp-%d", slash ? dir : ".", base, getpid());
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return NO;
    }

    // A failed write is not a failed marker: the marker latches by its name and mtime (set below). Its content is what
    // `stuntcam sentinel` and `status` show (the CLI derives the latch's effect from it), so a short write only
    // degrades that report.
    (void)write(fd, content, strlen(content));
    SentinelSlowMarker(path);
    struct timespec ts[2] = {{until, 0}, {until, 0}};
    // a marker whose until couldn't be set reads as expired: the latch is lost early, never held too long
    (void)futimens(fd, ts);
    close(fd);
    if (gOffMarkerRename) {
        return YES;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return NO;
    }

    return YES;
}

// A temp file left by a process that died mid-write is removed once it is a minute old. Its mtime is the until, so its
// age is read from ctime.
static void SentinelPruneTmp(const char *dir, const char *name, long now) {
    if (name[0] != '.' || !strstr(name, ".tmp-")) {
        return;   // not a temp file
    }
    BOOL latchedTemp = !strncmp(name + 1, "latched-", kSentinelLatchedPrefixLen);
    if (!latchedTemp && strncmp(name + 1, "preflight-fail-", kSentinelPreflightFailPrefixLen)) {
        return;   // not a marker's temp file
    }
    char path[kSentinelEntryPathBytes];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_ctime < now - kSentinelStaleTempSeconds && unlink(path) == 0) {
        LOG(@"sentinel: removed a stale marker temp file %s", path);
    }
}

// Prunes one expired marker. A stat followed by an unlink by path could delete a fresh marker that another process
// renamed over the expired one in between. So the file is first renamed aside, to ".<name>.tmp-prune-<pid>" (a temp
// name, which no reader matches and which SentinelPruneTmp removes after 60 s if this process dies here). It is then
// re-checked on the renamed inode and unlinked only if it is still expired. A fresh one goes back with RENAME_EXCL:
// if a newer marker landed at the path meanwhile, that one stays and the one set aside goes. The remaining window:
// while it is aside (a few syscalls) a fresh marker is missing from its path, so another process's load-time stat in
// that instant reads "no marker" for one launch.
// SIMCAM_TEST_PRUNE_PAUSE_MS=<n> (activation.sh pf-prune-kept) pauses n ms after the caller's stat found the marker
// expired, before the rename, so a test can rename a fresh marker over it in between (the "kept" branch).
static BOOL SentinelPruneExpired(const char *dir, const char *name, const char *path, long now) {
    char aside[kSentinelEntryPathBytes];
    snprintf(aside, sizeof aside, "%s/.%s.tmp-prune-%d", dir, name, getpid());
    if (gTestPrunePauseMs > 0) {
        fprintf(stderr, "[simcam %d] SIMCAM_TEST_PRUNE_PAUSE_MS: prune of %s paused %ld ms\n", getpid(), path,
                gTestPrunePauseMs);
        SlicedSleepMs(gTestPrunePauseMs);
    }
    if (rename(path, aside) != 0) {
        return NO;
    }
    struct stat st;
    if (stat(aside, &st) == 0 && st.st_mtime <= now) {
        unlink(aside);
        return YES;
    }
    if (renamex_np(aside, path, RENAME_EXCL) != 0) {
        unlink(aside);
    }
    LOG(@"sentinel: %s was re-written while being pruned: kept", path);
    return NO;
}

// PREPARE's preflight step: a real preflight failure writes preflight-fail-<build> (valid for 24 h) next to the
// events.log in use, like the latch marker; a passing preflight unlinks it in both locations.
static void SentinelPreflightMarker(BOOL passed, const char *what) {
    if (gSentinel.mode == kSentinelModeOff) {
        return;
    }
    for (int i = 0; i < kStateDirCount; i++) {
        if (!gSentinel.paths.preflightFail[i][0]) {
            continue;
        }
        if (passed) {
            unlink(gSentinel.paths.preflightFail[i]);
        } else if (gSentinel.events.dirIndex < 0 || i == gSentinel.events.dirIndex) {
            char content[300];
            snprintf(content, sizeof content, "%s\n", what);
            // not retried: the next activation's preflight fails again and writes it
            (void)SentinelWriteMarker(gSentinel.paths.preflightFail[i], content, time(NULL) + kSentinelWindowSeconds);
        }
    }
}

// Counts one build's strikes in the parsed past, within the window of 20 activations and 24 h. until[i] is the
// second-newest counted event plus 24 h (the latch holds while two strikes are in the window).
static void SentinelCountStrikes(NSArray *past, NSString *build, long now, int act, int counts[kFeatureCount],
                                 long until[kFeatureCount], long *newest) {
    long newestEpoch[kFeatureCount] = {0};
    long secondEpoch[kFeatureCount] = {0};

    for (int i = 0; i < kFeatureCount; i++) {
        counts[i] = 0;
        until[i] = 0;
    }

    for (NSArray *fields in past) {
        NSString *kind = fields[kEventFieldKind];
        if (!([kind isEqual:@"crash"] || [kind isEqual:@"hang"])) {
            continue;
        }
        if (![fields[kEventFieldBuild] isEqual:build]) {
            continue;
        }
        long epoch = [fields[kEventFieldEpoch] longLongValue];
        // outside the window of the last kSentinelWindowActivations activations and kSentinelWindowSeconds
        if ([fields[kEventFieldActivation] intValue] <= act - kSentinelWindowActivations ||
            epoch < now - kSentinelWindowSeconds) {
            continue;
        }
        int featureIndex = SentinelFeatureIndex([fields[kEventFieldFeature] UTF8String]);
        counts[featureIndex]++;
        if (epoch >= newestEpoch[featureIndex]) {
            secondEpoch[featureIndex] = newestEpoch[featureIndex];
            newestEpoch[featureIndex] = epoch;
        } else if (epoch > secondEpoch[featureIndex]) {
            secondEpoch[featureIndex] = epoch;
        }
    }

    for (int i = 0; i < kFeatureCount; i++) {
        if (counts[i] >= kSentinelStrikesToLatch) {
            until[i] = secondEpoch[i] + kSentinelWindowSeconds;
        }
    }

    if (newest) {
        for (int i = 0; i < kFeatureCount; i++) {
            newest[i] = newestEpoch[i];
        }
    }
}

// This build's markers in state directory dirIndex: the latch and motion markers are written next to the events.log in
// use, and unlinked in the other location (where an older engine that wrote to both may have left them).
static void SentinelMarkersWriteOwn(int dirIndex, const SentinelLatch *latch) {
    BOOL inUse = dirIndex == gSentinel.events.dirIndex;
    if (latch->camera && inUse) {
        char content[200];
        snprintf(content, sizeof content, "%s\n", latch->features);
        // not retried: the next activation's sentinel step derives the markers from events.log and writes them again
        (void)SentinelWriteMarker(gSentinel.paths.latchMarker[dirIndex], content, latch->until);
    } else if (unlink(gSentinel.paths.latchMarker[dirIndex]) == 0 && latch->camera) {
        LOG(@"sentinel: removed this build's marker outside the events.log location: %s",
            gSentinel.paths.latchMarker[dirIndex]);
    }
    if (latch->coreMotion && inUse) {
        // not retried, as above
        (void)SentinelWriteMarker(gSentinel.paths.motionMarker[dirIndex], "motion\n", latch->motionUntil);
    } else if (unlink(gSentinel.paths.motionMarker[dirIndex]) == 0) {
        LOG(@"sentinel: removed %s (%s)", gSentinel.paths.motionMarker[dirIndex],
            latch->coreMotion ? "outside the events.log location" : "motion no longer latched");
    }
}

// YES if the parsed past still latches another build's marker: latched-motion-<build> at kSentinelStrikesToLatch
// motion strikes, latched-<build> at that many strikes of a camera-closing feature.
static BOOL SentinelMarkerStillLatched(NSArray *past, const char *markerBuild, BOOL motion, long now) {
    int counts[kFeatureCount];
    long untils[kFeatureCount];
    SentinelCountStrikes(past, @(markerBuild), now, gSentinel.events.activationNumber, counts, untils, NULL);
    for (int k = 0; k < kFeatureCount; k++) {
        BOOL latchesMarker = motion ? k == kFeatureMotion : SentinelClosesCamera(k);
        if (counts[k] >= kSentinelStrikesToLatch && latchesMarker) {
            return YES;
        }
    }
    return NO;
}

// In state directory dirIndex: the stale temp files, and every other build's latched-* and latched-motion-* marker
// that the parsed past no longer supports (swap-back: another build's activations pushed its strikes out of the
// window).
static void SentinelMarkersPruneOtherBuilds(int dirIndex, NSArray *past, long now) {
    const char *dirPath = gSentinel.paths.eventsDirs[dirIndex];
    DIR *dirStream = opendir(dirPath);
    if (!dirStream) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dirStream))) {
        SentinelPruneTmp(dirPath, entry->d_name, now);
        if (strncmp(entry->d_name, "latched-", kSentinelLatchedPrefixLen)) {
            continue;
        }
        BOOL motion = !strncmp(entry->d_name, "latched-motion-", kSentinelLatchedMotionPrefixLen);
        size_t prefixLen = motion ? kSentinelLatchedMotionPrefixLen : kSentinelLatchedPrefixLen;
        const char *markerBuild = entry->d_name + prefixLen;
        if (!strcmp(markerBuild, gSentinel.build)) {
            continue;
        }
        char path[kSentinelEntryPathBytes];
        snprintf(path, sizeof path, "%s/%s", dirPath, entry->d_name);
        BOOL stillLatched = SentinelMarkerStillLatched(past, markerBuild, motion, now);
        // If the events say the marker is stale, it is removed whatever its until (a racing write of that build is
        // stale too). Otherwise it is removed only once expired, through SentinelPruneExpired.
        struct stat st;
        if (!stillLatched || stat(path, &st) != 0) {
            unlink(path);
            LOG(@"sentinel: removed stale marker %s", path);
        } else if (st.st_mtime <= now && SentinelPruneExpired(dirPath, entry->d_name, path, now)) {
            LOG(@"sentinel: removed stale marker %s", path);
        }
    }
    closedir(dirStream);
}

// preflight-fail-<any build> past its until, in both locations' sim-level directories. Unexpired ones of other builds
// stay: no event records a preflight failure, so nothing says they are stale, and that build still reads them.
static void SentinelMarkersPrunePreflightFail(long now) {
    for (int i = 0; i < kStateDirCount; i++) {
        char dir[kSentinelPathBytes];
        snprintf(dir, sizeof dir, "%s", gSentinel.paths.preflightFail[i]);
        char *slash = strrchr(dir, '/');
        if (!gSentinel.paths.preflightFail[i][0] || !slash) {
            continue;
        }
        *slash = 0;
        DIR *dirStream = opendir(dir);
        if (!dirStream) {
            continue;
        }
        struct dirent *entry;
        while ((entry = readdir(dirStream))) {
            SentinelPruneTmp(dir, entry->d_name, now);
            if (strncmp(entry->d_name, "preflight-fail-", kSentinelPreflightFailPrefixLen)) {
                continue;
            }
            char path[kSentinelEntryPathBytes];
            snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_mtime <= now && SentinelPruneExpired(dir, entry->d_name, path, now)) {
                LOG(@"sentinel: removed expired marker %s", path);
            }
        }
        closedir(dirStream);
    }
}

// The sentinel step's marker update, in both state directories. This build's markers are written only next to the
// events.log in use, so `stuntcam sentinel clear` (the state dir, and the container-tmp fallback on a booted sim)
// removes every marker that can latch. Other builds' markers the events no longer support are deleted.
static void SentinelMarkersUpdate(NSArray *past, long now, const SentinelLatch *latch) {
    for (int i = 0; i < kStateDirCount; i++) {
        if (!gSentinel.paths.eventsDirs[i][0]) {
            continue;
        }
        SentinelMarkersWriteOwn(i, latch);
        SentinelMarkersPruneOtherBuilds(i, past, now);
    }
    SentinelMarkersPrunePreflightFail(now);
}
