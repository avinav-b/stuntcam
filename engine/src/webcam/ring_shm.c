// SPDX-License-Identifier: Apache-2.0
// ring_shm.c: creating, checking, mapping and retiring the POSIX shm object of a webcam ring (ring_shm.h).
#include "ring_shm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int scw_shm_check_stat(const struct stat *info, uid_t uid, const char *name, char *msg, size_t cap) {
    if (info->st_uid != uid) {
        if (msg) {
            snprintf(msg, cap, "webcam: shm name %s owned by uid %lu", name, (unsigned long)info->st_uid);
        }
        return 0;
    }
    if (info->st_mode & 077) {
        if (msg) {
            snprintf(msg, cap, "webcam: shm name %s has mode %04o (group/other access); refusing it", name,
                     (unsigned)(info->st_mode & 07777));
        }
        return 0;
    }
    return 1;
}

// Owner of whatever object sits at `name`: its uid, or -1 if it can't be opened for reading.
static long scw_owner_of(const char *name) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    struct stat info;
    long uid = fstat(fd, &info) == 0 ? (long)info.st_uid : -1;
    close(fd);
    return uid;
}

static void scw_refuse_owner(scw_shm_report *report, const char *name, long owner, int err) {
    report->owner = owner;
    if (owner >= 0) {
        snprintf(report->msg, sizeof report->msg, "webcam: shm name %s owned by uid %ld", name, owner);
    } else {
        snprintf(report->msg, sizeof report->msg, "webcam: shm name %s is held by another user (%s)", name,
                 strerror(err));
    }
}

// Gives up a new object that can't be published: closes its descriptor, unlinks its name if unlink_name, and returns
// err as a result code.
static int scw_create_abandon(int fd, const char *name, int err, int unlink_name) {
    close(fd);
    if (unlink_name) {
        shm_unlink(name);
    }
    return SCW_E_SYS(err);
}

int scw_shm_create(scw_shm_writer *writer, const char *name, uint64_t bytes, scw_shm_report *report) {
    memset(writer, 0, sizeof *writer);
    writer->fd = -1;
    memset(report, 0, sizeof *report);
    report->owner = -1;

    if (!name || strlen(name) > SCW_NAME_MAX || name[0] != '/' || bytes < SCW_HDR || bytes > SCW_MAX_OBJECT) {
        snprintf(report->msg, sizeof report->msg, "webcam: bad shm name or size");
        return SCW_E_ARG;
    }

    uid_t uid = getuid();

    // 1. Remove a stale object: a crashed helper's, or this helper's own from a previous geometry.
    report->unlinkErrno = shm_unlink(name) == 0 ? 0 : errno;

    if (report->unlinkErrno && report->unlinkErrno != ENOENT) {
        scw_refuse_owner(report, name, scw_owner_of(name), report->unlinkErrno);
        return SCW_E_SYS(report->unlinkErrno);
    }

    // 2. Create a new object, never open an existing one. The mode is passed explicitly: macOS doesn't apply the
    //    umask to shm objects (the object gets exactly this mode), and where a umask applies it can only remove bits
    //    from 0600.
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (fd < 0) {
        int err = errno;
        if (err == EEXIST || err == EACCES) {
            scw_refuse_owner(report, name, scw_owner_of(name), err);
        } else {
            snprintf(report->msg, sizeof report->msg, "webcam: shm_open %s: %s", name, strerror(err));
        }
        return SCW_E_SYS(err);
    }

    // 3. Belt and braces where the platform supports it (macOS refuses fchmod on a shm descriptor with EINVAL).
    report->fchmodErrno = fchmod(fd, 0600) == 0 ? 0 : errno;

    // 4. The guarantee: refuse to publish unless this user owns the object and nobody else has any access bits.
    struct stat info;

    if (fstat(fd, &info) != 0) {
        int err = errno;
        snprintf(report->msg, sizeof report->msg, "webcam: fstat %s: %s", name, strerror(err));
        return scw_create_abandon(fd, name, err, 1);
    }

    report->mode = (unsigned)(info.st_mode & 07777);

    if (!scw_shm_check_stat(&info, uid, name, report->msg, sizeof report->msg)) {
        report->owner = (long)info.st_uid;
        return scw_create_abandon(fd, name, EPERM, info.st_uid == uid);   // never unlink another user's object
    }

    if (ftruncate(fd, (off_t)bytes) != 0) {
        int err = errno;
        snprintf(report->msg, sizeof report->msg, "webcam: ftruncate %s to %llu: %s", name, (unsigned long long)bytes,
                 strerror(err));
        return scw_create_abandon(fd, name, err, 1);
    }

    void *mapping = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (mapping == MAP_FAILED) {
        int err = errno;
        snprintf(report->msg, sizeof report->msg, "webcam: mmap %s: %s", name, strerror(err));
        return scw_create_abandon(fd, name, err, 1);
    }

    snprintf(writer->name, sizeof writer->name, "%s", name);
    writer->fd = fd;
    writer->header = mapping;
    writer->len = bytes;
    return SCW_OK;
}

// 1 if the object at `name` is owned by this user and carries `epoch` (macOS reports st_dev = st_ino = 0 for every
// shm object, so the epoch is the only identity).
static int scw_name_has_epoch(const char *name, uint64_t epoch) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    struct stat info;
    int same = 0;
    if (fstat(fd, &info) == 0 && info.st_uid == getuid() && info.st_size >= (off_t)SCW_HDR) {
        void *mapping = mmap(NULL, SCW_HDR, PROT_READ, MAP_SHARED, fd, 0);
        if (mapping != MAP_FAILED) {
            same = __atomic_load_n(&((const scw_header *)mapping)->epoch, __ATOMIC_ACQUIRE) == epoch;
            munmap(mapping, SCW_HDR);
        }
    }
    close(fd);
    return same;
}

void scw_shm_retire(scw_shm_writer *writer, uint32_t final_state) {
    if (!writer->header) {
        return;
    }

    // the epoch and geometry come from the writer's private copy, never from the shared header
    if (writer->ring.header) {
        scw_writer_state(&writer->ring, final_state);
        scw_writer_clear(&writer->ring);
    }

    // Unlink only if the name still holds this object (after a supersede it already holds the new one). The private
    // epoch is stored into this object's header first, so a header another process rewrote still identifies as this
    // writer's; a different object at the name keeps its own epoch.
    __atomic_store_n(&writer->header->epoch, writer->ring.epoch, __ATOMIC_SEQ_CST);
    if (scw_name_has_epoch(writer->name, writer->ring.epoch)) {
        shm_unlink(writer->name);
    }
    munmap(writer->header, (size_t)writer->len);
    if (writer->fd >= 0) {
        close(writer->fd);
    }
    writer->header = NULL;
    writer->fd = -1;
    writer->len = 0;
    memset(&writer->ring, 0, sizeof writer->ring);
}

int scw_shm_open_ro(const char *name, const uint32_t *accepted, unsigned naccepted, scw_shm_open_result *result) {
    char *why = result->why;
    size_t cap = sizeof result->why;
    result->map = NULL;
    result->len = 0;
    result->owner = -1;
    int fd = shm_open(name, O_RDONLY, 0);

    if (fd < 0) {
        int err = errno;
        if (err == ENOENT) {
            snprintf(why, cap, "webcam: no ring at shm name %s (helper not running)", name);
        } else {
            snprintf(why, cap, "webcam: shm name %s: %s", name, strerror(err));
        }
        return SCW_E_SYS(err);
    }

    struct stat info;

    if (fstat(fd, &info) != 0) {
        int err = errno;
        close(fd);
        snprintf(why, cap, "webcam: fstat %s: %s", name, strerror(err));
        return SCW_E_SYS(err);
    }

    if (!scw_shm_check_stat(&info, getuid(), name, why, cap)) {
        result->owner = (long)info.st_uid;
        close(fd);
        return SCW_E_SYS(EPERM);
    }

    if (info.st_size < (off_t)SCW_HDR || (uint64_t)info.st_size > SCW_MAX_OBJECT) {
        close(fd);
        snprintf(why, cap, "webcam: shm name %s has size %lld, not a ring", name, (long long)info.st_size);
        return info.st_size < (off_t)SCW_HDR ? SCW_E_SHORT : SCW_E_SIZE;
    }

    uint64_t size = (uint64_t)info.st_size;
    void *mapping = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
    int err = errno;
    close(fd);

    if (mapping == MAP_FAILED) {
        snprintf(why, cap, "webcam: mmap %s: %s", name, strerror(err));
        return SCW_E_SYS(err);
    }

    uint32_t found = 0;
    int rc = scw_validate(mapping, size, accepted, naccepted, &result->geom, &found);

    if (rc != SCW_OK) {
        munmap(mapping, (size_t)size);
        if (rc == SCW_E_VERSION) {
            scw_version_message(why, cap, found, accepted, naccepted);
        } else {
            snprintf(why, cap, "webcam: %s (%s)", scw_strerror(rc), name);
        }
        return rc;
    }

    result->map = mapping;
    result->len = size;
    return SCW_OK;
}

static int scw_reader_is_dead(const scw_shm_reader *reader, uint64_t epoch) {
    unsigned count = reader->ndead < SCW_READER_DEAD ? reader->ndead : SCW_READER_DEAD;
    for (unsigned i = 0; i < count; i++) {
        if (reader->dead[i] == epoch) {
            return 1;
        }
    }
    return 0;
}

void scw_shm_reader_init(scw_shm_reader *reader, const char *name, const uint32_t *accepted, unsigned naccepted) {
    memset(reader, 0, sizeof *reader);
    snprintf(reader->name, sizeof reader->name, "%s", name);
    for (unsigned i = 0; i < naccepted && i < SCW_READER_ACCEPTED_MAX; i++) {
        reader->accepted[reader->naccepted++] = accepted[i];
    }
    reader->owner = -1;
    reader->status = SCW_E_NOTREADY;
    snprintf(reader->why, sizeof reader->why, "webcam: not opened yet");
}

void scw_shm_reader_close(scw_shm_reader *reader) {
    if (reader->map) {
        munmap((void *)reader->map, (size_t)reader->len);
    }
    reader->map = NULL;
    reader->len = 0;
}

// The mapping the reader holds: 1 while it is live. A mapping whose identity changed is dropped (re-open at once); one
// whose helper stopped or superseded it is dropped and its epoch never adopted again.
static int scw_reader_check_mapping(scw_shm_reader *reader, uint64_t now_ns, scw_peek_t *peek) {
    int rc = scw_peek(reader->map, &reader->geom, peek);
    if (rc != SCW_OK) {
        scw_shm_reader_close(reader);
        reader->nextOpenNs = now_ns;
        return 0;
    }
    if (peek->state == SCW_STATE_SUPERSEDED || peek->state == SCW_STATE_STOPPED) {
        reader->dead[reader->ndead++ % SCW_READER_DEAD] = reader->geom.epoch;   // never adopted again
        scw_shm_reader_close(reader);
        // the new object is already there
        if (peek->state == SCW_STATE_SUPERSEDED) {
            reader->nextOpenNs = now_ns;
        }
        snprintf(reader->why, sizeof reader->why, "webcam: helper %s", scw_state_name(peek->state));
        return 0;
    }
    return scw_is_live(peek, now_ns);
}

// 1 if a valid ring is larger than this reader copies and draws each tick (in either orientation).
static int scw_reader_too_large(const scw_shm_reader *reader, const scw_geom *geom) {
    if (!reader->maxWidth || !reader->maxHeight) {
        return 0;
    }
    int fits = geom->width <= reader->maxWidth && geom->height <= reader->maxHeight;
    int fitsTurned = geom->width <= reader->maxHeight && geom->height <= reader->maxWidth;
    return !fits && !fitsTurned;
}

// One re-open attempt. It adopts a new object, keeps the mapping it has, or refuses what it found. Returns 0 when the
// reader is offline for this poll (the reason is in reader->why), 1 when the poll goes on to read the mapping it has.
static int scw_reader_reopen(scw_shm_reader *reader, uint64_t now_ns) {
    reader->nextOpenNs = now_ns + SCW_REOPEN_NS;
    scw_shm_open_result opened;
    int rc = scw_shm_open_ro(reader->name, reader->accepted, reader->naccepted, &opened);
    reader->status = rc;
    reader->owner = opened.owner;
    if (rc == SCW_OK && scw_reader_is_dead(reader, opened.geom.epoch)) {
        // still the retired object (the helper hasn't unlinked it yet): wait for the next attempt
        munmap((void *)opened.map, (size_t)opened.len);
        if (!reader->map) {
            snprintf(reader->why, sizeof reader->why, "webcam: waiting for the helper's new ring (%s)", reader->name);
            return 0;
        }
        return 1;
    }
    if (rc == SCW_OK && scw_reader_too_large(reader, &opened.geom)) {
        // valid, but larger than this reader will copy and draw each tick: offline, never mapped
        munmap((void *)opened.map, (size_t)opened.len);
        reader->status = SCW_E_LIMIT;
        snprintf(reader->why, sizeof reader->why, "webcam: ring %ux%u is larger than this engine accepts (%ux%u) (%s)",
                 opened.geom.width, opened.geom.height, reader->maxWidth, reader->maxHeight, reader->name);
        return 0;
    }
    if (rc == SCW_OK) {
        if (!reader->map || opened.geom.epoch != reader->geom.epoch) {
            scw_shm_reader_close(reader);
            reader->map = opened.map;
            reader->len = opened.len;
            reader->geom = opened.geom;
            reader->opens++;
        } else {
            munmap((void *)opened.map, (size_t)opened.len);   // the same object this reader already maps
        }
        return 1;
    }
    snprintf(reader->why, sizeof reader->why, "%s", opened.why);
    if (!reader->map) {
        return 0;
    }
    // The mapping held here is not live. Why the name can't be opened (a refused owner, a newer ring version) says
    // more than "stale"; only a missing or half-made name goes on to the mapping's own reason.
    return rc == SCW_E_SYS(ENOENT) || rc == SCW_E_NOTREADY;
}

int scw_shm_reader_poll(scw_shm_reader *reader, uint64_t now_ns, scw_peek_t *peek) {
    scw_peek_t local;
    if (!peek) {
        peek = &local;
    }
    memset(peek, 0, sizeof *peek);

    if (reader->map && scw_reader_check_mapping(reader, now_ns, peek)) {
        return 1;
    }

    // Offline: re-open the name at most once per SCW_REOPEN_NS, so that a restarted helper, an unlinked object or an
    // idle exit never pins the reader to a dead mapping.
    if (now_ns >= reader->nextOpenNs && !scw_reader_reopen(reader, now_ns)) {
        return 0;
    }
    if (!reader->map) {
        return 0;
    }

    if (scw_peek(reader->map, &reader->geom, peek) != SCW_OK) {
        snprintf(reader->why, sizeof reader->why, "webcam: ring replaced under the mapping (%s)", reader->name);
        return 0;
    }
    if (scw_is_live(peek, now_ns)) {
        return 1;
    }
    if (peek->state != SCW_STATE_RUNNING) {
        snprintf(reader->why, sizeof reader->why, "webcam: helper %s", scw_state_name(peek->state));
    } else {
        snprintf(reader->why, sizeof reader->why, "webcam: no fresh frame (%s)", peek->latest ? "stale" : "none yet");
    }
    return 0;
}
