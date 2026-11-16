// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the webcam ring parser (ring.c). Run by engine/src/webcam/test.sh.
// libFuzzer:   clang -fsanitize=fuzzer,address,undefined ring_fuzz.c
// without it:  clang -DSCW_FUZZ_STANDALONE -fsanitize=address,undefined ring_fuzz.c   (a deterministic mutation loop:
//              ring_fuzz [-seconds N] [-seed S]; also `ring_fuzz --write-seeds DIR` writes the seed corpus)
//
// The first input byte picks a target:
//   0  raw:    the rest of the input is the whole mapping; validate it, then peek and read through it
//   1  sized:  input[1..4097) is the header; the mapping is the size the header claims, plus or minus a delta
//              (input[4097]), up to 8 MiB; the rest is a script that rewrites shared fields between and during reads
//   2  pid file text through scw_pidfile_parse (a parsed file must format and parse back to the same fields)
//   3  names and messages: home / uniqueID / uid / version / accepted lists from the input
//   4  writer: a ring of fuzzed geometry and mapping length, then a script of writer calls interleaved with hostile
//      rewrites of the shared header and slot records (between calls and between begin and end), and reader passes
//      over the result; the writer's private geometry must never change and its writes must stay in the mapping
// Mappings are heap blocks of exactly their length, so ASan reports any read past the end.
#include "ring.c"
#include <stdlib.h>

static const uint32_t ACC1[] = {1};
static const uint32_t ACC12[] = {1, 2};

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void reads(uint8_t *map, const scw_geom *g, const uint8_t *ops, size_t nops) {
    size_t dbpr = (size_t)g->width * 4 + (ops && nops ? ops[0] % 64 : 0);
    size_t dlen = dbpr * g->height;
    uint8_t *dst = malloc(dlen ? dlen : 1);
    scw_header *h = (scw_header *)map;
    uint64_t last = 0;
    scw_peek_t pk;
    scw_slot_read sr;
    scw_peek(map, g, &pk);
    scw_is_live(&pk, rd64((const uint8_t *)h + 96));
    if (scw_read_latest(map, g, last, dst, dbpr, dlen, &sr) == SCW_OK) last = sr.seq;
    // script: 11-byte ops (kind, offset16, value64)
    for (size_t i = 0; i + 11 <= nops; i += 11) {
        uint8_t kind = ops[i] % 6;
        uint16_t off = (uint16_t)(ops[i + 1] | ops[i + 2] << 8);
        uint64_t v = rd64(ops + i + 3);
        switch (kind) {
        case 0: memcpy(map + (off % (SCW_HDR - 7)), &v, 8); break;                 // rewrite any header bytes
        case 1: h->latest = v; break;
        case 2: {                                                                     // make the latest slot look complete
            uint64_t s = h->latest;
            uint32_t idx = (uint32_t)(s % g->nslots);
            h->slot[idx].seq = s; h->slot[idx].gen = 2 * s + 2; h->slot[idx].publishNs = v;
            break;
        }
        case 3:
            if (scw_read_latest(map, g, last, dst, dbpr, dlen, &sr) == SCW_OK) last = sr.seq;
            scw_peek(map, g, &pk);
            scw_is_live(&pk, v);
            break;
        case 4:                                                                       // a rewrite in the middle of a read
            if (scw_slot_begin(map, g, last, &sr) == SCW_OK) {
                volatile uint8_t sink = sr.pixels[0];
                sink ^= sr.pixels[(size_t)g->bpr0 * g->height - 1];
                (void)sink;
                memcpy(map + (off % (SCW_HDR - 7)), &v, 8);
                if (scw_slot_end(map, g, &sr) == SCW_OK) last = sr.seq;
            }
            break;
        case 5: {                                                                     // re-validate the mutated header
            scw_geom g2; uint32_t fv;
            if (scw_validate(map, g->mapBytes, ACC12, 2, &g2, &fv) == SCW_OK && g2.mapBytes != g->mapBytes) abort();
            break;
        }
        }
    }
    free(dst);
}

static void target_raw(const uint8_t *d, size_t n) {
    uint8_t *map = malloc(n ? n : 1);
    if (n) memcpy(map, d, n);
    scw_geom g; uint32_t fv;
    if (scw_validate(map, n, ACC1, 1, &g, &fv) == SCW_OK) {
        if (g.totalBytes > n || g.mapBytes != n) abort();
        reads(map, &g, NULL, 0);
    }
    scw_validate(map, n, ACC12, 2, &g, &fv);
    scw_validate(map, n, NULL, 0, &g, &fv);
    free(map);
}

static void target_sized(const uint8_t *d, size_t n) {
    uint8_t hdr[SCW_HDR];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, d, n < SCW_HDR ? n : SCW_HDR);
    const scw_header *h = (const scw_header *)hdr;
    int8_t delta = n > SCW_HDR ? (int8_t)d[SCW_HDR] : 0;
    uint64_t claim = h->totalBytes;
    if (claim > (8u << 20)) claim = SCW_HDR + (claim & 0xffff);
    int64_t len = (int64_t)claim + (int64_t)delta * 64;
    if (len < 0) len = 0;
    uint8_t *map = calloc(1, (size_t)len ? (size_t)len : 1);
    memcpy(map, hdr, (size_t)len < SCW_HDR ? (size_t)len : SCW_HDR);
    scw_geom g; uint32_t fv;
    if (scw_validate(map, (uint64_t)len, ACC1, 1, &g, &fv) == SCW_OK) {
        if (g.totalBytes > (uint64_t)len) abort();
        const uint8_t *ops = n > SCW_HDR + 1 ? d + SCW_HDR + 1 : NULL;
        reads(map, &g, ops, ops ? n - SCW_HDR - 1 : 0);
    }
    free(map);
}

static void target_pid(const uint8_t *d, size_t n) {
    char *t = malloc(n ? n : 1);
    if (n) memcpy(t, d, n);
    scw_pidfile p, q;
    if (scw_pidfile_parse(t, n, &p) == SCW_OK) {
        char buf[1024];
        int k = scw_pidfile_format(buf, sizeof buf, &p);
        if (k <= 0 || scw_pidfile_parse(buf, (size_t)k, &q) != SCW_OK) abort();
        if (q.pid != p.pid || strcmp(q.sha, p.sha) || q.nrings != p.nrings || q.auth != p.auth || q.state != p.state ||
            strcmp(q.camera, p.camera) || q.started != p.started || memcmp(q.rings, p.rings, sizeof q.rings))
            abort();
    }
    free(t);
}

static void target_names(const uint8_t *d, size_t n) {
    if (n < 12) return;
    unsigned long uid = (unsigned long)rd64(d);
    uint32_t ver; memcpy(&ver, d + 8, 4);
    size_t split = n > 12 ? 12 + (d[12] % (n - 12 + 1)) : 12;
    char *home = calloc(1, split - 12 + 1), *uq = calloc(1, n - split + 1);
    memcpy(home, d + 12, split - 12);
    memcpy(uq, d + split, n - split);
    char hash[9], name[64], small[8];
    scw_cam_hash(home, uq, hash);
    if (strlen(hash) != 8) abort();
    int k = scw_shm_name(name, sizeof name, uid, hash, ver);
    if (k > (int)SCW_NAME_MAX) abort();
    scw_shm_name(small, sizeof small, uid, hash, ver);
    scw_shm_name(name, sizeof name, uid, home, ver);   // arbitrary text as the hash: must be refused or bounded
    uint32_t acc[16];
    unsigned na = (unsigned)((n - 12) / 4 > 16 ? 16 : (n - 12) / 4);
    memcpy(acc, d + 12, na * 4);
    char msg[160];
    int m = scw_version_message(msg, sizeof msg, ver, acc, na);
    if (m < 0) abort();
    scw_version_message(small, sizeof small, ver, acc, na);
    scw_pick_version(acc, na, ACC12, 2);
    scw_strerror((int)ver); scw_state_name(ver); scw_auth_name((int)ver);
    free(home); free(uq);
}

static void target_writer(const uint8_t *d, size_t n) {
    if (n < 5) return;
    uint32_t w = 1 + d[0] % 64, h = 1 + d[1] % 64, nslots = d[2] >= 250 ? d[2] - 250 : 1 + d[2] % 8;
    uint32_t bpr = w * 4 + (d[3] % 8) * 4;
    uint64_t total = scw_object_bytes(h, bpr, nslots ? nslots : 1, NULL);
    int64_t len = (int64_t)total + (int64_t)(int8_t)d[4] * 16;
    if (len < 0) len = 0;
    uint8_t *map = calloc(1, (size_t)len ? (size_t)len : 1);
    scw_writer wr;
    if (scw_writer_init(&wr, map, (uint64_t)len, w, h, bpr, nslots, 0x77, 30000, 1, "fuzz") != SCW_OK) {
        if (scw_writer_begin(&wr, 1)) abort();                    // a failed init leaves a writer that writes nothing
        free(map);
        return;
    }
    scw_writer_publish(&wr);
    const scw_writer saved = wr;
    scw_header *hd = (scw_header *)map;
    const uint8_t *ops = d + 5;
    size_t nops = n - 5;
    for (size_t i = 0; i + 11 <= nops; i += 11) {
        uint8_t kind = ops[i] % 8;
        uint16_t off = (uint16_t)(ops[i + 1] | ops[i + 2] << 8);
        uint64_t v = rd64(ops + i + 3);
        switch (kind) {
        case 0: memcpy(map + (off % (SCW_HDR - 7)), &v, 8); break;               // hostile header bytes
        case 1: {                                                                   // hostile slot record
            scw_slot *s = &hd->slot[off % SCW_MAX_SLOTS];
            if (off & 0x100) s->gen = v; else if (off & 0x200) s->seq = v; else hd->latest = v;
            break;
        }
        case 2: case 3: {                                                           // a frame (2 complete, 3 left busy)
            uint64_t seq = v % 5 == 0 ? wr.lastSeq : wr.lastSeq + 1 + (v >> 8) % 3;
            uint8_t *p = scw_writer_begin(&wr, seq);
            if (p) {
                if (off & 1) memcpy(map + ((off >> 1) % (SCW_HDR - 7)), &v, 8);     // rewritten mid-frame
                memset(p, (int)(seq & 0xff), (size_t)wr.bpr0 * wr.height);
                if (kind == 2) scw_writer_end(&wr, v & 0x10000 ? seq + 1 : seq, v, v, 0);
            }
            break;
        }
        case 4: scw_writer_end(&wr, v, v, v, (uint32_t)v); break;
        case 5: scw_writer_clear(&wr); break;
        case 6: if (off & 1) scw_writer_beat(&wr, v); else scw_writer_state(&wr, (uint32_t)(v % 10)); break;
        case 7: {                                                                   // a reader over the result
            scw_geom g; uint32_t fv;
            if (scw_validate(map, (uint64_t)len, ACC1, 1, &g, &fv) == SCW_OK) reads(map, &g, NULL, 0);
            break;
        }
        }
        if (wr.h != saved.h || wr.mapBytes != saved.mapBytes || wr.nslots != saved.nslots || wr.slotBytes != saved.slotBytes ||
            wr.bpr0 != saved.bpr0 || wr.height != saved.height || wr.epoch != saved.epoch || wr.lastSeq < saved.lastSeq)
            abort();
    }
    free(map);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    const uint8_t *d = data + 1;
    size_t n = size - 1;
    switch (data[0] % 5) {
    case 0: target_raw(d, n); break;
    case 1: target_sized(d, n); break;
    case 2: target_pid(d, n); break;
    case 3: target_names(d, n); break;
    case 4: target_writer(d, n); break;
    }
    return 0;
}

// ---- seeds and the standalone driver

typedef struct { uint8_t *p; size_t n; } buf_t;

static buf_t seed_ring(int target, uint32_t w, uint32_t hgt, uint32_t nslots, int with_ops) {
    uint64_t slot, total = scw_object_bytes(hgt, w * 4, nslots, &slot);
    buf_t b;
    size_t ops = with_ops ? 11 * 8 : 0;
    b.n = 1 + (target == 0 ? (size_t)total : SCW_HDR + 1 + ops);
    b.p = calloc(1, b.n);
    b.p[0] = (uint8_t)target;
    scw_header h;
    scw_header_init(&h, w, hgt, w * 4, nslots, 0x51, 30000, 77, "seed camera");
    h.magic = SCW_MAGIC; h.state = SCW_STATE_RUNNING;
    h.latest = 3; h.slot[3 % nslots].seq = 3; h.slot[3 % nslots].gen = 8; h.slot[3 % nslots].publishNs = 5;
    memcpy(b.p + 1, &h, sizeof h);
    if (target == 1 && with_ops) {
        uint8_t *o = b.p + 1 + SCW_HDR + 1;
        const uint8_t kinds[8] = {3, 1, 2, 3, 4, 0, 5, 3};
        for (int i = 0; i < 8; i++) {
            o[i * 11] = kinds[i];
            o[i * 11 + 1] = (uint8_t)(i * 8 + 24);                // offsets into width/height/... fields
            uint64_t v = i == 1 ? 4 : (uint64_t)i * 1000;
            memcpy(o + i * 11 + 3, &v, 8);
        }
    }
    return b;
}

static buf_t seed_writer(void) {
    static const uint8_t kinds[] = {2, 2, 7, 0, 2, 3, 2, 7, 1, 2, 5, 2, 7, 6, 4, 7};
    buf_t b;
    b.n = 1 + 5 + 11 * sizeof kinds;
    b.p = calloc(1, b.n);
    b.p[0] = 4; b.p[1] = 15; b.p[2] = 7; b.p[3] = 2; b.p[4] = 1; b.p[5] = 0;
    for (unsigned i = 0; i < sizeof kinds; i++) {
        uint8_t *o = b.p + 6 + i * 11;
        o[0] = kinds[i]; o[1] = (uint8_t)(24 + i * 8); o[2] = 0;
        uint64_t v = 1000 + i;
        memcpy(o + 3, &v, 8);
    }
    return b;
}

static int write_seeds(const char *dir) {
    buf_t seeds[16];
    int k = 0;
    seeds[k++] = seed_ring(0, 16, 8, 2, 0);
    seeds[k++] = seed_ring(0, 64, 4, 4, 0);
    seeds[k++] = seed_ring(1, 64, 48, 3, 1);
    seeds[k++] = seed_ring(1, 320, 240, 4, 1);
    seeds[k++] = seed_ring(1, 8, 8, 8, 1);
    const char *pid = "\x02stuntcam-webcam 1\npid=4321\nsha=0123456789ab\nrings=1,2\nauth=prompting\nstate=prompting\n"
                      "camera=0x1420000005ac8600\nstarted=1794790000\n";
    seeds[k].n = strlen(pid); seeds[k].p = (uint8_t *)strdup(pid); k++;
    const char *nm = "\x03\xf5\x01\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x2f/Users/x/Library/Application Support/stuntcam0x14200";
    seeds[k++] = seed_writer();
    seeds[k].n = 1 + 12 + 50; seeds[k].p = malloc(seeds[k].n); memset(seeds[k].p, 'a', seeds[k].n);
    memcpy(seeds[k].p, nm, strlen(nm) < seeds[k].n ? strlen(nm) : seeds[k].n); k++;
    for (int i = 0; i < k; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/seed-%02d", dir, i);
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fwrite(seeds[i].p, 1, seeds[i].n, f);
        fclose(f);
        free(seeds[i].p);
    }
    printf("wrote %d seeds to %s\n", k, dir);
    return 0;
}

#ifdef SCW_FUZZ_STANDALONE
#include <time.h>
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static const uint64_t interesting[] = {0, 1, 2, 3, 4, 7, 8, 9, 0x7f, 0x80, 0xff, 0x100, 0xfff, 0x1000, 0x1001, 0x7fff,
    0x8000, 0xffff, 0x10000, 8192, 8193, 0x7fffffff, 0x80000000ull, 0xffffffffull, 0x100000000ull, 1ull << 30,
    (1ull << 30) + 1, 1ull << 40, 1ull << 63, 0xfffffffffffff000ull, UINT64_MAX, SCW_MAGIC, SCW_FORMAT_BGRA};

static void mutate(uint8_t *p, size_t n) {
    if (!n) return;
    int k = 1 + (int)(next() % 4);
    for (int i = 0; i < k; i++) {
        size_t at = next() % n;
        switch (next() % 5) {
        case 0: p[at] ^= (uint8_t)(1u << (next() % 8)); break;
        case 1: p[at] = (uint8_t)next(); break;
        case 2: {                                                 // an interesting value at a 4- or 8-byte field
            uint64_t v = interesting[next() % (sizeof interesting / sizeof interesting[0])];
            size_t w = next() % 2 ? 4 : 8;
            at = 1 + ((at / w) * w) % (n > w + 1 ? n - w - 1 : 1);
            if (at + w <= n) memcpy(p + at, &v, w);
            break;
        }
        case 3: if (n > 2) { size_t b = next() % n; p[at] = p[b]; } break;
        case 4: p[at] = (uint8_t)(p[at] + (int8_t)(next() % 33 - 16)); break;
        }
    }
}

int main(int argc, char **argv) {
    double seconds = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--write-seeds") && i + 1 < argc) return write_seeds(argv[i + 1]);
        if (!strncmp(argv[i], "-seconds", 8) && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strncmp(argv[i], "-seed", 5) && i + 1 < argc) rng = strtoull(argv[++i], NULL, 0) | 1;
    }
    buf_t pool[8];
    int np = 0;
    pool[np++] = seed_ring(0, 16, 8, 2, 0);
    pool[np++] = seed_ring(1, 64, 48, 3, 1);
    pool[np++] = seed_ring(1, 8, 8, 8, 1);
    const char *pid = "\x02stuntcam-webcam 1\npid=4321\nsha=0123456789ab\nrings=1,2\nauth=prompting\nstate=running\n";
    pool[np].n = strlen(pid); pool[np].p = (uint8_t *)strdup(pid); np++;
    pool[np].n = 80; pool[np].p = calloc(1, 80); pool[np].p[0] = 3; memset(pool[np].p + 13, 'x', 60); np++;
    pool[np++] = seed_writer();
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long long iters = 0;
    uint8_t *work = NULL;
    size_t cap = 0;
    for (;;) {
        buf_t *s = &pool[next() % np];
        size_t n = s->n;
        if (next() % 8 == 0) n = next() % (s->n + 64);           // truncate or extend
        if (n > cap) { cap = n; work = realloc(work, cap); }
        memset(work, 0, n);
        memcpy(work, s->p, n < s->n ? n : s->n);
        mutate(work, n);
        if (n && work) work[0] = s->p[0];                         // keep the target
        uint8_t *exact = malloc(n ? n : 1);
        memcpy(exact, work, n);
        LLVMFuzzerTestOneInput(exact, n);
        free(exact);
        if ((++iters & 4095) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (el >= seconds) { printf("standalone: %llu inputs in %.0f s, 0 crashes\n", iters, el); break; }
        }
    }
    free(work);
    for (int i = 0; i < np; i++) free(pool[i].p);
    return 0;
}
#elif defined(SCW_FUZZ_SEEDS)
int main(int argc, char **argv) { return argc == 2 ? write_seeds(argv[1]) : 2; }
#endif
