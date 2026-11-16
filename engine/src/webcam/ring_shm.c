// SPDX-License-Identifier: Apache-2.0
// ring_shm.c: creating, checking, mapping and retiring the POSIX shm object of a webcam ring (ring_shm.h).
#include "ring_shm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int scw_shm_check_stat(const struct stat *st, uid_t uid, const char *name, char *msg, size_t cap) {
    if (st->st_uid != uid) {
        if (msg) snprintf(msg, cap, "webcam: shm name %s owned by uid %lu", name, (unsigned long)st->st_uid);
        return 0;
    }
    if (st->st_mode & 077) {
        if (msg) snprintf(msg, cap, "webcam: shm name %s has mode %04o (group/other access); refusing it", name,
                          (unsigned)(st->st_mode & 07777));
        return 0;
    }
    return 1;
}

// Owner of whatever object sits at `name`: its uid, or -1 if it can't be opened for reading.
static long scw_owner_of(const char *name) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) return -1;
    struct stat st;
    long uid = fstat(fd, &st) == 0 ? (long)st.st_uid : -1;
    close(fd);
    return uid;
}

static void scw_refuse_owner(scw_shm_report *rep, const char *name, long owner, int err) {
    rep->owner = owner;
    if (owner >= 0)
        snprintf(rep->msg, sizeof rep->msg, "webcam: shm name %s owned by uid %ld", name, owner);
    else
        snprintf(rep->msg, sizeof rep->msg, "webcam: shm name %s is held by another user (%s)", name, strerror(err));
}

int scw_shm_create(scw_shm_writer *w, const char *name, uint64_t bytes, scw_shm_report *rep) {
    memset(w, 0, sizeof *w);
    w->fd = -1;
    memset(rep, 0, sizeof *rep);
    rep->owner = -1;
    if (!name || strlen(name) > SCW_NAME_MAX || name[0] != '/' || bytes < SCW_HDR || bytes > SCW_MAX_OBJECT) {
        snprintf(rep->msg, sizeof rep->msg, "webcam: bad shm name or size");
        return SCW_E_ARG;
    }
    uid_t uid = getuid();
    // 1. remove a stale object (a crashed helper's, or ours from a previous geometry)
    rep->unlink_errno = shm_unlink(name) == 0 ? 0 : errno;
    if (rep->unlink_errno && rep->unlink_errno != ENOENT) {
        scw_refuse_owner(rep, name, scw_owner_of(name), rep->unlink_errno);
        return SCW_E_SYS(rep->unlink_errno);
    }
    // 2. a new object, never an existing one. The mode is passed explicitly: macOS doesn't apply the umask to shm
    //    objects (the object gets exactly this mode), and where a umask applies it can only remove bits from 0600
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        int e = errno;
        if (e == EEXIST || e == EACCES) scw_refuse_owner(rep, name, scw_owner_of(name), e);
        else snprintf(rep->msg, sizeof rep->msg, "webcam: shm_open %s: %s", name, strerror(e));
        return SCW_E_SYS(e);
    }
    // 3. belt and braces where the platform supports it (macOS refuses fchmod on a shm descriptor with EINVAL)
    rep->fchmod_errno = fchmod(fd, 0600) == 0 ? 0 : errno;
    // 4. the guarantee: refuse to publish unless this user owns it and nobody else has any access bits
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        snprintf(rep->msg, sizeof rep->msg, "webcam: fstat %s: %s", name, strerror(e));
        close(fd); shm_unlink(name);
        return SCW_E_SYS(e);
    }
    rep->mode = (unsigned)(st.st_mode & 07777);
    if (!scw_shm_check_stat(&st, uid, name, rep->msg, sizeof rep->msg)) {
        rep->owner = (long)st.st_uid;
        close(fd);
        if (st.st_uid == uid) shm_unlink(name);
        return SCW_E_SYS(EPERM);
    }
    if (ftruncate(fd, (off_t)bytes) != 0) {
        int e = errno;
        snprintf(rep->msg, sizeof rep->msg, "webcam: ftruncate %s to %llu: %s", name, (unsigned long long)bytes, strerror(e));
        close(fd); shm_unlink(name);
        return SCW_E_SYS(e);
    }
    void *m = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        int e = errno;
        snprintf(rep->msg, sizeof rep->msg, "webcam: mmap %s: %s", name, strerror(e));
        close(fd); shm_unlink(name);
        return SCW_E_SYS(e);
    }
    snprintf(w->name, sizeof w->name, "%s", name);
    w->fd = fd; w->h = m; w->len = bytes;
    return SCW_OK;
}

// 1 if the object at `name` is owned by this user and carries `epoch` (macOS reports st_dev = st_ino = 0 for every
// shm object, so the epoch is the only identity).
static int scw_name_has_epoch(const char *name, uint64_t epoch) {
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) return 0;
    struct stat st;
    int same = 0;
    if (fstat(fd, &st) == 0 && st.st_uid == getuid() && st.st_size >= (off_t)SCW_HDR) {
        void *m = mmap(NULL, SCW_HDR, PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            same = __atomic_load_n(&((const scw_header *)m)->epoch, __ATOMIC_ACQUIRE) == epoch;
            munmap(m, SCW_HDR);
        }
    }
    close(fd);
    return same;
}

void scw_shm_retire(scw_shm_writer *w, uint32_t final_state) {
    if (!w->h) return;
    // the epoch and geometry come from the writer's private copy, never from the shared header
    if (w->ring.h) {
        scw_writer_state(&w->ring, final_state);
        scw_writer_clear(&w->ring);
    }
    // unlink only if the name still holds this object (after a supersede it already holds the new one). The private
    // epoch is stored into this object's header first, so a header another process rewrote still identifies as ours;
    // a different object at the name keeps its own epoch.
    __atomic_store_n(&w->h->epoch, w->ring.epoch, __ATOMIC_SEQ_CST);
    if (scw_name_has_epoch(w->name, w->ring.epoch)) shm_unlink(w->name);
    munmap(w->h, (size_t)w->len);
    if (w->fd >= 0) close(w->fd);
    w->h = NULL; w->fd = -1; w->len = 0;
    memset(&w->ring, 0, sizeof w->ring);
}

int scw_shm_open_ro(const char *name, const uint32_t *accepted, unsigned naccepted, const void **map, uint64_t *len,
                    scw_geom *g, long *owner, char *why, size_t cap) {
    *map = NULL; *len = 0; *owner = -1;
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT) snprintf(why, cap, "webcam: no ring at shm name %s (helper not running)", name);
        else snprintf(why, cap, "webcam: shm name %s: %s", name, strerror(e));
        return SCW_E_SYS(e);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        snprintf(why, cap, "webcam: fstat %s: %s", name, strerror(e));
        return SCW_E_SYS(e);
    }
    if (!scw_shm_check_stat(&st, getuid(), name, why, cap)) {
        *owner = (long)st.st_uid;
        close(fd);
        return SCW_E_SYS(EPERM);
    }
    if (st.st_size < (off_t)SCW_HDR || (uint64_t)st.st_size > SCW_MAX_OBJECT) {
        close(fd);
        snprintf(why, cap, "webcam: shm name %s has size %lld, not a ring", name, (long long)st.st_size);
        return st.st_size < (off_t)SCW_HDR ? SCW_E_SHORT : SCW_E_SIZE;
    }
    uint64_t n = (uint64_t)st.st_size;
    void *m = mmap(NULL, (size_t)n, PROT_READ, MAP_SHARED, fd, 0);
    int e = errno;
    close(fd);
    if (m == MAP_FAILED) {
        snprintf(why, cap, "webcam: mmap %s: %s", name, strerror(e));
        return SCW_E_SYS(e);
    }
    uint32_t found = 0;
    int rc = scw_validate(m, n, accepted, naccepted, g, &found);
    if (rc != SCW_OK) {
        munmap(m, (size_t)n);
        if (rc == SCW_E_VERSION) scw_version_message(why, cap, found, accepted, naccepted);
        else snprintf(why, cap, "webcam: %s (%s)", scw_strerror(rc), name);
        return rc;
    }
    *map = m; *len = n;
    return SCW_OK;
}

static int scw_reader_is_dead(const scw_shm_reader *r, uint64_t epoch) {
    unsigned n = r->ndead < SCW_READER_DEAD ? r->ndead : SCW_READER_DEAD;
    for (unsigned i = 0; i < n; i++) if (r->dead[i] == epoch) return 1;
    return 0;
}

void scw_shm_reader_init(scw_shm_reader *r, const char *name, const uint32_t *accepted, unsigned naccepted) {
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    for (unsigned i = 0; i < naccepted && i < 4; i++) r->accepted[r->naccepted++] = accepted[i];
    r->owner = -1;
    r->status = SCW_E_NOTREADY;
    snprintf(r->why, sizeof r->why, "webcam: not opened yet");
}

void scw_shm_reader_close(scw_shm_reader *r) {
    if (r->map) munmap((void *)r->map, (size_t)r->len);
    r->map = NULL; r->len = 0;
}

int scw_shm_reader_poll(scw_shm_reader *r, uint64_t now_ns, scw_peek_t *peek) {
    scw_peek_t local;
    if (!peek) peek = &local;
    memset(peek, 0, sizeof *peek);
    if (r->map) {
        int rc = scw_peek(r->map, &r->g, peek);
        if (rc != SCW_OK) {
            scw_shm_reader_close(r);
            r->next_open_ns = now_ns;
        } else if (peek->state == SCW_STATE_SUPERSEDED || peek->state == SCW_STATE_STOPPED) {
            r->dead[r->ndead++ % SCW_READER_DEAD] = r->g.epoch;   // never adopted again
            scw_shm_reader_close(r);
            if (peek->state == SCW_STATE_SUPERSEDED) r->next_open_ns = now_ns;   // the new object is already there
            snprintf(r->why, sizeof r->why, "webcam: helper %s", scw_state_name(peek->state));
        } else if (scw_is_live(peek, now_ns)) {
            return 1;
        }
    }
    // offline: re-open the name at most once per SCW_REOPEN_NS, so a restarted helper, an unlinked object or an idle
    // exit never pins the reader to a dead mapping
    if (now_ns >= r->next_open_ns) {
        r->next_open_ns = now_ns + SCW_REOPEN_NS;
        const void *m; uint64_t n; scw_geom g; long owner; char why[sizeof r->why];
        int rc = scw_shm_open_ro(r->name, r->accepted, r->naccepted, &m, &n, &g, &owner, why, sizeof why);
        r->status = rc; r->owner = owner;
        if (rc == SCW_OK && scw_reader_is_dead(r, g.epoch)) {
            // still the retired object (the helper hasn't unlinked it yet): wait for the next attempt
            munmap((void *)m, (size_t)n);
            if (!r->map) {
                snprintf(r->why, sizeof r->why, "webcam: waiting for the helper's new ring (%s)", r->name);
                return 0;
            }
        } else if (rc == SCW_OK && r->max_w && r->max_h &&
                   !((g.width <= r->max_w && g.height <= r->max_h) || (g.width <= r->max_h && g.height <= r->max_w))) {
            // valid, but larger than this reader will copy and draw each tick: offline, never mapped
            munmap((void *)m, (size_t)n);
            r->status = SCW_E_LIMIT;
            snprintf(r->why, sizeof r->why, "webcam: ring %ux%u is larger than this engine accepts (%ux%u) (%s)", g.width,
                     g.height, r->max_w, r->max_h, r->name);
            return 0;
        } else if (rc == SCW_OK) {
            if (!r->map || g.epoch != r->g.epoch) {
                scw_shm_reader_close(r);
                r->map = m; r->len = n; r->g = g; r->opens++;
            } else {
                munmap((void *)m, (size_t)n);                     // the same object we already map
            }
        } else {
            snprintf(r->why, sizeof r->why, "%s", why);
            if (!r->map) return 0;
            // the mapping held here is not live; why the name can't be opened (a refused owner, a newer ring
            // version) says more than "stale"
            if (rc != SCW_E_SYS(ENOENT) && rc != SCW_E_NOTREADY) return 0;
        }
    }
    if (!r->map) return 0;
    if (scw_peek(r->map, &r->g, peek) != SCW_OK) {
        snprintf(r->why, sizeof r->why, "webcam: ring replaced under the mapping (%s)", r->name);
        return 0;
    }
    if (scw_is_live(peek, now_ns)) return 1;
    if (peek->state != SCW_STATE_RUNNING) snprintf(r->why, sizeof r->why, "webcam: helper %s", scw_state_name(peek->state));
    else snprintf(r->why, sizeof r->why, "webcam: no fresh frame (%s)", peek->latest ? "stale" : "none yet");
    return 0;
}
