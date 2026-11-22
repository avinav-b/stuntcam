// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order, after core/markers.inc): the events file's
// writers outside the crash handler: the trim, the locked append and the record queue's writer.
//
// Provides: SentinelTrim (core/sentinel_activate.inc), SentinelRecordEvent (core/lazy.inc)
// Owns:     nothing shared.
// Uses:     core/act.inc, core/safety.inc, core/markers.inc
#include "activation.h"
#include "sentinel.h"

// The trim runs only past kSentinelTrimAboveLines lines. The newest kSentinelTrimKeepLines go to a temp file, which
// replaces events.log only if the file is unchanged since the read (size, mtime, inode), under flock. The record queue
// appends under the same lock.
#define kSentinelTrimAboveLines 4000
#define kSentinelTrimKeepLines 2000
#define kSentinelAppendTries 3    // reopens when a trim renamed the file between the open and the lock
#define kSentinelTailBytes 8192   // how much of the file's end SentinelTailMaxActivation reads
#define kSentinelRecordLineBytes 300

static void SentinelTrim(void) {
    struct stat before;
    if (stat(gSentinel.events.path, &before) != 0) {
        return;
    }
    NSString *old =
        [NSString stringWithContentsOfFile:@(gSentinel.events.path) encoding:NSUTF8StringEncoding error:nil];
    NSArray *oldLines = [old componentsSeparatedByString:@"\n"];
    if (oldLines.count <= kSentinelTrimAboveLines) {
        return;
    }
    atomic_store(&gActivation.phase, kActivationPhaseTrim);
    NSString *kept =
        [[oldLines subarrayWithRange:NSMakeRange(oldLines.count - kSentinelTrimKeepLines, kSentinelTrimKeepLines)]
            componentsJoinedByString:@"\n"];
    char tmp[kSentinelPathBytes];
    snprintf(tmp, sizeof tmp, "%s.trim", gSentinel.events.path);

    if ([kept writeToFile:@(tmp) atomically:NO encoding:NSUTF8StringEncoding error:nil]) {
        ActivationTestSlowAt(kActivationPhaseTrim);
        int fd = open(gSentinel.events.path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            // a failed lock (EINTR, ENOLCK) only leaves the trim unlocked; the unlock below is best effort too
            (void)flock(fd, LOCK_EX);
            struct stat now;
            BOOL same = fstat(fd, &now) == 0 && now.st_ino == before.st_ino && now.st_size == before.st_size &&
                        now.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec &&
                        now.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec;
            if (same && rename(tmp, gSentinel.events.path) == 0) {
                LOG(@"sentinel: trimmed %s to the newest 2000 lines", gSentinel.events.path);
            } else {
                unlink(tmp);
                LOG(@"sentinel: trim skipped (%s changed during the trim); the next launch trims",
                    gSentinel.events.path);
            }
            (void)flock(fd, LOCK_UN);
            close(fd);
        } else {
            unlink(tmp);
        }
    }

    atomic_store(&gActivation.phase, kActivationPhaseSentinel);
}

// Appends one line under the trim's lock. If the file was renamed meanwhile (by a trim), it reopens the path and
// appends to the new file.
static BOOL SentinelAppendLocked(const char *path, const char *line, size_t len) {
    for (int tries = 0; tries < kSentinelAppendTries; tries++) {
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            return NO;
        }
        (void)flock(fd, LOCK_EX);   // as in the trim: a failed lock only leaves the append unlocked
        struct stat opened;
        struct stat atPath;
        BOOL current = fstat(fd, &opened) == 0 && stat(path, &atPath) == 0 && opened.st_ino == atPath.st_ino;
        // a failed write still counts as appended: retrying after a short write could append the line twice
        if (current) {
            (void)write(fd, line, len);
        }
        (void)flock(fd, LOCK_UN);
        close(fd);
        if (current) {
            return YES;
        }
    }
    return NO;
}

// The highest activation number in the last kSentinelTailBytes of a file. Activation numbers only grow, so the tail
// holds the maximum.
static int SentinelTailMaxActivation(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    off_t from = end > kSentinelTailBytes ? end - kSentinelTailBytes : 0;
    char tail[kSentinelTailBytes + 1];
    ssize_t bytesRead = pread(fd, tail, end - from, from);
    close(fd);
    if (bytesRead <= 0) {
        return 0;
    }
    tail[bytesRead] = 0;
    int maxAct = 0;
    char *cursor = tail;

    if (from > 0) {   // skip a partial first line
        char *lineEnd = strchr(cursor, '\n');
        if (!lineEnd) {
            return 0;
        }
        cursor = lineEnd + 1;
    }
    while (*cursor) {
        char *lineEnd = strchr(cursor, '\n');
        char *space = strchr(cursor, ' ');
        if (space && (!lineEnd || space < lineEnd)) {
            int act = atoi(space + 1);
            if (act > maxAct) {
                maxAct = act;
            }
        }
        if (!lineEnd) {
            break;
        }
        cursor = lineEnd + 1;
    }

    return maxAct;
}

// The record queue's writer. It uses this launch's path and activation number if the sentinel step published them.
// Otherwise it tries the load-time state dir, then the container-tmp fallback (mkdir -p each; the first that opens
// wins), with the activation number one past the tail's maximum. Returns the activation number used, or 0 if nothing
// could be written.
static int SentinelRecordEvent(const char *kind, const char *feature, const char *detail) {
    if (gSentinel.mode == kSentinelModeOff) {
        return 0;
    }
    char path[kSentinelPathBytes];
    int act = 0;
    const char *published = atomic_load_explicit(&gSentinel.events.pathPublished, memory_order_acquire);

    if (published) {
        snprintf(path, sizeof path, "%s", published);
        act = atomic_load(&gSentinel.events.activationNumberPublished);
    } else {
        int recorded = atomic_load(&gActivation.timeoutActivationNumber);
        for (int i = 0; i < kStateDirCount && !act; i++) {
            if (!gSentinel.paths.eventsDirs[i][0] || SentinelMkdirP(gSentinel.paths.eventsDirs[i])) {
                continue;
            }
            snprintf(path, sizeof path, "%s/events.log", gSentinel.paths.eventsDirs[i]);
            int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) {
                continue;
            }
            close(fd);
            act = recorded ? recorded : SentinelTailMaxActivation(path) + 1;
        }
        if (!act) {
            return 0;
        }
    }

    char line[kSentinelRecordLineBytes];
    int len = SentinelFormatEvent(line, sizeof line, act, kind, feature, detail, -1);
    return SentinelAppendLocked(path, line, len) ? act : 0;
}
