// SPDX-License-Identifier: Apache-2.0
// Host unit tests for the webcam ring (ring.c) and its shm object (ring_shm.c). Run by engine/src/webcam/test.sh;
// by hand:  xcrun clang -Wall -g -fsanitize=address,undefined -o "$TMPDIR/ring_test" engine/src/webcam/ring_test.c &&
//             "$TMPDIR/ring_test" [--no-shm] [--stress SECONDS]
// The shm tests create and remove objects named /scwt.<pid>.* and print what this OS does with fchmod and umask.
#include "ring.c"
#include "ring_shm.c"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
    if (_a != _b) { fprintf(stderr, "FAIL %s:%d %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); fails++; } } while (0)

static const uint32_t V1[] = {1};

// A ring in ordinary memory, allocated to exactly its object size so ASan flags any read past the end.
typedef struct { uint8_t *mem; uint64_t len; scw_header *h; scw_writer w; } ring_t;
static ring_t mk(uint32_t w, uint32_t h, uint32_t bpr, uint32_t nslots) {
    ring_t r = {0};
    uint64_t slot, total = scw_object_bytes(h, bpr, nslots, &slot);
    if (!total) { fprintf(stderr, "mk: bad geometry\n"); exit(2); }
    r.mem = calloc(1, (size_t)total); r.len = total; r.h = (scw_header *)r.mem;
    if (scw_writer_init(&r.w, r.mem, total, w, h, bpr, nslots, 0x1234, 30000, 4242, "Test Camera") != SCW_OK) { fprintf(stderr, "mk: init\n"); exit(2); }
    scw_writer_publish(&r.w);
    return r;
}
static void put(ring_t *r, uint64_t seq, uint64_t now) {   // one frame whose every byte is seq & 0xff
    uint8_t *p = scw_writer_begin(&r->w, seq);
    if (!p) { fprintf(stderr, "put: seq %llu refused\n", (unsigned long long)seq); exit(2); }
    memset(p, (int)(seq & 0xff), (size_t)r->w.bpr0 * r->w.height);
    scw_writer_end(&r->w, seq, now - 1000, now, 0);
}
static int validate(ring_t *r, scw_geom *g) { uint32_t fv; return scw_validate(r->mem, r->len, V1, 1, g, &fv); }

static void test_validate(void) {
    scw_geom g; uint32_t fv;
    ring_t r = mk(64, 48, 256, 3);
    CHECKEQ(validate(&r, &g), SCW_OK);
    CHECK(g.width == 64 && g.height == 48 && g.bpr0 == 256 && g.nslots == 3 && g.slotBytes == 12288 && g.epoch == 0x1234);
    CHECK(g.totalBytes == SCW_HDR + 3 * 12288 && g.mapBytes == r.len && !strcmp(g.device, "Test Camera"));
    scw_header save = *r.h;
#define MUT(stmt, want) do { *r.h = save; stmt; CHECKEQ(validate(&r, &g), want); } while (0)
    // zero and huge dimensions and rows
    MUT(r.h->width = 0, SCW_E_GEOMETRY);
    MUT(r.h->height = 0, SCW_E_GEOMETRY);
    MUT(r.h->width = SCW_MAX_DIM + 1, SCW_E_GEOMETRY);
    MUT(r.h->height = SCW_MAX_DIM + 1, SCW_E_GEOMETRY);
    MUT(r.h->width = UINT32_MAX, SCW_E_GEOMETRY);
    MUT(r.h->height = UINT32_MAX, SCW_E_GEOMETRY);
    MUT(r.h->bpr0 = 0, SCW_E_GEOMETRY);
    MUT(r.h->bpr0 = 4 * 64 - 4, SCW_E_GEOMETRY);              // shorter than a row
    MUT(r.h->bpr0 = 258, SCW_E_GEOMETRY);                      // not a multiple of 4
    MUT(r.h->bpr0 = UINT32_MAX & ~3u, SCW_E_GEOMETRY);         // bpr0*height far beyond slotBytes
    MUT((r.h->bpr0 = 4096, r.h->height = 8), SCW_E_GEOMETRY);  // 32768 > slotBytes 12288
    // slotBytes and nslots
    MUT(r.h->slotBytes = 0, SCW_E_GEOMETRY);
    MUT(r.h->slotBytes = 12288 + 1, SCW_E_GEOMETRY);
    MUT((r.h->slotBytes = 1ull << 63, r.h->nslots = 2), SCW_E_SIZE);                 // slotBytes*nslots overflows
    MUT((r.h->slotBytes = 0xfffffffffffff000ull, r.h->nslots = 1), SCW_E_SIZE);      // + header overflows
    MUT((r.h->slotBytes = 1ull << 40, r.h->totalBytes = SCW_HDR + 3 * (1ull << 40)), SCW_E_SIZE);   // > mapping, > 1 GiB
    MUT(r.h->nslots = 0, SCW_E_GEOMETRY);
    MUT(r.h->nslots = SCW_MAX_SLOTS + 1, SCW_E_GEOMETRY);
    MUT(r.h->nslots = 4, SCW_E_GEOMETRY);                      // totalBytes no longer matches
    MUT((r.h->nslots = 4, r.h->totalBytes = SCW_HDR + 4 * 12288), SCW_E_SIZE);   // matches, but the mapping is short
    MUT(r.h->totalBytes = r.h->totalBytes - 4096, SCW_E_GEOMETRY);
    // planes: BGRA has none past plane 0, so any plane-1 offset (past the end or not) is refused
    MUT(r.h->plane1Off = 4096, SCW_E_GEOMETRY);
    MUT(r.h->plane1Off = UINT64_MAX, SCW_E_GEOMETRY);
    MUT(r.h->bpr1 = 64, SCW_E_GEOMETRY);
    // identity, format, state
    MUT(r.h->format = 0x34323076u /* '420v' */, SCW_E_FORMAT);
    MUT(r.h->magic = 0, SCW_E_NOTREADY);
    MUT(r.h->magic = 0x52574354u, SCW_E_MAGIC);
    MUT(r.h->epoch = 0, SCW_E_GEOMETRY);
    MUT(r.h->headerBytes = 8192, SCW_E_GEOMETRY);
    MUT(r.h->flags = 1, SCW_E_GEOMETRY);
    MUT(r.h->state = SCW_STATE_COUNT, SCW_E_STATE);
    MUT(r.h->state = UINT32_MAX, SCW_E_STATE);
    MUT(r.h->state = SCW_STATE_PROMPTING, SCW_OK);
    // versions: unknown -> SCW_E_VERSION with the version found, and the "update stuntcam" line
    *r.h = save; r.h->version = 2;
    CHECKEQ(scw_validate(r.mem, r.len, V1, 1, &g, &fv), SCW_E_VERSION);
    CHECKEQ(fv, 2);
    char msg[128];
    scw_version_message(msg, sizeof msg, fv, V1, 1);
    CHECK(!strcmp(msg, "webcam: update stuntcam (ring v2, engine accepts v1)"));
    const uint32_t V12[] = {1, 2};
    CHECKEQ(scw_validate(r.mem, r.len, V12, 2, &g, &fv), SCW_OK);
    scw_version_message(msg, sizeof msg, 3, V12, 2);
    CHECK(!strcmp(msg, "webcam: update stuntcam (ring v3, engine accepts v1, v2)"));
    CHECKEQ(scw_validate(r.mem, r.len, NULL, 0, &g, &fv), SCW_E_VERSION);
    // truncated mappings
    *r.h = save;
    CHECKEQ(scw_validate(r.mem, r.len - 1, V1, 1, &g, &fv), SCW_E_SIZE);
    CHECKEQ(scw_validate(r.mem, SCW_HDR, V1, 1, &g, &fv), SCW_E_SIZE);
    CHECKEQ(scw_validate(r.mem, SCW_HDR - 1, V1, 1, &g, &fv), SCW_E_SHORT);
    CHECKEQ(scw_validate(r.mem, 0, V1, 1, &g, &fv), SCW_E_SHORT);
    CHECKEQ(scw_validate(r.mem, SCW_MAX_OBJECT + 1, V1, 1, &g, &fv), SCW_E_SIZE);   // never read past the header here
    CHECKEQ(scw_validate(NULL, r.len, V1, 1, &g, &fv), SCW_E_ARG);
    // device name: no terminator, control characters
    *r.h = save;
    memset(r.h->device, 'A', SCW_DEVICE_LEN); r.h->device[3] = '\n';
    CHECKEQ(validate(&r, &g), SCW_OK);
    CHECK(strlen(g.device) == SCW_DEVICE_LEN - 1 && g.device[3] == '?');
    // device name: well-formed UTF-8 is kept; anything else becomes '?', byte by byte
    struct { const char *in, *out; } dv[] = {
        {"Cam\xc3\xa9ra", "Cam\xc3\xa9ra"},                  // é
        {"\xe2\x80\x94 \xf0\x9f\x93\xb7", "\xe2\x80\x94 \xf0\x9f\x93\xb7"},   // em dash, camera emoji
        {"a\xffz", "a?z"}, {"\xc0\xaf", "??"},                  // invalid byte; overlong '/'
        {"\xed\xa0\x80", "???"}, {"\xf4\x90\x80\x80", "????"},  // surrogate; above U+10FFFF
        {"ab\xc3", "ab?"}, {"\xe2\x80", "??"}, {"\x1b[2J", "?[2J"},   // cut sequence; control character
        {"a\xc2\x85" "b", "a??b"},                        // C1 control (U+0085)
    };
    for (unsigned i = 0; i < sizeof dv / sizeof dv[0]; i++) {
        *r.h = save; memset(r.h->device, 0, SCW_DEVICE_LEN); strcpy(r.h->device, dv[i].in);
        CHECKEQ(validate(&r, &g), SCW_OK);
        if (strcmp(g.device, dv[i].out)) { fprintf(stderr, "FAIL device case %u: got '%s'\n", i, g.device); fails++; }
        checks++;
    }
    *r.h = save; memset(r.h->device, 'x', SCW_DEVICE_LEN); memcpy(r.h->device + 62, "\xc3\xa9", 2);   // é cut by the 63-byte limit
    CHECKEQ(validate(&r, &g), SCW_OK);
    CHECK(strlen(g.device) == 63 && g.device[62] == '?' && g.device[61] == 'x');
#undef MUT
    free(r.mem);
}

static void test_writer_init(void) {
    scw_header h; uint64_t slot;
    CHECKEQ(scw_object_bytes(1080, 7680, 4, &slot), SCW_HDR + 4ull * 8294400);
    CHECKEQ(slot, 8294400);
    CHECKEQ(scw_object_bytes(2160, 15360, 4, &slot), SCW_HDR + 4ull * 33177600);
    CHECKEQ(scw_object_bytes(8192, 32768, 8, &slot), 0);       // 2 GiB: over the object limit
    CHECKEQ(scw_object_bytes(0, 7680, 4, &slot), 0);
    CHECKEQ(scw_object_bytes(1080, 7680, 9, &slot), 0);
    CHECKEQ(scw_header_init(&h, 1920, 1080, 7680, 4, 1, 30000, 1, "x"), SCW_OK);
    CHECKEQ(h.magic, 0);                                       // stored only by scw_writer_publish
    CHECKEQ(scw_header_init(&h, 1920, 1080, 7676, 4, 1, 30000, 1, "x"), SCW_E_GEOMETRY);
    CHECKEQ(scw_header_init(&h, 1920, 1080, 7680, 4, 0, 30000, 1, "x"), SCW_E_ARG);
    CHECKEQ(scw_header_init(&h, 0, 1080, 7680, 4, 1, 30000, 1, "x"), SCW_E_GEOMETRY);
    CHECKEQ(h.magic, 0);
}

static void test_reads(void) {
    ring_t r = mk(64, 48, 256, 3);
    scw_geom g; scw_slot_read sr; scw_peek_t pk;
    CHECKEQ(validate(&r, &g), SCW_OK);
    size_t dbpr = 64 * 4 + 32, dlen = dbpr * 48;
    uint8_t *dst = malloc(dlen);
    CHECKEQ(scw_read_latest(r.mem, &g, 0, dst, dbpr, dlen, &sr), SCW_NONE);   // nothing published yet
    CHECKEQ(scw_peek(r.mem, &g, &pk), SCW_OK);
    CHECK(pk.latest == 0 && !pk.hasTimestamps && !scw_is_live(&pk, 1));
    uint64_t now = 10000000000ull;
    put(&r, 1, now);
    CHECKEQ(scw_read_latest(r.mem, &g, 0, dst, dbpr, dlen, &sr), SCW_OK);
    CHECK(sr.seq == 1 && sr.index == 1 && sr.publishNs == now && sr.captureNs == now - 1000 && dst[0] == 1 && dst[dlen - 33] == 1);
    CHECKEQ(scw_read_latest(r.mem, &g, 1, dst, dbpr, dlen, &sr), SCW_NONE);   // same frame again
    CHECKEQ(scw_read_latest(r.mem, &g, 0, dst, 64 * 4 - 1, dlen, &sr), SCW_E_ARG);
    CHECKEQ(scw_read_latest(r.mem, &g, 0, dst, dbpr, dlen - 1, &sr), SCW_E_ARG);
    // peek and liveness
    r.h->state = SCW_STATE_RUNNING;
    CHECKEQ(scw_peek(r.mem, &g, &pk), SCW_OK);
    CHECK(pk.latest == 1 && pk.hasTimestamps && pk.publishNs == now && pk.captureNs == now - 1000);
    CHECK(scw_is_live(&pk, now + SCW_LIVE_NS - 1) && !scw_is_live(&pk, now + SCW_LIVE_NS));
    r.h->state = SCW_STATE_IDLE; scw_peek(r.mem, &g, &pk);
    CHECK(!scw_is_live(&pk, now));
    r.h->state = SCW_STATE_RUNNING;

    // torn: the slot is rewritten between begin and end (seq 1 and seq 4 share slot 1 of 3)
    put(&r, 2, now); put(&r, 3, now);
    r.h->latest = 1;                                           // an older latest whose slot wasn't reused yet reads fine
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_OK);        // (seq 2 and 3 went to slots 2 and 0)
    r.h->latest = 3;
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_OK);
    put(&r, 6, now);                                           // seq 6 -> slot 0, the slot being read
    CHECKEQ(scw_slot_end(r.mem, &g, &sr), SCW_E_TORN);
    // torn: begin sees a slot mid-write (odd gen)
    scw_writer_begin(&r.w, 7);                                 // slot 1 busy, latest still 6
    r.h->latest = 7;
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_E_TORN);
    scw_writer_end(&r.w, 7, now, now, 0);
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_OK);
    // the slot's seq disagrees with its gen
    r.h->slot[1].seq = 8;
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_E_TORN);
    r.h->slot[1].seq = 7;
    // latest so large that 2*seq+2 would overflow
    r.h->latest = UINT64_MAX;
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_E_TORN);
    CHECKEQ(scw_peek(r.mem, &g, &pk), SCW_OK); CHECK(!pk.hasTimestamps);
    r.h->latest = 7;

    // version or epoch change mid-read
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_OK);
    r.h->epoch = 0x9999;
    CHECKEQ(scw_slot_end(r.mem, &g, &sr), SCW_E_EPOCH);
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_E_EPOCH);
    CHECKEQ(scw_peek(r.mem, &g, &pk), SCW_E_EPOCH);
    r.h->epoch = 0x1234;
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_OK);
    r.h->version = 2;
    CHECKEQ(scw_slot_end(r.mem, &g, &sr), SCW_E_EPOCH);
    r.h->version = 1;
    r.h->magic = 0;
    CHECKEQ(scw_peek(r.mem, &g, &pk), SCW_E_EPOCH);
    r.h->magic = SCW_MAGIC;

    // the geometry is never re-read: a hostile header rewrite after validation stays inside the mapping
    r.h->width = 8192; r.h->height = 8192; r.h->bpr0 = 32768; r.h->nslots = 1; r.h->slotBytes = 1ull << 40;
    put(&r, 9, now);                                           // the writer, too, ignores the rewritten header
    CHECKEQ(scw_read_latest(r.mem, &g, 0, dst, dbpr, dlen, &sr), SCW_OK);
    CHECK(sr.index == 0 && dst[0] == 9 && dst[dlen - 33] == 9);
    r.h->latest = 10; r.h->slot[1].seq = 10; r.h->slot[1].gen = 22;   // index = 10 % private nslots (3) = 1
    CHECKEQ(scw_slot_begin(r.mem, &g, 0, &sr), SCW_OK);
    CHECK(sr.index == 1 && sr.pixels == r.mem + SCW_HDR + 12288);
    free(dst); free(r.mem);
}

static void test_clear(void) {
    ring_t r = mk(64, 48, 256, 2);
    put(&r, 1, 5); put(&r, 2, 6);
    scw_writer_clear(&r.w);
    CHECK(r.h->latest == 0 && r.h->slot[0].gen == 0 && r.h->slot[1].gen == 0 && r.h->slot[0].publishNs == 0);
    int zero = 1;
    for (uint64_t i = SCW_HDR; i < r.len; i++) {
        if (r.mem[i]) { zero = 0; break; }
    }
    CHECK(zero);
    // after a clear the sequence continues: seq never restarts within an epoch
    CHECK(scw_writer_begin(&r.w, 2) == NULL && scw_writer_begin(&r.w, 3) != NULL);
    scw_writer_end(&r.w, 3, 7, 7, 0);
    free(r.mem);
}

// The writer never reads geometry or sequence state back from the shared header (any same-user process can rewrite
// it): every write lands where the private copy says, and nslots = 0 in the header can't divide by zero.
static void test_writer_hostile(void) {
    ring_t r = mk(64, 48, 256, 3);
    put(&r, 1, 10);
    scw_header evil;
    memset(&evil, 0xff, sizeof evil);
    evil.nslots = 0; evil.slotBytes = 1ull << 40; evil.bpr0 = UINT32_MAX; evil.height = UINT32_MAX;
    evil.totalBytes = UINT64_MAX; evil.epoch = 0xbad; evil.latest = UINT64_MAX;
    memcpy(r.h, &evil, sizeof evil);
    for (int i = 0; i < 8; i++) {
        r.h->slot[i].gen = UINT64_MAX;
    }
    put(&r, 2, 20); put(&r, 3, 30); put(&r, 4, 40);            // slots 2, 0, 1 by the private nslots (3)
    CHECK(r.mem[SCW_HDR + 2 * 12288] == 2 && r.mem[SCW_HDR] == 3 && r.mem[SCW_HDR + 12288] == 4);
    CHECK(r.mem[r.len - 1] == 2);                              // the last byte of the object: slot 2's last pixel
    CHECK(r.h->slot[1].gen == 2 * 4 + 2 && r.h->latest == 4 && r.h->slot[1].seq == 4);
    CHECK(r.w.epoch == 0x1234 && r.w.nslots == 3 && r.w.slotBytes == 12288);
    // sequence numbers: never reused, never reset; an end without its begin is ignored
    CHECK(scw_writer_begin(&r.w, 4) == NULL && scw_writer_begin(&r.w, 1) == NULL);
    scw_writer_end(&r.w, 5, 1, 1, 0);
    CHECK(r.h->latest == 4);
    CHECK(scw_writer_begin(&r.w, UINT64_MAX / 2) == NULL);    // 2*seq+2 would overflow
    scw_writer_clear(&r.w);                                     // bounded by the private geometry
    CHECK(r.mem[SCW_HDR] == 0 && r.mem[SCW_HDR + 12288] == 0 && r.mem[SCW_HDR + 2 * 12288] == 0 && r.h->latest == 0);
    scw_writer_state(&r.w, SCW_STATE_STOPPED);
    CHECK(r.h->state == SCW_STATE_STOPPED);
    // init refuses a mapping shorter than the geometry, and never writes past it
    scw_writer w2;
    uint8_t *small = calloc(1, SCW_HDR + 100);
    CHECKEQ(scw_writer_init(&w2, small, SCW_HDR + 100, 64, 48, 256, 3, 1, 30000, 1, "x"), SCW_E_SIZE);
    CHECKEQ(scw_writer_init(&w2, small, SCW_HDR - 1, 64, 48, 256, 3, 1, 30000, 1, "x"), SCW_E_ARG);
    CHECK(scw_writer_begin(&w2, 1) == NULL);                   // an uninitialized writer writes nothing
    free(small); free(r.mem);
}

// The same, with a second thread scribbling over the header while the writer publishes frames.
typedef struct { ring_t *r; volatile int stop; uint64_t writes; } scribble_ctx;
static void *scribbler(void *a) {
    scribble_ctx *c = a;
    uint64_t x = 0x9e3779b97f4a7c15ull;
    while (!c->stop) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t *word = (uint64_t *)c->r->mem + (x % (SCW_HDR / 8));
        static const uint64_t vals[] = {0, 1, UINT64_MAX, 1ull << 40, 0xffffffff00000000ull};
        __atomic_store_n(word, (x >> 32) % 3 ? vals[(x >> 40) % 5] : x, __ATOMIC_RELAXED);
        c->writes++;
    }
    return NULL;
}
static void test_writer_scribbled(double seconds) {
    ring_t r = mk(64, 48, 256, 3);
    scribble_ctx c = {&r, 0, 0};
    pthread_t t; pthread_create(&t, NULL, scribbler, &c);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t seq = 0;
    for (;;) {
        seq++;
        put(&r, seq, seq);
        if ((seq & 1023) == 0) {
            scw_writer_clear(&r.w);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 >= seconds) {
                break;
            }
        }
    }
    c.stop = 1; pthread_join(t, NULL);
    printf("writer under a concurrent header scribbler %.0fs: %llu frames, %llu hostile header writes, private geometry intact\n",
           seconds, (unsigned long long)seq, (unsigned long long)c.writes);
    CHECK(r.w.nslots == 3 && r.w.slotBytes == 12288 && r.w.epoch == 0x1234 && r.w.lastSeq == seq);
    free(r.mem);
}

static void test_names(void) {
    char h[SCW_CAM_HASH_BYTES], n[64];
    scw_cam_hash("/Users/x/Library/Application Support/stuntcam", "0x1420000005ac8600", h);
    CHECK(!strcmp(h, "190c1c7f"));
    scw_cam_hash("", "", h); CHECK(!strcmp(h, "8601b7df"));
    scw_cam_hash("/h", "cam", h); CHECK(!strcmp(h, "fc04bcb5"));
    CHECKEQ(scw_shm_name(n, sizeof n, 501, "1a2b3c4d", 1), 20);
    CHECK(!strcmp(n, "/scw.501.1a2b3c4d.v1"));
    CHECKEQ(scw_shm_name(n, sizeof n, 4294967295ul, "1a2b3c4d", 65535), 31);   // the longest: exactly the limit
    CHECKEQ(scw_shm_name(n, sizeof n, 4294967295ul, "1a2b3c4d", 100000), SCW_E_ARG);
    CHECKEQ(scw_shm_name(n, sizeof n, 501, "1A2B3C4D", 1), SCW_E_ARG);
    CHECKEQ(scw_shm_name(n, sizeof n, 501, "1a2b3c4", 1), SCW_E_ARG);
    CHECKEQ(scw_shm_name(n, sizeof n, 501, "1a2b3c4d", 0), SCW_E_ARG);
    CHECKEQ(scw_shm_name(n, 20, 501, "1a2b3c4d", 1), SCW_E_ARG);              // no room for the terminator
    const uint32_t a[] = {1, 2}, b[] = {2, 3}, c[] = {3};
    CHECKEQ(scw_pick_version(a, 2, b, 2), 2);
    CHECKEQ(scw_pick_version(a, 2, c, 1), 0);
    CHECK(!strcmp(scw_state_name(SCW_STATE_PROMPTING), "prompting") && !strcmp(scw_state_name(99), "unknown"));
}

static void test_pidfile(void) {
    scw_pidfile p = {0}, q;
    p.pid = 4321; strcpy(p.sha, "0123456789ab"); p.rings[0] = 1; p.rings[1] = 2; p.nrings = 2;
    p.auth = SCW_AUTH_PROMPTING; p.state = SCW_STATE_PROMPTING; strcpy(p.camera, "0x1420000005ac8600"); p.started = 1794790000;
    char buf[512];
    int n = scw_pidfile_format(buf, sizeof buf, &p);
    CHECK(n > 0 && !strcmp(buf, "stuntcam-webcam 1\npid=4321\nsha=0123456789ab\nrings=1,2\nauth=prompting\nstate=prompting\n"
                                "camera=0x1420000005ac8600\nstarted=1794790000\n"));
    CHECKEQ(scw_pidfile_parse(buf, (size_t)n, &q), SCW_OK);
    CHECK(q.pid == 4321 && !strcmp(q.sha, p.sha) && q.nrings == 2 && q.rings[1] == 2 && q.auth == SCW_AUTH_PROMPTING &&
          q.state == SCW_STATE_PROMPTING && !strcmp(q.camera, p.camera) && q.started == p.started);
    CHECKEQ(scw_pidfile_parse(buf, (size_t)n - 1, &q), SCW_OK);              // no final newline
    CHECKEQ(scw_pidfile_format(buf, 40, &p), SCW_E_ARG);                     // doesn't fit
#define BAD(s) CHECKEQ(scw_pidfile_parse(s, strlen(s), &q), SCW_E_ARG)
#define GOOD(s) CHECKEQ(scw_pidfile_parse(s, strlen(s), &q), SCW_OK)
    const char *base = "stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\n";
    GOOD(base);
    snprintf(buf, sizeof buf, "%sfuture=whatever\n", base); GOOD(buf);           // unknown keys are ignored
    BAD("");
    BAD("stuntcam-webcam 2\npid=1\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\n");          // no pid
    BAD("stuntcam-webcam 1\npid=0\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=99999999999\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789AB\nrings=1\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789a\nrings=1\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1,\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1,2,3,4,5,6,7,8,9\nauth=authorized\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1\nauth=maybe\nstate=running\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=asleep\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\nno equals sign\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1\nauth=authorized\nstate=running\ncamera=a\tb\n");
    BAD("stuntcam-webcam 1\npid=1\nsha=0123456789ab\nrings=1,2\nauth=authorized\nstate=running\nrings=3\n");   // duplicate key
#undef BAD
#undef GOOD
}

// ---- concurrency: one writer, one reader, the smallest ring (2 slots) so the writer laps the reader constantly
typedef struct { ring_t *r; volatile int stop; uint64_t written; } wctx;
static void *writer(void *a) {
    wctx *c = a;
    uint64_t seq = 0;
    while (!c->stop) {
        seq++;
        uint8_t *p = scw_writer_begin(&c->r->w, seq);
        for (uint32_t y = 0; y < c->r->w.height; y++) {
            memset(p + (size_t)y * c->r->w.bpr0, (int)(seq & 0xff), c->r->w.bpr0);
        }
        scw_writer_end(&c->r->w, seq, seq, seq, 0);
    }
    c->written = seq;
    return NULL;
}
static void test_stress(double seconds) {
    ring_t r = mk(256, 64, 1024, 2);
    scw_geom g; CHECKEQ(validate(&r, &g), SCW_OK);
    wctx c = {&r, 0, 0};
    pthread_t t; pthread_create(&t, NULL, writer, &c);
    size_t dlen = (size_t)g.bpr0 * g.height;
    uint8_t *dst = malloc(dlen);
    uint64_t last = 0, ok = 0, torn = 0, none = 0, bad = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        scw_slot_read sr;
        int rc = scw_read_latest(r.mem, &g, last, dst, g.bpr0, dlen, &sr);
        if (rc == SCW_OK) {
            ok++; last = sr.seq;
            uint8_t want = (uint8_t)(sr.seq & 0xff);
            for (size_t i = 0; i < dlen; i++) {
                if (dst[i] != want) { bad++; break; }
            }
            if (sr.captureNs != sr.seq || sr.publishNs != sr.seq) {
                bad++;
            }
        } else if (rc == SCW_E_TORN) {
            torn++;
        } else {
            none++;
        }
        if (((ok + torn + none) & 1023) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 >= seconds) {
                break;
            }
        }
    }
    c.stop = 1; pthread_join(t, NULL);
    printf("stress %.0fs: writer %llu frames; reader ok %llu, torn detected %llu, nothing new %llu, inconsistent accepted %llu\n",
           seconds, (unsigned long long)c.written, (unsigned long long)ok, (unsigned long long)torn,
           (unsigned long long)none, (unsigned long long)bad);
    CHECKEQ(bad, 0);
    CHECK(ok > 0 && torn > 0);   // both paths exercised
    free(dst); free(r.mem);
}

// ---- shm
static char N1[40], N2[40], N3[40];
static void shm_names(void) {
    snprintf(N1, sizeof N1, "/scwt.%d.a", (int)getpid());
    snprintf(N2, sizeof N2, "/scwt.%d.b", (int)getpid());
    snprintf(N3, sizeof N3, "/scwt.%d.c", (int)getpid());
}
static void shm_cleanup(void) { shm_unlink(N1); shm_unlink(N2); shm_unlink(N3); }
static uint64_t RB;   // bytes of the test ring object
static int publish(scw_shm_writer *w, uint64_t epoch) {
    if (scw_writer_init(&w->ring, w->header, w->len, 64, 48, 256, 3, epoch, 30000, getpid(), "shm test") != SCW_OK) {
        return -1;
    }
    scw_writer_publish(&w->ring);
    return 0;
}
static void wput(scw_shm_writer *w, uint64_t seq, uint64_t now) {
    uint8_t *p = scw_writer_begin(&w->ring, seq);
    memset(p, (int)(seq & 0xff), 256 * 48);
    scw_writer_end(&w->ring, seq, now, now, 0);
}

static void test_shm(void) {
    shm_names(); shm_cleanup();
    RB = scw_object_bytes(48, 256, 3, NULL);
    scw_shm_writer w, w2; scw_shm_report rep;
    uid_t me = getuid();
    char why[192];

    // creation under umask 0 and 077, and what fchmod does
    mode_t masks[] = {0, 077, 022};
    for (int i = 0; i < 3; i++) {
        mode_t old = umask(masks[i]);
        int rc = scw_shm_create(&w, N1, RB, &rep);
        umask(old);
        CHECKEQ(rc, SCW_OK);
        CHECKEQ(rep.mode, 0600);
        printf("shm create under umask %03o: mode %04o, fchmod(fd, 0600) -> %s, initial unlink -> %s\n", (unsigned)masks[i],
               rep.mode, rep.fchmodErrno ? strerror(rep.fchmodErrno) : "ok",
               rep.unlinkErrno ? strerror(rep.unlinkErrno) : "removed an object");
        scw_shm_retire(&w, SCW_STATE_STOPPED);
    }
    // what shm_open does with a group/other mode under umask 077 (the helper never asks for one)
    {
        mode_t old = umask(077);
        int fd = shm_open(N2, O_CREAT | O_EXCL | O_RDWR, 0666);
        umask(old);
        struct stat st; fstat(fd, &st);
        printf("shm_open(0666) under umask 077: mode %04o (umask %s)\n", (unsigned)(st.st_mode & 07777),
               (st.st_mode & 077) ? "not applied" : "applied");
        close(fd); shm_unlink(N2);
    }
    // a name pre-taken by this user, with a world-readable mode: the helper replaces it with a fresh 0600 object
    {
        int fd = shm_open(N1, O_CREAT | O_EXCL | O_RDWR, 0644);
        CHECK(fd >= 0); CHECK(ftruncate(fd, 16384) == 0); close(fd);
        CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK);
        CHECK(rep.unlinkErrno == 0 && rep.mode == 0600);
        printf("pre-taken name (same uid, mode 0644): unlinked and recreated as %04o\n", rep.mode);
        scw_shm_retire(&w, SCW_STATE_STOPPED);
    }
    // the reader refuses this user's object when it has group/other bits, even with a valid header
    {
        int fd = shm_open(N2, O_CREAT | O_EXCL | O_RDWR, 0644);
        CHECK(ftruncate(fd, (off_t)RB) == 0);
        scw_header *h = mmap(NULL, (size_t)RB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        scw_writer pw; scw_writer_init(&pw, h, RB, 64, 48, 256, 3, 77, 30000, getpid(), "planted"); scw_writer_publish(&pw);
        scw_shm_open_result opened;
        CHECK(scw_shm_open_ro(N2, V1, 1, &opened) < 0 && opened.map == NULL);
        CHECK(strstr(opened.why, "mode 0644") != NULL);
        printf("reader, own object with mode 0644: refused: %s\n", opened.why);
        munmap(h, (size_t)RB); close(fd); shm_unlink(N2);
    }
    // owner checks against another uid: a fake stat, and a real object owned by root if this OS has one
    {
        struct stat st; memset(&st, 0, sizeof st);
        st.st_uid = me + 1; st.st_mode = 0100600;
        CHECK(!scw_shm_check_stat(&st, me, "/scw.x", why, sizeof why));
        char want[96]; snprintf(want, sizeof want, "webcam: shm name /scw.x owned by uid %lu", (unsigned long)(me + 1));
        CHECK(!strcmp(why, want));
        st.st_uid = me; st.st_mode = 0100640;
        CHECK(!scw_shm_check_stat(&st, me, "/scw.x", why, sizeof why) && strstr(why, "0640"));
        st.st_mode = 0100600;
        CHECK(scw_shm_check_stat(&st, me, "/scw.x", why, sizeof why));
        const char *sys = "apple.shm.notification_center";   // notifyd's object, owned by root, on macOS
        long o = scw_owner_of(sys);
        if (o >= 0 && o != (long)me) {
            scw_shm_open_result opened;
            int rc = scw_shm_open_ro(sys, V1, 1, &opened);
            CHECK(rc < 0 && opened.owner == o && strstr(opened.why, "owned by uid"));
            printf("reader, another uid's object (%s, uid %ld): refused: %s\n", sys, o, opened.why);
            // the helper's O_EXCL create on a taken name fails, and the report names the owner (the helper's initial
            // shm_unlink is not exercised on this name: it is a live system object)
            int fd = shm_open(sys, O_CREAT | O_EXCL | O_RDWR, 0600);
            int e = errno;
            CHECK(fd < 0 && e == EEXIST);
            memset(&rep, 0, sizeof rep);
            scw_refuse_owner(&rep, sys, scw_owner_of(sys), e);
            printf("helper, O_EXCL on another uid's name: %s -> \"%s\"\n", strerror(e), rep.msg);
            CHECK(strstr(rep.msg, "owned by uid 0") != NULL || o != 0);
        } else {
            printf("no other uid's shm object found; the fake-stat check stands in\n");
        }
    }
    // names longer than the macOS limit fail
    {
        char longn[40]; memset(longn, 'a', 33); longn[0] = '/'; longn[32] = 0;
        CHECK(scw_shm_create(&w, longn, RB, &rep) == SCW_E_ARG);
    }
    // down: retire zeroes the slots before unlinking; a reader still mapping the old object sees only zeros
    {
        CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK);
        publish(&w, 1001); wput(&w, 1, 5); wput(&w, 2, 6);
        scw_shm_open_result opened;
        CHECKEQ(scw_shm_open_ro(N1, V1, 1, &opened), SCW_OK);
        const void *m = opened.map;
        uint64_t n = opened.len;
        CHECK(n >= RB);                                        // st_size may be rounded up to the page size
        printf("object of %llu bytes reports st_size %llu\n", (unsigned long long)RB, (unsigned long long)n);
        CHECK(((const uint8_t *)m)[SCW_HDR + 12288] == 1);
        scw_shm_retire(&w, SCW_STATE_STOPPED);
        const scw_header *h = m;
        int zero = 1;
        for (uint64_t i = SCW_HDR; i < RB; i++) {
            if (((const uint8_t *)m)[i]) { zero = 0; break; }
        }
        CHECK(zero && h->latest == 0 && h->state == SCW_STATE_STOPPED);
        CHECK(shm_open(N1, O_RDONLY, 0) < 0 && errno == ENOENT);
        // the reader can't write through its mapping
        CHECK(mprotect((void *)m, (size_t)n, PROT_READ | PROT_WRITE) != 0);
        munmap((void *)m, (size_t)n);
    }
    // retire never unlinks a newer object at the same name
    {
        CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK); publish(&w, 2001);
        CHECKEQ(scw_shm_create(&w2, N1, RB, &rep), SCW_OK); publish(&w2, 2002);   // unlinks the first, makes another
        scw_shm_retire(&w, SCW_STATE_STOPPED);
        scw_shm_open_result opened;
        CHECKEQ(scw_shm_open_ro(N1, V1, 1, &opened), SCW_OK);
        CHECKEQ(opened.geom.epoch, 2002);
        munmap((void *)opened.map, (size_t)opened.len);
        scw_shm_retire(&w2, SCW_STATE_STOPPED);
    }
    // a second resize of a shm object fails on macOS, so a mapped ring can't be truncated under a reader
    {
        CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK);
        int rc = ftruncate(w.fd, 4096);
        printf("ftruncate of a sized shm object: %s\n", rc ? strerror(errno) : "allowed");
        scw_shm_retire(&w, SCW_STATE_STOPPED);
    }
}

static void test_reader_lifecycle(void) {
    shm_names(); shm_cleanup();
    scw_shm_writer w, w2; scw_shm_report rep; scw_shm_reader r; scw_peek_t pk;
    const uint64_t S = 1000000000ull;
    uint64_t t = 100 * S;
    scw_shm_reader_init(&r, N1, V1, 1);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);                 // no helper yet
    CHECK(strstr(r.why, "no ring") != NULL);
    // being created: no magic yet
    CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK);
    scw_writer_init(&w.ring, w.header, w.len, 64, 48, 256, 3, 3001, 30000, getpid(), "lifecycle");
    t += S / 2;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);                 // within 1 s of the last attempt: no re-open
    CHECK(r.opens == 0 && strstr(r.why, "no ring") != NULL);
    t += S / 2;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);
    CHECK(r.status == SCW_E_NOTREADY);
    scw_writer_publish(&w.ring); scw_writer_state(&w.ring, SCW_STATE_PROMPTING);
    t += S;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);                 // mapped, but prompting: offline with the reason
    CHECK(r.opens == 1 && pk.state == SCW_STATE_PROMPTING && strstr(r.why, "prompting"));
    scw_writer_state(&w.ring, SCW_STATE_RUNNING);
    wput(&w, 1, t);
    CHECKEQ(scw_shm_reader_poll(&r, t + 1000, &pk), 1);           // live
    // frames stop (a hung or crashed helper): offline after 500 ms; the same object re-opened is not re-mapped
    t += S * 6 / 10;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);
    CHECK(strstr(r.why, "stale") && r.opens == 1);
    // a new helper replaces the object without a state change (the old one crashed): picked up within 1 s
    CHECKEQ(scw_shm_create(&w2, N1, RB, &rep), SCW_OK); publish(&w2, 3002); scw_writer_state(&w2.ring, SCW_STATE_RUNNING);
    wput(&w2, 1, t);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);                 // last attempt was < 1 s ago
    t += S;
    wput(&w2, 2, t);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 1);
    CHECK(r.opens == 2 && r.geom.epoch == 3002);
    munmap(w.header, (size_t)w.len); close(w.fd);                // the "crashed" helper's leftovers
    // superseded, in the helper's order: the new object is created and published first (scw_shm_create unlinks the
    // old name; the old mapping stays valid), then the old one is retired as superseded. The reader re-opens at once,
    // in the same poll, and finds the new object.
    scw_shm_writer w3;
    CHECKEQ(scw_shm_create(&w3, N1, RB, &rep), SCW_OK); publish(&w3, 3003); scw_writer_state(&w3.ring, SCW_STATE_RUNNING);
    wput(&w3, 1, t + 10);
    scw_shm_retire(&w2, SCW_STATE_SUPERSEDED);                  // the name holds 3003, so it is not unlinked
    CHECKEQ(scw_shm_reader_poll(&r, t + 20, &pk), 1);
    CHECK(r.opens == 3 && r.geom.epoch == 3003);
    // the wrong order (old marked superseded while the name still holds it): the reader drops it and never
    // re-adopts that epoch; the new object is picked up on the next 1 s attempt
    scw_writer_state(&w3.ring, SCW_STATE_SUPERSEDED);
    t += S;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);
    CHECK(r.map == NULL && r.opens == 3 && strstr(r.why, "waiting for the helper's new ring"));
    scw_shm_writer w4;
    CHECKEQ(scw_shm_create(&w4, N1, RB, &rep), SCW_OK); publish(&w4, 3005); scw_writer_state(&w4.ring, SCW_STATE_RUNNING);
    wput(&w4, 1, t + S / 2);
    CHECKEQ(scw_shm_reader_poll(&r, t + S / 2, &pk), 0);         // the schedule holds: < 1 s since the last attempt
    t += S;
    wput(&w4, 2, t);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 1);
    CHECK(r.opens == 4 && r.geom.epoch == 3005);
    scw_shm_retire(&w3, SCW_STATE_SUPERSEDED);
    {   // retire unlinks by the private epoch, even when the shared header's epoch was rewritten
        scw_shm_writer w5;
        CHECKEQ(scw_shm_create(&w5, N2, RB, &rep), SCW_OK); publish(&w5, 4001);
        w5.header->epoch = 4002;                                // another same-user process rewrites it
        w5.header->nslots = 0; w5.header->slotBytes = 1ull << 40;  // and the geometry
        scw_shm_retire(&w5, SCW_STATE_STOPPED);                  // clears by the private geometry; no crash
        int fd = shm_open(N2, O_RDONLY, 0);
        CHECK(fd < 0 && errno == ENOENT);                        // still recognised as ours, and unlinked
        if (fd >= 0) {
            close(fd);
        }
        shm_unlink(N2);
    }
    w3 = w4;
    // stopped (down): offline, and back once a helper starts again
    scw_shm_retire(&w3, SCW_STATE_STOPPED);
    t += S;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);
    CHECK(r.map == NULL && (strstr(r.why, "stopped") || strstr(r.why, "no ring")));
    // a ring of a version this engine doesn't accept: the "update stuntcam" line
    CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK); publish(&w, 3004); w.header->version = 2;
    t += S;
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);
    CHECK(!strcmp(r.why, "webcam: update stuntcam (ring v2, engine accepts v1)") && r.status == SCW_E_VERSION);
    printf("reader offline reason for a v2 ring: %s\n", r.why);
    w.header->version = 1;
    scw_shm_retire(&w, SCW_STATE_STOPPED);
    scw_shm_reader_close(&r);
    shm_cleanup();
}

// a reader's size limit (the engine's 4096x2160, either orientation): a larger valid ring stays offline, unmapped
static void test_reader_limit(void) {
    shm_names(); shm_cleanup();
    scw_shm_writer w; scw_shm_report rep; scw_shm_reader r; scw_peek_t pk;
    const uint64_t S = 1000000000ull;
    uint64_t t = 200 * S;
    CHECKEQ(scw_shm_create(&w, N1, RB, &rep), SCW_OK); publish(&w, 5001); scw_writer_state(&w.ring, SCW_STATE_RUNNING);
    wput(&w, 1, t);
    scw_shm_reader_init(&r, N1, V1, 1);
    r.maxWidth = 63; r.maxHeight = 48;                            // the 64x48 ring is one pixel too wide
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);
    CHECK(r.map == NULL && r.opens == 0 && r.status == SCW_E_LIMIT);
    CHECK(strstr(r.why, "ring 64x48 is larger than this engine accepts (63x48)") != NULL);
    printf("reader limit: %s\n", r.why);
    t += S; wput(&w, 2, t);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 0);                 // still refused on the next attempt
    CHECK(r.map == NULL && r.status == SCW_E_LIMIT);
    r.maxWidth = 48; r.maxHeight = 64;                            // portrait limit, landscape ring: accepted
    t += S; wput(&w, 3, t);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 1);
    CHECK(r.opens == 1 && r.geom.width == 64);
    scw_shm_reader_close(&r);
    r.maxWidth = 0; r.maxHeight = 0;                              // no limit
    scw_shm_reader_init(&r, N1, V1, 1);
    CHECKEQ(scw_shm_reader_poll(&r, t, &pk), 1);
    scw_shm_reader_close(&r);
    scw_shm_retire(&w, SCW_STATE_STOPPED);
    // the engine's limit: a 4097x1 ring is refused, 4096x1 accepted
    for (uint32_t wd = 4097; wd >= 4096; wd--) {
        uint64_t bytes = scw_object_bytes(1, wd * 4, 2, NULL);
        CHECKEQ(scw_shm_create(&w, N2, bytes, &rep), SCW_OK);
        CHECKEQ(scw_writer_init(&w.ring, w.header, w.len, wd, 1, wd * 4, 2, 5002 + wd, 30000, getpid(), "wide"), SCW_OK);
        scw_writer_publish(&w.ring); scw_writer_state(&w.ring, SCW_STATE_RUNNING);
        uint8_t *p = scw_writer_begin(&w.ring, 1); memset(p, 1, wd * 4); scw_writer_end(&w.ring, 1, t, t, 0);
        scw_shm_reader_init(&r, N2, V1, 1); r.maxWidth = 4096; r.maxHeight = 2160;
        int live = scw_shm_reader_poll(&r, t, &pk);
        if (wd == 4097) {
            CHECK(live == 0 && r.status == SCW_E_LIMIT && strstr(r.why, "4097x1") && strstr(r.why, "(4096x2160)"));
        } else {
            CHECK(live == 1 && r.geom.width == 4096);
        }
        scw_shm_reader_close(&r);
        scw_shm_retire(&w, SCW_STATE_STOPPED);
    }
    CHECK(!strcmp(scw_strerror(SCW_E_LIMIT), "ring larger than this reader accepts"));
    shm_cleanup();
}

int main(int argc, char **argv) {
    int shm = 1;
    double stress = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-shm")) {
            shm = 0;
        } else if (!strcmp(argv[i], "--stress") && i + 1 < argc) {
            stress = atof(argv[++i]);
        }
    }
    test_validate();
    test_writer_init();
    test_reads();
    test_clear();
    test_writer_hostile();
    if (stress > 0) {
        test_writer_scribbled(stress / 2);
    }
    test_names();
    test_pidfile();
    if (stress > 0) {
        test_stress(stress);
    }
    if (shm) { test_shm(); test_reader_lifecycle(); test_reader_limit(); }
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
