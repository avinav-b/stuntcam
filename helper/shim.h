// SPDX-License-Identifier: Apache-2.0
// shim.h: C helpers for the camera helper (Swift bridging header). The ring itself (layout, seqlock writer, shm object,
// pid file) is engine/src/webcam/ring.{h,c} and ring_shm.{h,c}, shared with the engine; docs/webcam-ring.md is the spec.
#pragma once
#include "ring.h"
#include "ring_shm.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Host clock ns (the ring's clock: mach_absolute_time in ns, shared with simulator processes).
static inline uint64_t sch_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// 64 random bits, never 0 (a ring epoch).
static inline uint64_t sch_epoch(void) {
    uint64_t e = 0;
    while (!e) arc4random_buf(&e, sizeof e);
    return e;
}

// The helper lock (spec §8): flock(LOCK_EX) on `path`, opened O_CREAT|O_NOFOLLOW 0600, retried every 10 ms for 250 ms.
// Returns the descriptor (held for the helper's life), or -1 with errno (EWOULDBLOCK: another helper holds it).
static inline int sch_lock(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    for (int i = 0; i <= 25; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
        if (errno != EWOULDBLOCK && errno != EINTR) break;
        if (i < 25) usleep(10000);
    }
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

// Writes `len` bytes to `path` through `path`.tmp and a rename (mode 0600), so a reader never sees a partial file.
static inline int sch_write_file(const char *path, const char *text, size_t len) {
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        off += (size_t)n;
    }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

// Reads at most cap-1 bytes of a regular file (O_NOFOLLOW) into out, NUL-terminated. Returns the length or -1.
static inline long sch_read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    return n;
}

// Seconds since the file's last modification, or -1.
static inline double sch_age_s(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)(now.tv_sec - st.st_mtimespec.tv_sec) + (now.tv_nsec - st.st_mtimespec.tv_nsec) / 1e9;
}

// 1 while `pid` exists (kill(pid, 0) succeeds, or fails only for lack of permission).
static inline int sch_pid_alive(int pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 || errno == EPERM;
}

static inline void sch_copy_rows(uint8_t *dst, size_t dbpr, const uint8_t *src, size_t sbpr, size_t row, uint32_t rows) {
    if (dbpr == sbpr && dbpr == row) { memcpy(dst, src, row * rows); return; }
    for (uint32_t y = 0; y < rows; y++) memcpy(dst + (size_t)y * dbpr, src + (size_t)y * sbpr, row);
}

// ---- latency stamp: an 8x8 grid of black/white cells in the centre of a BGRA frame, side = height/2.
// 64 bits, bit k at row k/8, column k%8 (white = 1): seq (20 bits) | capture time on the host clock in 100 us units
// (28 bits, wraps after 7.4 h) | a 16-bit check. fixtures/plain (M5APP_MODE=webcam) decodes it under any rotation,
// mirror and scale, from video frames, photos and movies.
static inline uint32_t sch_stamp_check(uint32_t seq20, uint32_t t28) {
    uint32_t x = (seq20 << 12) ^ (t28 * 2654435761u);
    x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12; x *= 0x297a2d39u; x ^= x >> 15;
    return x & 0xffff;
}
static inline void sch_stamp(uint8_t *p, uint32_t bpr, uint32_t w, uint32_t h, uint64_t seq, uint64_t capture_ns) {
    uint32_t s = (uint32_t)(seq & 0xfffff), t = (uint32_t)((capture_ns / 100000) & 0xfffffff);
    uint64_t payload = (uint64_t)s | ((uint64_t)t << 20) | ((uint64_t)sch_stamp_check(s, t) << 48);
    uint32_t cell = (h / 16) & ~1u, side = cell * 8;
    if (!cell || side > w) return;
    uint32_t x0 = ((w - side) / 2) & ~1u, y0 = ((h - side) / 2) & ~1u;
    for (int k = 0; k < 64; k++) {
        uint32_t v = ((payload >> k) & 1) ? 0xffffffffu : 0xff000000u, cx = x0 + (k % 8) * cell, cy = y0 + (k / 8) * cell;
        for (uint32_t y = cy; y < cy + cell; y++) {
            uint32_t *row = (uint32_t *)(p + (size_t)y * bpr) + cx;
            for (uint32_t x = 0; x < cell; x++) row[x] = v;
        }
    }
}

// ---- synthetic pattern (BGRA): eight colour bars scrolling 8 px per frame, and a grey ramp band moving down 6 px per frame.
typedef struct { uint32_t w; uint32_t *bars, *ramp; } sch_synth;
static inline void sch_synth_free(sch_synth *s) { free(s->bars); free(s->ramp); s->bars = s->ramp = NULL; s->w = 0; }
static inline void sch_synth_draw(sch_synth *s, uint8_t *p, uint32_t bpr, uint32_t w, uint32_t h, uint64_t frame) {
    if (s->w != w || !s->bars) {
        sch_synth_free(s);
        static const uint32_t bars[8] = {0xffc0c0c0, 0xffc0c000, 0xff00c0c0, 0xff00c000, 0xffc000c0, 0xffc00000, 0xff0000c0, 0xff101010};
        s->bars = malloc(2 * (size_t)w * 4); s->ramp = malloc(2 * (size_t)w * 4);
        if (!s->bars || !s->ramp) { sch_synth_free(s); return; }
        s->w = w;
        for (uint32_t x = 0; x < 2 * w; x++) {
            s->bars[x] = bars[(x % w) * 8 / w];
            s->ramp[x] = 0xff000000u | (((x % w) * 255 / w) * 0x010101u);
        }
    }
    uint32_t off = (uint32_t)((frame * 8) % w), band = (uint32_t)((frame * 6) % h), bandH = h / 10;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src = ((y - band) < bandH ? s->ramp : s->bars) + off;
        memcpy(p + (size_t)y * bpr, src, (size_t)w * 4);
    }
}

// The pid file record (spec §8) for this helper: ring version 1 only.
static inline void sch_pidfile_fill(scw_pidfile *p, int pid, const char *sha, int auth, uint32_t state, const char *camera,
                                    uint64_t started) {
    memset(p, 0, sizeof *p);
    p->pid = pid;
    snprintf(p->sha, sizeof p->sha, "%s", sha);
    p->rings[0] = 1; p->nrings = 1;
    p->auth = auth; p->state = state;
    snprintf(p->camera, sizeof p->camera, "%s", camera ? camera : "");
    p->started = started;
}
static inline scw_writer *sch_ring(scw_shm_writer *w) { return &w->ring; }
static inline const char *sch_report_msg(const scw_shm_report *r) { return r->msg; }
