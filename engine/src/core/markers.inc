// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/crash_handler.inc): the sentinel's
// load-time state and its markers (latched-<build>, latched-motion-<build>, preflight-fail-<build>): paths, the
// load-time stat()s, writes and prunes.
// ---- load-time state, markers, the record queue's writer ----
// Markers (mtime = until; one past its until is ignored and deleted by the next prepare):
//   <state>/<bundle>/latched-<build>         the latched camera features; read at load: the camera starts STOCK, no
//                                            prepare, and CoreMotion is stock
//   <state>/<bundle>/latched-motion-<build>  `motion`: read at load by motion only; the camera is unaffected
//   <state>/preflight-fail-<build>           a real preflight failure, until +24 h: read at load by motion only (the
//                                            camera always runs its own preflight); a passing preflight unlinks it
// Every location is covered twice: SIMCAM_STATE_DIR and the container-tmp fallback (17.5, where the state dir can't be
// written).
static int gSCSentinel = 1;                    // SIMCAM_SENTINEL: 0 off, 1 on, 2 clear
static char gSCBundle[256] = "unknown";
static char gSCDir[2][1024], gSCMarker[2][1100], gSCPfPath[2][1100], gSCMarkerAtLoad[1100];
static char gSCMotionMarker[2][1100];          // <dir>/latched-motion-<build>, both locations
static char gSCMotionStockAt[1100];            // the marker that made CoreMotion stock at load ("" = the stand-in)
static time_t gSCMotionStockUntil;             // its until (st_mtime) at that load-time stat
static ino_t gSCMotionStockIno;                // and its inode: the stock line reads the features only from this one
// what it says if it is gone by the stock line: "camera" | "motion" | "preflight failed"
static const char *gSCMotionStockKind;
static BOOL gSCClearedAtLoad;
static _Atomic(const char *) gSCPathPub;       // published by PREPARE 1 (release) for the record queue
static _Atomic int gSCActPub;
static _Atomic int gActRecordedAct;            // the activation number the record queue's timeout line used (0 = none)
static char gSCLatchMsg[600];                  // PREPARE 1's latch line, printed through the record queue
static int SCMkdirP(const char *path) {
    char b[1024];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0755);
            *p = '/';
        }
    }
    return (mkdir(b, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}
static void SCTmpRoot(char *out, size_t cap) {   // <container tmp>/simcam-state
    const char *t = getenv("TMPDIR");
    char b[1024];
    if (t && *t) snprintf(b, sizeof b, "%s", t);
    else snprintf(b, sizeof b, "%s", NSTemporaryDirectory().fileSystemRepresentation);
    size_t l = strlen(b);
    while (l > 1 && b[l - 1] == '/') b[--l] = 0;
    snprintf(out, cap, "%s/simcam-state", b);
}
// at load: no I/O apart from the marker stat()s (and the explicit clear below)
static void SCLoadInit(void) {
    const char *sw = getenv(ENV_SENTINEL);
    gSCSentinel = sw && sw[0] == '0' ? 0 : sw && !strcmp(sw, "clear") ? 2 : 1;
    gSCScale = (dlsym(RTLD_DEFAULT, "__tsan_init") || dlsym(RTLD_DEFAULT, "__asan_init")) ? 10 : 1;
    Dl_info di;
    if (dladdr((const void *)SCLoadInit, &di) && di.dli_fname) {   // build = the content-hash dir of our dylib
        const char *dirEnd = strrchr(di.dli_fname, '/');
        if (dirEnd) {
            const char *dirStart = dirEnd;
            while (dirStart > di.dli_fname && dirStart[-1] != '/') dirStart--;
            snprintf(gSCBuild, sizeof gSCBuild, "%.*s", (int)(dirEnd - dirStart), dirStart);
        }
    }
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFStringRef bundleID = mainBundle ? CFBundleGetIdentifier(mainBundle) : NULL;
    if (bundleID) CFStringGetCString(bundleID, gSCBundle, sizeof gSCBundle, kCFStringEncodingUTF8);
    const char *stateDir = getenv(ENV_STATE_DIR);
    char tmp[1024];
    SCTmpRoot(tmp, sizeof tmp);
    if (stateDir && *stateDir) {
        snprintf(gSCDir[0], sizeof gSCDir[0], "%s/%s", stateDir, gSCBundle);
        snprintf(gSCPfPath[0], sizeof gSCPfPath[0], "%s/preflight-fail-%s", stateDir, gSCBuild);
    }
    snprintf(gSCDir[1], sizeof gSCDir[1], "%s/%s", tmp, gSCBundle);
    snprintf(gSCPfPath[1], sizeof gSCPfPath[1], "%s/preflight-fail-%s", tmp, gSCBuild);
    for (int i = 0; i < 2; i++) {
        if (gSCDir[i][0]) {
            snprintf(gSCMarker[i], sizeof gSCMarker[i], "%s/latched-%s", gSCDir[i], gSCBuild);
            snprintf(gSCMotionMarker[i], sizeof gSCMotionMarker[i], "%s/latched-motion-%s", gSCDir[i], gSCBuild);
        }
    }
    gOffHandlerMotionMarker = getenv(ENV_OFF_HANDLER_MOTION_MARKER) != NULL;
    gOffMarkerRename = getenv(ENV_OFF_MARKER_RENAME) != NULL;
    const char *slowMarker = getenv(ENV_TEST_SLOW_MARKER_MS);
    if (slowMarker) gTestSlowMarkerMs = atol(slowMarker);
    const char *prunePause = getenv(ENV_TEST_PRUNE_PAUSE_MS);
    if (prunePause) gTestPrunePauseMs = atol(prunePause);
}
static BOOL SCMarkerValidAtLoad(void) {
    if (gSCSentinel != 1) return NO;
    time_t now = time(NULL);
    for (int i = 0; i < 2; i++) {
        struct stat st;
        if (gSCMarker[i][0] && stat(gSCMarker[i], &st) == 0 && st.st_mtime > now) {
            snprintf(gSCMarkerAtLoad, sizeof gSCMarkerAtLoad, "%s", gSCMarker[i]);
            return YES;
        }
    }
    return NO;
}
// CoreMotion is decided once, at load, from markers only. Any valid one of this build's latched-<build> (a
// camera latch), latched-motion-<build> or preflight-fail-<build>, in either location, with the sentinel on and no
// clear, makes CoreMotion stock for the process. Markers this process writes later can't change it (a camera activation
// that fails in this launch keeps the stand-in for the launch). SIMCAM_OFF_MOTION_MARKERS=1 (ledger L58) ignores them.
static BOOL SCMotionStockAtLoad(void) {
    if (gSCSentinel != 1 || getenv(ENV_OFF_MOTION_MARKERS)) return NO;
    time_t now = time(NULL);
    const char *candidates[6] = {gSCMarker[0], gSCMarker[1], gSCMotionMarker[0], gSCMotionMarker[1], gSCPfPath[0],
                                 gSCPfPath[1]};
    for (int i = 0; i < 6; i++) {
        struct stat st;
        if (candidates[i][0] && stat(candidates[i], &st) == 0 && st.st_mtime > now) {
            snprintf(gSCMotionStockAt, sizeof gSCMotionStockAt, "%s", candidates[i]);
            // kept for the stock line: by the first motion call a passing preflight or PREPARE 1 may have unlinked the
            // marker
            gSCMotionStockUntil = st.st_mtime;
            gSCMotionStockIno = st.st_ino;
            gSCMotionStockKind = i < 2 ? "camera" : i < 4 ? "motion" : "preflight failed";
            return YES;
        }
    }
    return NO;
}
static void SCUnlinkLatchMarkers(const char *dir, const char *keepBuild) {   // every latched-* in dir (but keepBuild's)
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "latched-", 8) || (keepBuild && !strcmp(e->d_name + 8, keepBuild))) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}
// SIMCAM_SENTINEL=clear at load: an explicit user action, so I/O at load is acceptable. Appends the clear line (also
// for an app that never runs prepare: a motion-only app) and unlinks the bundle's markers and this build's
// preflight-fail.
static void SCClearAtLoad(void) {
    if (gSCSentinel != 2) return;
    for (int i = 0; i < 2; i++) {
        if (!gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/events.log", gSCDir[i]);
        int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        char b[256];
        int n = SCFormat(b, sizeof b, 0, "clear", "-", "SIMCAM_SENTINEL=clear", -1);
        // best effort: a failed write loses the clear line (PREPARE 1 then reads the older records); the markers are
        // unlinked below either way
        (void)write(fd, b, n);
        close(fd);
        gSCClearedAtLoad = YES;
        break;
    }
    for (int i = 0; i < 2; i++) {
        if (gSCDir[i][0]) SCUnlinkLatchMarkers(gSCDir[i], NULL);
        if (gSCPfPath[i][0]) unlink(gSCPfPath[i]);
    }
}
// A marker is written to "<dir>/.<name>.tmp-<pid>", its mtime set to the until, then renamed
// into place. Written in place, it would exist with mtime = now between open(O_TRUNC) and futimens, and another
// process's PREPARE 1 prune (st_mtime <= now) could unlink it. Temp names start with '.', so no prune, load-time stat
// or CLI listing matches them. SIMCAM_OFF_MARKER_RENAME=1 (ledger L64's mutant) writes in place;
// SIMCAM_TEST_SLOW_MARKER_MS=<n> pauses n ms after the content is written, before the until is set (pf-prune-race
// widens the window with it).
static void SCSlowMarker(const char *path) {
    if (gTestSlowMarkerMs <= 0) return;
    fprintf(stderr, "[simcam %d] SIMCAM_TEST_SLOW_MARKER_MS: marker write paused %ld ms (%s)\n", getpid(),
            gTestSlowMarkerMs, path);
    ActSleepMs(gTestSlowMarkerMs);
}
static BOOL SCWriteMarker(const char *path, const char *content, time_t until) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        SCMkdirP(dir);
    }
    char tmp[1300];
    const char *base = slash ? slash + 1 : path;
    if (gOffMarkerRename) snprintf(tmp, sizeof tmp, "%s", path);
    else snprintf(tmp, sizeof tmp, "%s/.%s.tmp-%d", slash ? dir : ".", base, getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return NO;
    // a failed write is not a failed marker: it latches by its name and mtime (set below). Its content is what
    // `stuntcam sentinel` and `status` show (the CLI derives the latch's effect from it), so a short write only
    // degrades that report
    (void)write(fd, content, strlen(content));
    SCSlowMarker(path);
    struct timespec ts[2] = {{until, 0}, {until, 0}};
    futimens(fd, ts);
    close(fd);
    if (gOffMarkerRename) return YES;
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return NO;
    }
    return YES;
}
// a temp file left by a process that died mid-write: removed once it is a minute old (its mtime is the until, so ctime)
static void SCPruneTmp(const char *dir, const char *name, long now) {
    if (name[0] != '.' || !strstr(name, ".tmp-") ||
        (strncmp(name + 1, "latched-", 8) && strncmp(name + 1, "preflight-fail-", 15)))
        return;
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    struct stat st;
    if (stat(p, &st) == 0 && st.st_ctime < now - 60 && unlink(p) == 0)
        LOG(@"sentinel: removed a stale marker temp file %s", p);
}
// Prune one expired marker: a stat, then an unlink by path, could delete a fresh marker that another
// process renamed over the expired one in between. So the file is renamed aside first (".<name>.tmp-prune-<pid>": a
// temp name, which no reader matches and SCPruneTmp removes after 60 s if this process dies here), re-checked on the
// renamed inode, and unlinked only if it is still expired; a fresh one goes back (RENAME_EXCL: if a newer marker landed
// at the path meanwhile, that one stays and ours goes). Remaining window: while it is aside (a few syscalls), a fresh
// marker is missing from its path, so another process's load-time stat in that instant reads "no marker" for one
// launch.
// SIMCAM_TEST_PRUNE_PAUSE_MS=<n> (activation.sh pf-prune-kept): pauses n ms after the caller's stat found the marker
// expired, before the rename, so a test can rename a fresh marker over it in between (the "kept" branch).
static BOOL SCPruneExpired(const char *dir, const char *name, const char *p, long now) {
    char aside[1400];
    snprintf(aside, sizeof aside, "%s/.%s.tmp-prune-%d", dir, name, getpid());
    if (gTestPrunePauseMs > 0) {
        fprintf(stderr, "[simcam %d] SIMCAM_TEST_PRUNE_PAUSE_MS: prune of %s paused %ld ms\n", getpid(), p,
                gTestPrunePauseMs);
        ActSleepMs(gTestPrunePauseMs);
    }
    if (rename(p, aside) != 0) return NO;
    struct stat st;
    if (stat(aside, &st) == 0 && st.st_mtime <= now) {
        unlink(aside);
        return YES;
    }
    if (renamex_np(aside, p, RENAME_EXCL) != 0) unlink(aside);
    LOG(@"sentinel: %s was re-written while being pruned: kept", p);
    return NO;
}
// PREPARE 3: a real preflight failure writes preflight-fail-<build> (until +24 h) next to the events.log in use, as the
// latch marker; a passing preflight unlinks it in both locations
static void SCPreflightMarker(BOOL passed, const char *what) {
    if (gSCSentinel == 0) return;
    for (int i = 0; i < 2; i++) {
        if (!gSCPfPath[i][0]) continue;
        if (passed) unlink(gSCPfPath[i]);
        else if (gSCIdx < 0 || i == gSCIdx) {
            char content[300];
            snprintf(content, sizeof content, "%s\n", what);
            // not retried: the next activation's preflight fails again and writes it
            (void)SCWriteMarker(gSCPfPath[i], content, time(NULL) + SC_WINDOW_SECONDS);
        }
    }
}
// counted strikes of one build in the parsed past: the 20-activation / 24 h window. until[i] = the second-newest
// counted event + 24 h (the latch holds while two are in the window).
static void SCCountStrikes(NSArray *past, NSString *build, long now, int act, int counts[SC_NFEAT],
                           long until[SC_NFEAT], long *newest) {
    long newestEpoch[SC_NFEAT] = {0}, secondEpoch[SC_NFEAT] = {0};
    for (int i = 0; i < SC_NFEAT; i++) {
        counts[i] = 0;
        until[i] = 0;
    }
    for (NSArray *fields in past) {
        NSString *kind = fields[2];
        if (!([kind isEqual:@"crash"] || [kind isEqual:@"hang"])) continue;
        if (![fields[4] isEqual:build]) continue;
        long epoch = [fields[0] longLongValue];
        if ([fields[1] intValue] <= act - SC_WINDOW_ACTIVATIONS || epoch < now - SC_WINDOW_SECONDS) continue;
        int fi = SCFeatureIndex([fields[3] UTF8String]);
        counts[fi]++;
        if (epoch >= newestEpoch[fi]) {
            secondEpoch[fi] = newestEpoch[fi];
            newestEpoch[fi] = epoch;
        } else if (epoch > secondEpoch[fi]) secondEpoch[fi] = epoch;
    }
    for (int i = 0; i < SC_NFEAT; i++)
        if (counts[i] >= SC_STRIKES_TO_LATCH) until[i] = secondEpoch[i] + SC_WINDOW_SECONDS;
    if (newest)
        for (int i = 0; i < SC_NFEAT; i++) newest[i] = newestEpoch[i];
}
// PREPARE 1 step 6: write this build's marker ONLY next to the events.log in use (gSCIdx), so `stuntcam sentinel
// clear` (the state dir, and this container-tmp fallback on a booted sim) removes every marker that can latch; unlink
// it in the other location (one an older engine wrote to both). In both locations, delete every other build's
// latched-* marker the parsed past no longer supports (swap-back: another build's 20 activations pushed its strikes
// out). The same for latched-motion-<build> (motion latched: >= 2 `motion` strikes; it never closes the camera), and a
// stale latched-motion-<other build> is deleted like a stale latched-<other build>.
static void SCMarkersUpdate(NSArray *past, long now, BOOL latched, const char *feats, long until, BOOL mLatched,
                            long mUntil) {
    for (int i = 0; i < 2; i++) {
        if (!gSCDir[i][0]) continue;
        if (latched && i == gSCIdx) {
            char content[200];
            snprintf(content, sizeof content, "%s\n", feats);
            // not retried: the next activation's PREPARE 1 derives the markers from events.log and writes them again
            (void)SCWriteMarker(gSCMarker[i], content, until);
        } else if (unlink(gSCMarker[i]) == 0 && latched)
            LOG(@"sentinel: removed this build's marker outside the events.log location: %s", gSCMarker[i]);
        if (mLatched && i == gSCIdx) (void)SCWriteMarker(gSCMotionMarker[i], "motion\n", mUntil);   // as above
        else if (unlink(gSCMotionMarker[i]) == 0)
            LOG(@"sentinel: removed %s (%s)", gSCMotionMarker[i],
                mLatched ? "outside the events.log location" : "motion no longer latched");
        DIR *d = opendir(gSCDir[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            SCPruneTmp(gSCDir[i], e->d_name, now);
            if (strncmp(e->d_name, "latched-", 8)) continue;
            BOOL motion = !strncmp(e->d_name, "latched-motion-", 15);
            const char *markerBuild = e->d_name + (motion ? 15 : 8);
            if (!strcmp(markerBuild, gSCBuild)) continue;
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", gSCDir[i], e->d_name);
            int counts[SC_NFEAT];
            long untils[SC_NFEAT];
            SCCountStrikes(past, @(markerBuild), now, gSCAct, counts, untils, NULL);
            BOOL still = NO;
            for (int k = 0; k < SC_NFEAT; k++)
                if (counts[k] >= SC_STRIKES_TO_LATCH && (motion ? k == SC_FMOTION : SCClosesCamera(k))) still = YES;
            // the events say stale: removed whatever its until (a racing write of that build is stale too); otherwise
            // only once expired, through SCPruneExpired
            struct stat st;
            if (!still || stat(p, &st) != 0) {
                unlink(p);
                LOG(@"sentinel: removed stale marker %s", p);
            } else if (st.st_mtime <= now && SCPruneExpired(gSCDir[i], e->d_name, p, now))
                LOG(@"sentinel: removed stale marker %s", p);
        }
        closedir(d);
    }
    // preflight-fail-<any build> past its until, in both locations' sim-level dirs. Unexpired ones of
    // other builds stay: no event records a preflight failure, so nothing says they are stale, and that build still
    // reads them.
    for (int i = 0; i < 2; i++) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s", gSCPfPath[i]);
        char *slash = strrchr(dir, '/');
        if (!gSCPfPath[i][0] || !slash) continue;
        *slash = 0;
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            SCPruneTmp(dir, e->d_name, now);
            if (strncmp(e->d_name, "preflight-fail-", 15)) continue;
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && st.st_mtime <= now && SCPruneExpired(dir, e->d_name, p, now))
                LOG(@"sentinel: removed expired marker %s", p);
        }
        closedir(d);
    }
}
