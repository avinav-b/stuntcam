// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/markers.inc): the events file's
// writers outside the crash handler: the trim, the locked append and the record queue's writer.
// the trim: only past 4000 lines; the kept 2000 go to a temp file, which replaces events.log only if the file is
// unchanged since the read (size, mtime, inode), under flock. The record queue appends under the same lock.
static void SCTrim(void) {
    struct stat before;
    if (stat(gSCPath, &before) != 0) return;
    NSString *old = [NSString stringWithContentsOfFile:@(gSCPath) encoding:NSUTF8StringEncoding error:nil];
    NSArray *oldLines = [old componentsSeparatedByString:@"\n"];
    if (oldLines.count <= 4000) return;
    atomic_store(&gActPhase, PH_TRIM);
    NSString *kept = [[oldLines subarrayWithRange:NSMakeRange(oldLines.count - 2000, 2000)]
                         componentsJoinedByString:@"\n"];
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.trim", gSCPath);
    if ([kept writeToFile:@(tmp) atomically:NO encoding:NSUTF8StringEncoding error:nil]) {
        ActTestSlowAt(PH_TRIM);
        int fd = open(gSCPath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            flock(fd, LOCK_EX);
            struct stat now;
            BOOL same = fstat(fd, &now) == 0 && now.st_ino == before.st_ino && now.st_size == before.st_size &&
                        now.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec &&
                        now.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec;
            if (same && rename(tmp, gSCPath) == 0) LOG(@"sentinel: trimmed %s to the newest 2000 lines", gSCPath);
            else {
                unlink(tmp);
                LOG(@"sentinel: trim skipped (%s changed during the trim); the next launch trims", gSCPath);
            }
            flock(fd, LOCK_UN);
            close(fd);
        } else unlink(tmp);
    }
    atomic_store(&gActPhase, PH_SENTINEL);
}
// append one line under the trim's lock; if the file was renamed meanwhile (a trim), reopen and append to the new one
static BOOL SCAppendLocked(const char *path, const char *line, size_t n) {
    for (int tries = 0; tries < 3; tries++) {
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return NO;
        flock(fd, LOCK_EX);
        struct stat opened, atPath;
        BOOL current = fstat(fd, &opened) == 0 && stat(path, &atPath) == 0 && opened.st_ino == atPath.st_ino;
        // a failed write still counts as appended: retrying after a short write could append the line twice
        if (current) (void)write(fd, line, n);
        flock(fd, LOCK_UN);
        close(fd);
        if (current) return YES;
    }
    return NO;
}
// the highest activation number in the last 8 KB of a file (activation numbers only grow, so the tail holds the max)
static int SCTailMaxAct(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    off_t end = lseek(fd, 0, SEEK_END), from = end > 8192 ? end - 8192 : 0;
    char b[8193];
    ssize_t n = pread(fd, b, end - from, from);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    int maxAct = 0;
    char *p = b;
    if (from > 0) {   // a partial first line
        char *nl = strchr(p, '\n');
        if (!nl) return 0;
        p = nl + 1;
    }
    while (*p) {
        char *nl = strchr(p, '\n');
        char *sp = strchr(p, ' ');
        if (sp && (!nl || sp < nl)) {
            int act = atoi(sp + 1);
            if (act > maxAct) maxAct = act;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return maxAct;
}
// the record queue's writer: this launch's path and activation number if PREPARE 1 published them, else the
// load-time state dir, then the container-tmp fallback (mkdir -p each; the first that opens), activation = tail max +
// 1. Returns the activation number used, 0 if nothing could be written.
static int SCRecordEvent(const char *kind, const char *feature, const char *detail) {
    if (gSCSentinel == 0) return 0;
    char path[1100];
    int act = 0;
    const char *published = atomic_load_explicit(&gSCPathPub, memory_order_acquire);
    if (published) {
        snprintf(path, sizeof path, "%s", published);
        act = atomic_load(&gSCActPub);
    } else {
        int recorded = atomic_load(&gActRecordedAct);
        for (int i = 0; i < 2 && !act; i++) {
            if (!gSCDir[i][0] || SCMkdirP(gSCDir[i])) continue;
            snprintf(path, sizeof path, "%s/events.log", gSCDir[i]);
            int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) continue;
            close(fd);
            act = recorded ? recorded : SCTailMaxAct(path) + 1;
        }
        if (!act) return 0;
    }
    char b[300];
    int n = SCFormat(b, sizeof b, act, kind, feature, detail, -1);
    return SCAppendLocked(path, b, n) ? act : 0;
}
