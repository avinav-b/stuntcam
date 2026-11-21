// SPDX-License-Identifier: Apache-2.0
// ring.c: validation, seqlock reads and writes, names, messages and the pid file of the webcam ring (ring.h,
// docs/webcam-ring.md). Plain C11 with no allocation and no I/O: the shm side is ring_shm.c.
//
// Every value read from the shared header is untrusted. scw_validate checks it once per mapping with overflow-checked
// arithmetic and copies what the reader needs into a private scw_geom; after that, pixel addresses come only from the
// private copy and the mapping length, so a writer that rewrites the header can at worst produce torn or stale frames,
// never an out-of-bounds read. The writer works the same way round: scw_writer holds its geometry, epoch and last seq,
// and it only ever stores into the shared header, so a process that rewrites the header can't steer its writes.
#include "ring.h"
#include <stdio.h>
#include <string.h>

// Shared fields are read and written with atomic builtins: the other side may change them at any moment. These macros
// and every static below carry the scw prefix, and the macros are #undef'd at the end, because this file is included
// into the engine's single translation unit.
#define SCW_LD32(p) __atomic_load_n((const uint32_t *)(p), __ATOMIC_ACQUIRE)
#define SCW_LD64(p) __atomic_load_n((const uint64_t *)(p), __ATOMIC_ACQUIRE)
#define SCW_LD64R(p) __atomic_load_n((const uint64_t *)(p), __ATOMIC_RELAXED)
#define SCW_LD32R(p) __atomic_load_n((const uint32_t *)(p), __ATOMIC_RELAXED)
#define SCW_ST32(p, v) __atomic_store_n((uint32_t *)(p), (v), __ATOMIC_RELEASE)
#define SCW_ST64(p, v) __atomic_store_n((uint64_t *)(p), (v), __ATOMIC_RELEASE)
#define SCW_ST64R(p, v) __atomic_store_n((uint64_t *)(p), (v), __ATOMIC_RELAXED)

static int scw_accepts(const uint32_t *accepted, unsigned n, uint32_t v) {
    for (unsigned i = 0; accepted && i < n; i++) if (accepted[i] == v) return 1;
    return 0;
}

// Copies a device name out of shared memory: stops at NUL or SCW_DEVICE_LEN-1 bytes, keeps well-formed UTF-8
// (no overlong forms, surrogates or code points above U+10FFFF, no sequence cut at the end), and replaces every other
// byte, and every control character, with '?'. The result is always terminated and safe to log.
static void scw_copy_device(char out[SCW_DEVICE_LEN], const char *shared) {
    unsigned char b[SCW_DEVICE_LEN];
    unsigned n = 0;
    while (n < SCW_DEVICE_LEN - 1) {
        b[n] = (unsigned char)__atomic_load_n(&shared[n], __ATOMIC_RELAXED);   // one read per byte
        if (!b[n]) break;
        n++;
    }
    unsigned i = 0, o = 0;
    while (i < n) {
        unsigned c = b[i], len = 0;
        uint32_t cp = 0;
        if (c < 0x80) { len = 1; cp = c; }
        else if (c >= 0xc2 && c <= 0xdf) { len = 2; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { len = 3; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { len = 4; cp = c & 0x07; }
        int ok = len > 0 && i + len <= n;
        for (unsigned k = 1; ok && k < len; k++) {
            if ((b[i + k] & 0xc0) != 0x80) ok = 0;
            else cp = cp << 6 | (b[i + k] & 0x3f);
        }
        if (ok && ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff))))
            ok = 0;
        if (ok && len == 1 && (c < 0x20 || c == 0x7f)) ok = 0;
        if (ok && len == 2 && cp < 0xa0) ok = 0;   // C1 controls (U+0080-U+009F)
        if (!ok) { out[o++] = '?'; i++; continue; }
        for (unsigned k = 0; k < len; k++) out[o++] = (char)b[i + k];
        i += len;
    }
    out[o] = 0;
}

uint64_t scw_object_bytes(uint32_t height, uint32_t bpr0, uint32_t nslots, uint64_t *slot_bytes) {
    uint64_t plane, slot, all, total;
    if (!height || !bpr0 || !nslots || nslots > SCW_MAX_SLOTS) return 0;
    if (__builtin_mul_overflow((uint64_t)bpr0, (uint64_t)height, &plane)) return 0;
    if (__builtin_add_overflow(plane, (uint64_t)SCW_SLOT_ALIGN - 1, &slot)) return 0;
    slot &= ~(uint64_t)(SCW_SLOT_ALIGN - 1);
    if (__builtin_mul_overflow(slot, (uint64_t)nslots, &all)) return 0;
    if (__builtin_add_overflow(all, (uint64_t)SCW_HDR, &total)) return 0;
    if (total > SCW_MAX_OBJECT) return 0;
    if (slot_bytes) *slot_bytes = slot;
    return total;
}

int scw_validate(const void *map, uint64_t map_len, const uint32_t *accepted, unsigned naccepted, scw_geom *g,
                 uint32_t *found_version) {
    if (found_version) *found_version = 0;
    if (!map || !g) return SCW_E_ARG;
    if (map_len < SCW_HDR) return SCW_E_SHORT;
    if (map_len > SCW_MAX_OBJECT) return SCW_E_SIZE;
    const scw_header *h = map;
    uint32_t magic = SCW_LD32(&h->magic);       // acquire: pairs with scw_writer_publish, so the fields below are complete
    if (magic == 0) return SCW_E_NOTREADY;
    if (magic != SCW_MAGIC) return SCW_E_MAGIC;
    uint32_t version = SCW_LD32R(&h->version);
    if (found_version) *found_version = version;
    if (!scw_accepts(accepted, naccepted, version)) return SCW_E_VERSION;

    // One snapshot of every geometry field; the checks and the private copy use only these locals.
    scw_geom t;
    memset(&t, 0, sizeof t);
    uint32_t headerBytes = SCW_LD32R(&h->headerBytes), state = SCW_LD32R(&h->state), bpr1 = SCW_LD32R(&h->bpr1);
    uint32_t flags = SCW_LD32R(&h->flags);
    uint64_t plane1Off = SCW_LD64R(&h->plane1Off);
    t.version = version;
    t.epoch = SCW_LD64R(&h->epoch);
    t.width = SCW_LD32R(&h->width);
    t.height = SCW_LD32R(&h->height);
    t.format = SCW_LD32R(&h->format);
    t.nslots = SCW_LD32R(&h->nslots);
    t.bpr0 = SCW_LD32R(&h->bpr0);
    t.slotBytes = SCW_LD64R(&h->slotBytes);
    t.totalBytes = SCW_LD64R(&h->totalBytes);
    t.fpsMilli = SCW_LD32R(&h->fpsMilli);
    t.producerPid = (int32_t)SCW_LD32R(&h->producerPid);

    if (state >= SCW_STATE_COUNT) return SCW_E_STATE;
    if (t.format != SCW_FORMAT_BGRA) return SCW_E_FORMAT;
    if (headerBytes != SCW_HDR || t.epoch == 0 || flags != 0) return SCW_E_GEOMETRY;
    if (t.nslots < 1 || t.nslots > SCW_MAX_SLOTS) return SCW_E_GEOMETRY;
    if (t.width < 1 || t.width > SCW_MAX_DIM || t.height < 1 || t.height > SCW_MAX_DIM) return SCW_E_GEOMETRY;
    uint64_t row, plane, all, total;
    if (__builtin_mul_overflow((uint64_t)t.width, (uint64_t)4, &row)) return SCW_E_GEOMETRY;
    if ((uint64_t)t.bpr0 < row || (t.bpr0 & 3u)) return SCW_E_GEOMETRY;
    if (t.slotBytes == 0 || (t.slotBytes % SCW_SLOT_ALIGN) != 0) return SCW_E_GEOMETRY;
    if (__builtin_mul_overflow((uint64_t)t.bpr0, (uint64_t)t.height, &plane)) return SCW_E_GEOMETRY;
    if (plane > t.slotBytes) return SCW_E_GEOMETRY;
    if (plane1Off != 0 || bpr1 != 0) return SCW_E_GEOMETRY;     // BGRA is single-plane
    if (__builtin_mul_overflow(t.slotBytes, (uint64_t)t.nslots, &all)) return SCW_E_SIZE;
    if (__builtin_add_overflow(all, (uint64_t)SCW_HDR, &total)) return SCW_E_SIZE;
    if (total != t.totalBytes) return SCW_E_GEOMETRY;
    if (total > map_len || total > SCW_MAX_OBJECT) return SCW_E_SIZE;
    t.mapBytes = map_len;
    scw_copy_device(t.device, h->device);
    *g = t;
    return SCW_OK;
}

// The identity fields that must not change for the life of a mapping.
static int scw_same_object(const scw_header *h, const scw_geom *g) {
    return SCW_LD32(&h->magic) == SCW_MAGIC && SCW_LD32R(&h->version) == g->version && SCW_LD64R(&h->epoch) == g->epoch;
}

int scw_peek(const void *map, const scw_geom *g, scw_peek_t *out) {
    const scw_header *h = map;
    memset(out, 0, sizeof *out);
    if (!scw_same_object(h, g)) return SCW_E_EPOCH;
    out->state = SCW_LD32(&h->state);
    uint64_t seq = SCW_LD64(&h->latest);
    out->latest = seq;
    if (!seq) return SCW_OK;
    uint32_t i = (uint32_t)(seq % g->nslots);                  // private nslots
    const scw_slot *s = &h->slot[i];
    uint64_t g0 = SCW_LD64(&s->gen);
    uint64_t cap = SCW_LD64R(&s->captureNs), pub = SCW_LD64R(&s->publishNs), sseq = SCW_LD64R(&s->seq);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t g1 = SCW_LD64R(&s->gen);
    if (g0 == g1 && sseq == seq && seq < (UINT64_MAX - 2) / 2 && g0 == 2 * seq + 2) {
        out->captureNs = cap; out->publishNs = pub; out->have_ts = 1;
    }
    return SCW_OK;
}

int scw_is_live(const scw_peek_t *p, uint64_t now_ns) {
    if (p->state != SCW_STATE_RUNNING || !p->latest || !p->have_ts) return 0;
    // a publish time in the future counts as fresh only within the liveness window (clock skew is not expected:
    // writer and readers share the host clock)
    uint64_t age = now_ns >= p->publishNs ? now_ns - p->publishNs : p->publishNs - now_ns;
    return age < SCW_LIVE_NS;
}

int scw_slot_begin(const void *map, const scw_geom *g, uint64_t last_seq, scw_slot_read *r) {
    const scw_header *h = map;
    memset(r, 0, sizeof *r);
    if (!scw_same_object(h, g)) return SCW_E_EPOCH;
    uint64_t seq = SCW_LD64(&h->latest);
    if (!seq || seq == last_seq) return SCW_NONE;
    if (seq >= (UINT64_MAX - 2) / 2) return SCW_E_TORN;          // gen would overflow: not a frame any writer makes
    uint32_t i = (uint32_t)(seq % g->nslots);
    const scw_slot *s = &h->slot[i];
    uint64_t g0 = SCW_LD64(&s->gen);
    if (g0 != 2 * seq + 2) return SCW_E_TORN;                     // being written, or already reused for a newer frame
    r->seq = seq; r->gen = g0; r->index = i;
    r->captureNs = SCW_LD64R(&s->captureNs);
    r->publishNs = SCW_LD64R(&s->publishNs);
    r->flags = SCW_LD32R(&s->flags);
    if (SCW_LD64R(&s->seq) != seq) return SCW_E_TORN;
    // offset = SCW_HDR + i*slotBytes; validated: SCW_HDR + nslots*slotBytes <= mapBytes and bpr0*height <= slotBytes
    r->pixels = (const uint8_t *)map + SCW_HDR + (uint64_t)i * g->slotBytes;
    return SCW_OK;
}

int scw_slot_end(const void *map, const scw_geom *g, const scw_slot_read *r) {
    const scw_header *h = map;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);                      // the pixel and meta reads happen before the re-check
    if (SCW_LD64R(&h->slot[r->index].gen) != r->gen) return SCW_E_TORN;
    if (!scw_same_object(h, g)) return SCW_E_EPOCH;
    return SCW_OK;
}

int scw_read_latest(const void *map, const scw_geom *g, uint64_t last_seq, void *dst, size_t dst_bpr, size_t dst_len,
                    scw_slot_read *r) {
    uint64_t row = (uint64_t)g->width * 4, need;
    if (!dst || dst_bpr < row || __builtin_mul_overflow((uint64_t)dst_bpr, (uint64_t)g->height, &need) || need > dst_len)
        return SCW_E_ARG;
    int rc = scw_slot_begin(map, g, last_seq, r);
    if (rc != SCW_OK) return rc;
    const uint8_t *src = r->pixels;
    uint8_t *d = dst;
    if (dst_bpr == g->bpr0) memcpy(d, src, (size_t)g->bpr0 * g->height);
    else for (uint32_t y = 0; y < g->height; y++) memcpy(d + (size_t)y * dst_bpr, src + (size_t)y * g->bpr0, (size_t)row);
    return scw_slot_end(map, g, r);
}

// ---- writer

int scw_header_init(scw_header *h, uint32_t width, uint32_t height, uint32_t bpr0, uint32_t nslots, uint64_t epoch,
                    uint32_t fpsMilli, int32_t pid, const char *device) {
    uint64_t slot, total;
    if (!h || !epoch) return SCW_E_ARG;
    if (width < 1 || width > SCW_MAX_DIM || height < 1 || height > SCW_MAX_DIM || (uint64_t)bpr0 < (uint64_t)width * 4 ||
        (bpr0 & 3u) || !(total = scw_object_bytes(height, bpr0, nslots, &slot)))
        return SCW_E_GEOMETRY;
    memset(h, 0, SCW_HDR > sizeof *h ? sizeof *h : SCW_HDR);
    h->version = SCW_VERSION;
    h->headerBytes = SCW_HDR;
    h->state = SCW_STATE_STARTING;
    h->epoch = epoch;
    h->width = width; h->height = height; h->format = SCW_FORMAT_BGRA; h->nslots = nslots; h->bpr0 = bpr0;
    h->slotBytes = slot; h->totalBytes = total; h->fpsMilli = fpsMilli; h->producerPid = pid;
    if (device) { strncpy(h->device, device, SCW_DEVICE_LEN - 1); h->device[SCW_DEVICE_LEN - 1] = 0; }
    return SCW_OK;
}

int scw_writer_init(scw_writer *w, void *map, uint64_t map_len, uint32_t width, uint32_t height, uint32_t bpr0,
                    uint32_t nslots, uint64_t epoch, uint32_t fpsMilli, int32_t pid, const char *device) {
    if (!w) return SCW_E_ARG;
    memset(w, 0, sizeof *w);
    if (!map || map_len < SCW_HDR) return SCW_E_ARG;
    scw_header tmp;                                               // built privately, then copied out
    int rc = scw_header_init(&tmp, width, height, bpr0, nslots, epoch, fpsMilli, pid, device);
    if (rc != SCW_OK) return rc;
    if (tmp.totalBytes > map_len) return SCW_E_SIZE;
    w->h = map; w->mapBytes = map_len;
    w->width = width; w->height = height; w->bpr0 = bpr0; w->nslots = nslots;
    w->slotBytes = tmp.slotBytes; w->totalBytes = tmp.totalBytes; w->epoch = epoch;
    memset(map, 0, SCW_HDR);
    memcpy(map, &tmp, sizeof tmp);                                // magic stays 0 until scw_writer_publish
    return SCW_OK;
}

void scw_writer_publish(scw_writer *w) { SCW_ST32(&w->h->magic, SCW_MAGIC); }

static uint8_t *scw_writer_slot(const scw_writer *w, uint32_t i) {
    return (uint8_t *)w->h + SCW_HDR + (uint64_t)i * w->slotBytes;   // i < nslots; init checked totalBytes <= mapBytes
}

uint8_t *scw_writer_begin(scw_writer *w, uint64_t seq) {
    if (!w->h || seq <= w->lastSeq || seq >= (UINT64_MAX - 2) / 2) return NULL;
    w->lastSeq = seq; w->busySeq = seq;
    uint32_t i = (uint32_t)(seq % w->nslots);
    SCW_ST64R(&w->h->slot[i].gen, 2 * seq + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);                      // the odd gen is visible before any pixel store
    return scw_writer_slot(w, i);
}

void scw_writer_end(scw_writer *w, uint64_t seq, uint64_t captureNs, uint64_t publishNs, uint32_t flags) {
    if (!w->h || !seq || seq != w->busySeq) return;
    w->busySeq = 0;
    uint32_t i = (uint32_t)(seq % w->nslots);
    scw_slot *s = &w->h->slot[i];
    SCW_ST64R(&s->seq, seq); SCW_ST64R(&s->captureNs, captureNs); SCW_ST64R(&s->publishNs, publishNs);
    __atomic_store_n(&s->flags, flags, __ATOMIC_RELAXED);
    SCW_ST64(&s->gen, 2 * seq + 2);                               // release: pixels and meta before the even gen
    SCW_ST64(&w->h->latest, seq);
}

void scw_writer_state(scw_writer *w, uint32_t state) { if (w->h) SCW_ST32(&w->h->state, state); }
void scw_writer_beat(scw_writer *w, uint64_t now_ns) { if (w->h) SCW_ST64(&w->h->beatNs, now_ns); }

void scw_writer_clear(scw_writer *w) {
    if (!w->h) return;
    SCW_ST64(&w->h->latest, 0);
    w->busySeq = 0;
    for (uint32_t i = 0; i < w->nslots; i++) {
        scw_slot *s = &w->h->slot[i];
        // odd: a reader never accepts an odd gen, so one mid-copy discards its copy
        SCW_ST64R(&s->gen, 2 * (w->lastSeq + 1) + 1);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memset(scw_writer_slot(w, i), 0, (size_t)w->slotBytes);
        SCW_ST64R(&s->seq, 0); SCW_ST64R(&s->captureNs, 0); SCW_ST64R(&s->publishNs, 0);
        SCW_ST64(&s->gen, 0);
    }
}

// ---- names

void scw_cam_hash(const char *home, const char *unique_id, char out[9]) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char *p = (const unsigned char *)(home ? home : ""); *p; p++) { h ^= *p; h *= 0x100000001b3ull; }
    h ^= 0; h *= 0x100000001b3ull;
    for (const unsigned char *p = (const unsigned char *)(unique_id ? unique_id : ""); *p; p++) { h ^= *p; h *= 0x100000001b3ull; }
    snprintf(out, 9, "%08x", (unsigned)(h & 0xffffffffu));
}

int scw_shm_name(char *out, size_t cap, unsigned long uid, const char *cam_hash, uint32_t version) {
    if (!out || !cap || !cam_hash || strlen(cam_hash) != 8 || !version) return SCW_E_ARG;
    for (int i = 0; i < 8; i++) {
        char c = cam_hash[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return SCW_E_ARG;
    }
    char buf[64];
    int n = snprintf(buf, sizeof buf, "/scw.%lu.%s.v%u", uid, cam_hash, version);
    if (n < 0 || (size_t)n > SCW_NAME_MAX || (size_t)n >= cap) return SCW_E_ARG;
    memcpy(out, buf, (size_t)n + 1);
    return n;
}

// ---- messages

const char *scw_strerror(int rc) {
    switch (rc) {
    case SCW_OK: return "ok";
    case SCW_NONE: return "no new frame";
    case SCW_E_SHORT: return "object shorter than the ring header";
    case SCW_E_NOTREADY: return "ring still being created";
    case SCW_E_MAGIC: return "not a webcam ring";
    case SCW_E_VERSION: return "unsupported ring version";
    case SCW_E_GEOMETRY: return "ring header out of range";
    case SCW_E_FORMAT: return "ring pixel format is not BGRA";
    case SCW_E_SIZE: return "ring object size doesn't match its geometry";
    case SCW_E_STATE: return "unknown ring state";
    case SCW_E_EPOCH: return "ring replaced under the mapping";
    case SCW_E_TORN: return "frame overwritten while read";
    case SCW_E_ARG: return "invalid argument";
    case SCW_E_LIMIT: return "ring larger than this reader accepts";
    default: return "unknown error";
    }
}

const char *scw_state_name(uint32_t state) {
    static const char *const names[SCW_STATE_COUNT] = {"init", "starting", "prompting", "running", "idle", "denied",
                                                       "stopped", "superseded"};
    return state < SCW_STATE_COUNT ? names[state] : "unknown";
}

int scw_version_message(char *out, size_t cap, uint32_t ring_version, const uint32_t *accepted, unsigned naccepted) {
    char list[96] = "";
    size_t used = 0;
    for (unsigned i = 0; accepted && i < naccepted && used < sizeof list - 16; i++) {
        int n = snprintf(list + used, sizeof list - used, "%sv%u", i ? ", " : "", accepted[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    return snprintf(out, cap, "webcam: update stuntcam (ring v%u, engine accepts %s)", ring_version,
                    naccepted ? list : "none");
}

uint32_t scw_pick_version(const uint32_t *a, unsigned na, const uint32_t *b, unsigned nb) {
    uint32_t best = 0;
    for (unsigned i = 0; a && i < na; i++)
        if (a[i] > best && scw_accepts(b, nb, a[i])) best = a[i];
    return best;
}

// ---- pid file

static const char *const scw_auth_names[] = {"unknown", "notDetermined", "prompting", "authorized", "denied"};

const char *scw_auth_name(int auth) {
    return auth >= 0 && auth <= SCW_AUTH_DENIED ? scw_auth_names[auth] : "unknown";
}

// Parses a decimal in [0, max]; the whole span must be digits.
static int scw_parse_u64(const char *s, size_t n, uint64_t max, uint64_t *out) {
    uint64_t v = 0;
    if (n == 0 || n > 20) return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (__builtin_mul_overflow(v, 10, &v) || __builtin_add_overflow(v, (uint64_t)(s[i] - '0'), &v)) return 0;
    }
    if (v > max) return 0;
    *out = v;
    return 1;
}

static int scw_span_is(const char *s, size_t n, const char *lit) { return strlen(lit) == n && memcmp(s, lit, n) == 0; }

int scw_pidfile_parse(const char *text, size_t len, scw_pidfile *out) {
    if (!text || !out) return SCW_E_ARG;
    memset(out, 0, sizeof *out);
    out->state = SCW_STATE_COUNT;
    enum { F_HEAD = 1, F_PID = 2, F_SHA = 4, F_RINGS = 8, F_AUTH = 16, F_STATE = 32, F_CAMERA = 64, F_STARTED = 128 };
    unsigned seen = 0;
    size_t pos = 0;
    int line_no = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        const char *l = text + pos;
        size_t n = end - pos;
        pos = end + 1;
        line_no++;
        if (line_no == 1) {
            if (!scw_span_is(l, n, "stuntcam-webcam 1")) return SCW_E_ARG;
            seen |= F_HEAD;
            continue;
        }
        if (n == 0) continue;
        const char *eq = memchr(l, '=', n);
        if (!eq) return SCW_E_ARG;
        size_t kn = (size_t)(eq - l), vn = n - kn - 1;
        const char *v = eq + 1;
        uint64_t u;
        if (scw_span_is(l, kn, "pid")) {
            if (seen & F_PID) return SCW_E_ARG;
            if (!scw_parse_u64(v, vn, 0x7fffffff, &u) || u == 0) return SCW_E_ARG;
            out->pid = (int32_t)u; seen |= F_PID;
        } else if (scw_span_is(l, kn, "sha")) {
            if (seen & F_SHA) return SCW_E_ARG;
            if (vn != 12) return SCW_E_ARG;
            for (size_t i = 0; i < 12; i++)
                if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return SCW_E_ARG;
            memcpy(out->sha, v, 12); out->sha[12] = 0; seen |= F_SHA;
        } else if (scw_span_is(l, kn, "rings")) {
            if (seen & F_RINGS) return SCW_E_ARG;
            size_t i = 0;
            out->nrings = 0;
            while (i <= vn) {
                size_t j = i;
                while (j < vn && v[j] != ',') j++;
                if (out->nrings >= SCW_PID_MAX_RINGS || !scw_parse_u64(v + i, j - i, 0xffff, &u) || u == 0) return SCW_E_ARG;
                out->rings[out->nrings++] = (uint32_t)u;
                i = j + 1;
            }
            seen |= F_RINGS;
        } else if (scw_span_is(l, kn, "auth")) {
            if (seen & F_AUTH) return SCW_E_ARG;
            int a = -1;
            for (int k = SCW_AUTH_NOT_DETERMINED; k <= SCW_AUTH_DENIED; k++) if (scw_span_is(v, vn, scw_auth_names[k])) a = k;
            if (a < 0) return SCW_E_ARG;
            out->auth = a; seen |= F_AUTH;
        } else if (scw_span_is(l, kn, "state")) {
            if (seen & F_STATE) return SCW_E_ARG;
            uint32_t st = SCW_STATE_COUNT;
            for (uint32_t k = 0; k < SCW_STATE_COUNT; k++) if (scw_span_is(v, vn, scw_state_name(k))) st = k;
            if (st == SCW_STATE_COUNT) return SCW_E_ARG;
            out->state = st; seen |= F_STATE;
        } else if (scw_span_is(l, kn, "camera")) {
            if (seen & F_CAMERA) return SCW_E_ARG;
            seen |= F_CAMERA;
            if (vn >= sizeof out->camera) return SCW_E_ARG;
            for (size_t i = 0; i < vn; i++) if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f) return SCW_E_ARG;
            memcpy(out->camera, v, vn); out->camera[vn] = 0;
        } else if (scw_span_is(l, kn, "started")) {
            if (seen & F_STARTED) return SCW_E_ARG;
            seen |= F_STARTED;
            if (!scw_parse_u64(v, vn, UINT64_MAX, &u)) return SCW_E_ARG;
            out->started = u;
        }
        // a known key twice is malformed; unknown keys are ignored (a newer helper may add fields)
    }
    unsigned need = F_HEAD | F_PID | F_SHA | F_RINGS | F_AUTH | F_STATE;
    return (seen & need) == need ? SCW_OK : SCW_E_ARG;
}

int scw_pidfile_format(char *out, size_t cap, const scw_pidfile *p) {
    if (!out || !p || p->pid <= 0 || strlen(p->sha) != 12 || !p->nrings || p->nrings > SCW_PID_MAX_RINGS ||
        p->auth < SCW_AUTH_NOT_DETERMINED || p->auth > SCW_AUTH_DENIED || p->state >= SCW_STATE_COUNT)
        return SCW_E_ARG;
    char rings[96] = "";
    size_t used = 0;
    for (unsigned i = 0; i < p->nrings; i++) {
        if (!p->rings[i] || p->rings[i] > 0xffff) return SCW_E_ARG;
        used += (size_t)snprintf(rings + used, sizeof rings - used, "%s%u", i ? "," : "", p->rings[i]);
    }
    for (const char *c = p->camera; *c; c++) if ((unsigned char)*c < 0x20 || *c == 0x7f) return SCW_E_ARG;
    int n = snprintf(out, cap, "stuntcam-webcam 1\npid=%d\nsha=%s\nrings=%s\nauth=%s\nstate=%s\ncamera=%s\nstarted=%llu\n",
                     p->pid, p->sha, rings, scw_auth_name(p->auth), scw_state_name(p->state), p->camera,
                     (unsigned long long)p->started);
    if (n < 0 || (size_t)n >= cap) return SCW_E_ARG;
    return n;
}

#undef SCW_LD32
#undef SCW_LD64
#undef SCW_LD64R
#undef SCW_LD32R
#undef SCW_ST32
#undef SCW_ST64
#undef SCW_ST64R
