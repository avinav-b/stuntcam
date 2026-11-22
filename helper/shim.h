// SPDX-License-Identifier: Apache-2.0
// shim.h: C helpers for the camera helper (Swift bridging header). The ring itself (layout, seqlock writer, shm
// object, pid file) is engine/src/webcam/ring.{h,c} and ring_shm.{h,c}, shared with the engine; docs/webcam-ring.md is
// the spec.
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
    uint64_t epoch = 0;
    while (!epoch) arc4random_buf(&epoch, sizeof epoch);
    return epoch;
}

// The helper lock is retried every SCH_LOCK_RETRY_US for SCH_LOCK_RETRIES times (250 ms), because a momentary holder
// is not necessarily another helper.
enum { SCH_LOCK_RETRIES = 25, SCH_LOCK_RETRY_US = 10000 };

// The helper lock (docs/webcam-ring.md, "The pid file"): flock(LOCK_EX) on `path`, opened O_CREAT|O_NOFOLLOW 0600.
// Returns the descriptor, which the helper holds for its whole life, or -1 with errno (EWOULDBLOCK: another helper
// holds it).
static inline int sch_lock(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    for (int attempt = 0; attempt <= SCH_LOCK_RETRIES; attempt++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
        if (errno != EWOULDBLOCK && errno != EINTR) break;
        if (attempt < SCH_LOCK_RETRIES) usleep(SCH_LOCK_RETRY_US);
    }
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

// The longest temporary path sch_write_file makes: a path of about 1 KiB and the ".tmp" suffix.
enum { SCH_TMP_PATH_MAX = 1100 };

// Writes `len` bytes to `path` through `path`.tmp and a rename (mode 0600), so a reader never sees a partial file.
static inline int sch_write_file(const char *path, const char *text, size_t len) {
    char tmp[SCH_TMP_PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, text + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        written += (size_t)n;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

// Reads at most cap-1 bytes of a regular file (O_NOFOLLOW) into out, NUL-terminated. Returns the length or -1.
static inline long sch_read_small(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
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

// Copies `rows` rows of `row_bytes` bytes from src (src_stride bytes per row) to dst (dst_stride bytes per row); one
// memcpy when both are packed.
static inline void sch_copy_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride,
                                 size_t row_bytes, uint32_t rows) {
    if (dst_stride == src_stride && dst_stride == row_bytes) {
        memcpy(dst, src, row_bytes * rows);
        return;
    }
    for (uint32_t y = 0; y < rows; y++) {
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
    }
}

// ---- The latency stamp: an 8x8 grid of black and white cells in the centre of a BGRA frame, half the frame's height
// on a side. It holds 64 bits, bit k at row k/8 and column k%8 (white = 1): the frame's seq (20 bits), its capture
// time on the host clock in 100 us units (28 bits, so it wraps after 7.4 h) and a 16-bit check. fixtures/plain
// (M5APP_MODE=webcam) decodes it under any rotation, mirror and scale, from video frames, photos and movies.
enum {
    SCH_STAMP_SIDE_CELLS = 8,             // cells per side of the grid
    SCH_STAMP_TIME_SHIFT = 20,            // the capture time's first bit; the seq takes the bits below it
    SCH_STAMP_CHECK_SHIFT = 48,           // the check's first bit
    SCH_STAMP_SEQ_MASK = 0xfffff,         // 20 bits
    SCH_STAMP_TIME_MASK = 0xfffffff,      // 28 bits
    SCH_STAMP_TIME_UNIT_NS = 100000,      // the capture time counts 100 us units
};
#define SCH_STAMP_WHITE 0xffffffffu      // opaque white and black BGRA pixels
#define SCH_STAMP_BLACK 0xff000000u

// The stamp's 16-bit check over its seq and time fields: an integer hash (multiply and xor-shift rounds with odd
// constants), so that a misread bit almost never yields a valid stamp.
static inline uint32_t sch_stamp_check(uint32_t seq20, uint32_t t28) {
    uint32_t x = (seq20 << 12) ^ (t28 * 2654435761u);
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    x *= 0x297a2d39u;
    x ^= x >> 15;
    return x & 0xffff;
}

// Draws the stamp for frame `seq`, captured at `capture_ns`, into the BGRA frame at pixels (bpr bytes per row). The
// cells are an even number of pixels wide and start at even coordinates. A frame too narrow for the grid gets no
// stamp.
static inline void sch_stamp(uint8_t *pixels, uint32_t bpr, uint32_t width, uint32_t height, uint64_t seq,
                             uint64_t capture_ns) {
    uint32_t seq20 = (uint32_t)(seq & SCH_STAMP_SEQ_MASK);
    uint32_t t28 = (uint32_t)((capture_ns / SCH_STAMP_TIME_UNIT_NS) & SCH_STAMP_TIME_MASK);
    uint64_t payload = (uint64_t)seq20 | ((uint64_t)t28 << SCH_STAMP_TIME_SHIFT) |
                       ((uint64_t)sch_stamp_check(seq20, t28) << SCH_STAMP_CHECK_SHIFT);
    uint32_t cell = (height / (2 * SCH_STAMP_SIDE_CELLS)) & ~1u;
    uint32_t side = cell * SCH_STAMP_SIDE_CELLS;
    if (!cell || side > width) return;
    uint32_t left = ((width - side) / 2) & ~1u;
    uint32_t top = ((height - side) / 2) & ~1u;
    for (int bit = 0; bit < SCH_STAMP_SIDE_CELLS * SCH_STAMP_SIDE_CELLS; bit++) {
        uint32_t colour = ((payload >> bit) & 1) ? SCH_STAMP_WHITE : SCH_STAMP_BLACK;
        uint32_t cell_x = left + (bit % SCH_STAMP_SIDE_CELLS) * cell;
        uint32_t cell_y = top + (bit / SCH_STAMP_SIDE_CELLS) * cell;
        for (uint32_t y = cell_y; y < cell_y + cell; y++) {
            uint32_t *row = (uint32_t *)(pixels + (size_t)y * bpr) + cell_x;
            for (uint32_t x = 0; x < cell; x++) row[x] = colour;
        }
    }
}

// ---- The synthetic pattern (BGRA): eight colour bars scrolling sideways, and a grey ramp band moving down.
enum {
    SCH_SYNTH_BAR_COUNT = 8,
    SCH_SYNTH_SCROLL_PX = 8,        // the bars scroll this far per frame
    SCH_SYNTH_BAND_STEP_PX = 6,     // the ramp band moves down this far per frame
    SCH_SYNTH_BAND_FRACTION = 10,   // the band is 1/10 of the frame's height
};
#define SCH_SYNTH_OPAQUE 0xff000000u   // the alpha byte of an opaque BGRA pixel
#define SCH_SYNTH_GREY_STEP 0x010101u  // one step of grey: the same increment in B, G and R

typedef struct {
    uint32_t w;      // the width bars and ramp were made for (0: none yet)
    uint32_t *bars;  // two widths of the colour bars, so a scrolled row is one contiguous copy
    uint32_t *ramp;  // two widths of the grey ramp
} sch_synth;

static inline void sch_synth_free(sch_synth *synth) {
    free(synth->bars);
    free(synth->ramp);
    synth->bars = NULL;
    synth->ramp = NULL;
    synth->w = 0;
}

// Draws frame number `frame` of the pattern into the BGRA frame at pixels (bpr bytes per row). The bars and the ramp
// are made once per width. Nothing is drawn if they can't be allocated.
static inline void sch_synth_draw(sch_synth *synth, uint8_t *pixels, uint32_t bpr, uint32_t width, uint32_t height,
                                  uint64_t frame) {
    if (synth->w != width || !synth->bars) {
        sch_synth_free(synth);
        static const uint32_t bar_colours[SCH_SYNTH_BAR_COUNT] = {0xffc0c0c0, 0xffc0c000, 0xff00c0c0, 0xff00c000,
                                                                  0xffc000c0, 0xffc00000, 0xff0000c0, 0xff101010};
        synth->bars = malloc(2 * (size_t)width * 4);
        synth->ramp = malloc(2 * (size_t)width * 4);
        if (!synth->bars || !synth->ramp) {
            sch_synth_free(synth);
            return;
        }
        synth->w = width;
        for (uint32_t x = 0; x < 2 * width; x++) {
            synth->bars[x] = bar_colours[(x % width) * SCH_SYNTH_BAR_COUNT / width];
            synth->ramp[x] = SCH_SYNTH_OPAQUE | (((x % width) * 255 / width) * SCH_SYNTH_GREY_STEP);
        }
    }
    uint32_t scroll = (uint32_t)((frame * SCH_SYNTH_SCROLL_PX) % width);
    uint32_t band_top = (uint32_t)((frame * SCH_SYNTH_BAND_STEP_PX) % height);
    uint32_t band_height = height / SCH_SYNTH_BAND_FRACTION;
    for (uint32_t y = 0; y < height; y++) {
        // unsigned: rows above the band wrap to large values and so fall outside it
        const uint32_t *src = ((y - band_top) < band_height ? synth->ramp : synth->bars) + scroll;
        memcpy(pixels + (size_t)y * bpr, src, (size_t)width * 4);
    }
}

// The pid file record (docs/webcam-ring.md, "The pid file") for this helper, which serves ring version 1 only.
static inline void sch_pidfile_fill(scw_pidfile *record, int pid, const char *sha, int auth, uint32_t state,
                                    const char *camera, uint64_t started) {
    memset(record, 0, sizeof *record);
    record->pid = pid;
    snprintf(record->sha, sizeof record->sha, "%s", sha);
    record->rings[0] = 1;
    record->nrings = 1;
    record->auth = auth;
    record->state = state;
    snprintf(record->camera, sizeof record->camera, "%s", camera ? camera : "");
    record->started = started;
}

// Field accessors Swift can't express directly: the writer inside a shm writer, and a report's message buffer.
static inline scw_writer *sch_ring(scw_shm_writer *writer) { return &writer->ring; }
static inline const char *sch_report_msg(const scw_shm_report *report) { return report->msg; }
