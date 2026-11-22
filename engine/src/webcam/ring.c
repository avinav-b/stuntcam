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
#define SCW_LOAD_ACQUIRE_U32(field) __atomic_load_n((const uint32_t *)(field), __ATOMIC_ACQUIRE)
#define SCW_LOAD_ACQUIRE_U64(field) __atomic_load_n((const uint64_t *)(field), __ATOMIC_ACQUIRE)
#define SCW_LOAD_RELAXED_U64(field) __atomic_load_n((const uint64_t *)(field), __ATOMIC_RELAXED)
#define SCW_LOAD_RELAXED_U32(field) __atomic_load_n((const uint32_t *)(field), __ATOMIC_RELAXED)
#define SCW_STORE_RELEASE_U32(field, value) __atomic_store_n((uint32_t *)(field), (value), __ATOMIC_RELEASE)
#define SCW_STORE_RELEASE_U64(field, value) __atomic_store_n((uint64_t *)(field), (value), __ATOMIC_RELEASE)
#define SCW_STORE_RELAXED_U64(field, value) __atomic_store_n((uint64_t *)(field), (value), __ATOMIC_RELAXED)

// A slot's seqlock value (scw_slot.gen) for frame `seq`: odd while the writer fills the slot, even once it is complete.
#define SCW_GEN_WRITING(seq) (2 * (seq) + 1)
#define SCW_GEN_COMPLETE(seq) (2 * (seq) + 2)
// Sequence numbers from here on are refused: their gen would overflow 64 bits.
#define SCW_SEQ_LIMIT ((UINT64_MAX - 2) / 2)

#define SCW_BYTES_PER_PIXEL 4         // BGRA
#define SCW_ROW_ALIGN_MASK 3u         // bpr0 is a whole number of 4-byte pixels
#define SCW_FNV_OFFSET 0xcbf29ce484222325ull   // FNV-1a 64-bit offset basis
#define SCW_FNV_PRIME 0x100000001b3ull         // FNV-1a 64-bit prime
#define SCW_PID_RING_MAX 0xffff       // the largest ring version the pid file's rings= line accepts
#define SCW_U64_DIGITS_MAX 20         // decimal digits of UINT64_MAX
#define SCW_LIST_BYTES 96             // the "v1, v2" list of scw_version_message and the rings= value of the pid file
#define SCW_LIST_ITEM_BYTES 16        // bytes kept free for one more ", v<uint32>" entry and its NUL

static int scw_accepts(const uint32_t *accepted, unsigned naccepted, uint32_t version) {
    for (unsigned i = 0; accepted && i < naccepted; i++) {
        if (accepted[i] == version) {
            return 1;
        }
    }
    return 0;
}

// Copies a device name out of shared memory: stops at NUL or SCW_DEVICE_LEN-1 bytes, keeps well-formed UTF-8
// (no overlong forms, surrogates or code points above U+10FFFF, no sequence cut at the end), and replaces every other
// byte, and every control character, with '?'. The result is always terminated and safe to log.
static void scw_copy_device(char out[SCW_DEVICE_LEN], const char *shared) {
    unsigned char bytes[SCW_DEVICE_LEN];
    unsigned count = 0;

    while (count < SCW_DEVICE_LEN - 1) {
        bytes[count] = (unsigned char)__atomic_load_n(&shared[count], __ATOMIC_RELAXED);   // one read per byte
        if (!bytes[count]) {
            break;
        }
        count++;
    }

    unsigned pos = 0;
    unsigned out_len = 0;

    while (pos < count) {
        // The lead byte gives the sequence length and the top bits of the code point.
        unsigned lead = bytes[pos];
        unsigned seq_len = 0;
        uint32_t code_point = 0;
        if (lead < 0x80) {
            seq_len = 1;
            code_point = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            seq_len = 2;
            code_point = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            seq_len = 3;
            code_point = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            seq_len = 4;
            code_point = lead & 0x07;
        }
        int ok = seq_len > 0 && pos + seq_len <= count;
        for (unsigned k = 1; ok && k < seq_len; k++) {
            if ((bytes[pos + k] & 0xc0) != 0x80) {
                ok = 0;   // not a continuation byte
            } else {
                code_point = code_point << 6 | (bytes[pos + k] & 0x3f);
            }
        }
        // overlong forms, UTF-16 surrogates, and code points above U+10FFFF
        int bad_three_byte = seq_len == 3 && (code_point < 0x800 || (code_point >= 0xd800 && code_point <= 0xdfff));
        int bad_four_byte = seq_len == 4 && (code_point < 0x10000 || code_point > 0x10ffff);
        if (ok && (bad_three_byte || bad_four_byte)) {
            ok = 0;
        }
        if (ok && seq_len == 1 && (lead < 0x20 || lead == 0x7f)) {
            ok = 0;   // C0 controls and DEL
        }
        if (ok && seq_len == 2 && code_point < 0xa0) {
            ok = 0;   // C1 controls (U+0080-U+009F)
        }
        if (!ok) {
            out[out_len++] = '?';
            pos++;
            continue;
        }
        for (unsigned k = 0; k < seq_len; k++) {
            out[out_len++] = (char)bytes[pos + k];
        }
        pos += seq_len;
    }

    out[out_len] = 0;
}

uint64_t scw_object_bytes(uint32_t height, uint32_t bpr0, uint32_t nslots, uint64_t *slot_bytes) {
    uint64_t plane_bytes;
    uint64_t slot_size;
    uint64_t all_slots_bytes;
    uint64_t total_bytes;
    if (!height || !bpr0 || !nslots || nslots > SCW_MAX_SLOTS) {
        return 0;
    }
    if (__builtin_mul_overflow((uint64_t)bpr0, (uint64_t)height, &plane_bytes)) {
        return 0;
    }
    // round the plane up to a whole number of SCW_SLOT_ALIGN pages
    if (__builtin_add_overflow(plane_bytes, (uint64_t)SCW_SLOT_ALIGN - 1, &slot_size)) {
        return 0;
    }
    slot_size &= ~(uint64_t)(SCW_SLOT_ALIGN - 1);
    if (__builtin_mul_overflow(slot_size, (uint64_t)nslots, &all_slots_bytes)) {
        return 0;
    }
    if (__builtin_add_overflow(all_slots_bytes, (uint64_t)SCW_HDR, &total_bytes)) {
        return 0;
    }
    if (total_bytes > SCW_MAX_OBJECT) {
        return 0;
    }
    if (slot_bytes) {
        *slot_bytes = slot_size;
    }
    return total_bytes;
}

// One relaxed load of every header field validation reads, taken after the magic's acquire: the checks and the private
// copy use only this snapshot, never the shared header again.
typedef struct {
    scw_geom geom;          // the fields the reader keeps
    uint32_t headerBytes;   // the fields only checked
    uint32_t state;
    uint32_t bpr1;
    uint32_t flags;
    uint64_t plane1Off;
} scw_snapshot;

// The mapping's identity: a stored magic (acquire: pairs with scw_writer_publish, so the other fields are complete)
// and a version this reader accepts. *version gets the header's version once the magic is good.
static int scw_check_identity(const scw_header *header, const uint32_t *accepted, unsigned naccepted,
                              uint32_t *version) {
    uint32_t magic = SCW_LOAD_ACQUIRE_U32(&header->magic);
    if (magic == 0) {
        return SCW_E_NOTREADY;
    }
    if (magic != SCW_MAGIC) {
        return SCW_E_MAGIC;
    }
    *version = SCW_LOAD_RELAXED_U32(&header->version);
    return scw_accepts(accepted, naccepted, *version) ? SCW_OK : SCW_E_VERSION;
}

static void scw_take_snapshot(const scw_header *header, uint32_t version, scw_snapshot *snapshot) {
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->headerBytes = SCW_LOAD_RELAXED_U32(&header->headerBytes);
    snapshot->state = SCW_LOAD_RELAXED_U32(&header->state);
    snapshot->bpr1 = SCW_LOAD_RELAXED_U32(&header->bpr1);
    snapshot->flags = SCW_LOAD_RELAXED_U32(&header->flags);
    snapshot->plane1Off = SCW_LOAD_RELAXED_U64(&header->plane1Off);
    scw_geom *geom = &snapshot->geom;
    geom->version = version;
    geom->epoch = SCW_LOAD_RELAXED_U64(&header->epoch);
    geom->width = SCW_LOAD_RELAXED_U32(&header->width);
    geom->height = SCW_LOAD_RELAXED_U32(&header->height);
    geom->format = SCW_LOAD_RELAXED_U32(&header->format);
    geom->nslots = SCW_LOAD_RELAXED_U32(&header->nslots);
    geom->bpr0 = SCW_LOAD_RELAXED_U32(&header->bpr0);
    geom->slotBytes = SCW_LOAD_RELAXED_U64(&header->slotBytes);
    geom->totalBytes = SCW_LOAD_RELAXED_U64(&header->totalBytes);
    geom->fpsMilli = SCW_LOAD_RELAXED_U32(&header->fpsMilli);
    geom->producerPid = (int32_t)SCW_LOAD_RELAXED_U32(&header->producerPid);
}

// The header itself: a known state, the one pixel format, this header size, a non-zero epoch, no flags.
static int scw_check_header(const scw_snapshot *snapshot) {
    if (snapshot->state >= SCW_STATE_COUNT) {
        return SCW_E_STATE;
    }
    if (snapshot->geom.format != SCW_FORMAT_BGRA) {
        return SCW_E_FORMAT;
    }
    if (snapshot->headerBytes != SCW_HDR || snapshot->geom.epoch == 0 || snapshot->flags != 0) {
        return SCW_E_GEOMETRY;
    }
    return SCW_OK;
}

// The picture: a slot count and dimensions within the limits; a row holds the width in whole pixels; a slot is
// page-aligned and holds the one plane.
static int scw_check_picture(const scw_snapshot *snapshot) {
    const scw_geom *geom = &snapshot->geom;
    if (geom->nslots < 1 || geom->nslots > SCW_MAX_SLOTS) {
        return SCW_E_GEOMETRY;
    }
    if (geom->width < 1 || geom->width > SCW_MAX_DIM) {
        return SCW_E_GEOMETRY;
    }
    if (geom->height < 1 || geom->height > SCW_MAX_DIM) {
        return SCW_E_GEOMETRY;
    }
    uint64_t row_bytes;
    uint64_t plane_bytes;
    if (__builtin_mul_overflow((uint64_t)geom->width, (uint64_t)SCW_BYTES_PER_PIXEL, &row_bytes)) {
        return SCW_E_GEOMETRY;
    }
    if ((uint64_t)geom->bpr0 < row_bytes || (geom->bpr0 & SCW_ROW_ALIGN_MASK)) {
        return SCW_E_GEOMETRY;
    }
    if (geom->slotBytes == 0 || (geom->slotBytes % SCW_SLOT_ALIGN) != 0) {
        return SCW_E_GEOMETRY;
    }
    if (__builtin_mul_overflow((uint64_t)geom->bpr0, (uint64_t)geom->height, &plane_bytes)) {
        return SCW_E_GEOMETRY;
    }
    if (plane_bytes > geom->slotBytes) {
        return SCW_E_GEOMETRY;
    }
    if (snapshot->plane1Off != 0 || snapshot->bpr1 != 0) {
        return SCW_E_GEOMETRY;   // BGRA is single-plane
    }
    return SCW_OK;
}

// The object: the header and the slots add up to totalBytes, which fits in the mapping.
static int scw_check_object_size(const scw_geom *geom, uint64_t map_len) {
    uint64_t all_slots_bytes;
    uint64_t total_bytes;
    if (__builtin_mul_overflow(geom->slotBytes, (uint64_t)geom->nslots, &all_slots_bytes)) {
        return SCW_E_SIZE;
    }
    if (__builtin_add_overflow(all_slots_bytes, (uint64_t)SCW_HDR, &total_bytes)) {
        return SCW_E_SIZE;
    }
    if (total_bytes != geom->totalBytes) {
        return SCW_E_GEOMETRY;
    }
    if (total_bytes > map_len || total_bytes > SCW_MAX_OBJECT) {
        return SCW_E_SIZE;
    }
    return SCW_OK;
}

int scw_validate(const void *map, uint64_t map_len, const uint32_t *accepted, unsigned naccepted, scw_geom *geom,
                 uint32_t *found_version) {
    if (found_version) {
        *found_version = 0;
    }
    if (!map || !geom) {
        return SCW_E_ARG;
    }
    if (map_len < SCW_HDR) {
        return SCW_E_SHORT;
    }
    if (map_len > SCW_MAX_OBJECT) {
        return SCW_E_SIZE;
    }
    const scw_header *header = map;
    uint32_t version = 0;
    int rc = scw_check_identity(header, accepted, naccepted, &version);
    if (found_version) {
        *found_version = version;
    }
    if (rc != SCW_OK) {
        return rc;
    }

    scw_snapshot snapshot;
    scw_take_snapshot(header, version, &snapshot);
    rc = scw_check_header(&snapshot);
    if (rc == SCW_OK) {
        rc = scw_check_picture(&snapshot);
    }
    if (rc == SCW_OK) {
        rc = scw_check_object_size(&snapshot.geom, map_len);
    }
    if (rc != SCW_OK) {
        return rc;
    }

    snapshot.geom.mapBytes = map_len;
    scw_copy_device(snapshot.geom.device, header->device);
    *geom = snapshot.geom;
    return SCW_OK;
}

// The identity fields that must not change for the life of a mapping.
static int scw_same_object(const scw_header *header, const scw_geom *geom) {
    return SCW_LOAD_ACQUIRE_U32(&header->magic) == SCW_MAGIC &&
           SCW_LOAD_RELAXED_U32(&header->version) == geom->version &&
           SCW_LOAD_RELAXED_U64(&header->epoch) == geom->epoch;
}

int scw_peek(const void *map, const scw_geom *geom, scw_peek_t *out) {
    const scw_header *header = map;
    memset(out, 0, sizeof *out);
    if (!scw_same_object(header, geom)) {
        return SCW_E_EPOCH;
    }
    out->state = SCW_LOAD_ACQUIRE_U32(&header->state);
    uint64_t seq = SCW_LOAD_ACQUIRE_U64(&header->latest);
    out->latest = seq;
    if (!seq) {
        return SCW_OK;
    }
    uint32_t index = (uint32_t)(seq % geom->nslots);           // private nslots
    const scw_slot *slot = &header->slot[index];
    uint64_t gen_before = SCW_LOAD_ACQUIRE_U64(&slot->gen);
    uint64_t capture_ns = SCW_LOAD_RELAXED_U64(&slot->captureNs);
    uint64_t publish_ns = SCW_LOAD_RELAXED_U64(&slot->publishNs);
    uint64_t slot_seq = SCW_LOAD_RELAXED_U64(&slot->seq);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t gen_after = SCW_LOAD_RELAXED_U64(&slot->gen);

    // the timestamps belong to `latest` only if the slot held that complete frame throughout
    if (gen_before == gen_after && slot_seq == seq && seq < SCW_SEQ_LIMIT && gen_before == SCW_GEN_COMPLETE(seq)) {
        out->captureNs = capture_ns;
        out->publishNs = publish_ns;
        out->hasTimestamps = 1;
    }

    return SCW_OK;
}

int scw_is_live(const scw_peek_t *peek, uint64_t now_ns) {
    if (peek->state != SCW_STATE_RUNNING || !peek->latest || !peek->hasTimestamps) {
        return 0;
    }
    // A publish time in the future counts as fresh only within the liveness window. Clock skew is not expected: the
    // writer and the readers share the host clock.
    uint64_t age = now_ns >= peek->publishNs ? now_ns - peek->publishNs : peek->publishNs - now_ns;
    return age < SCW_LIVE_NS;
}

int scw_slot_begin(const void *map, const scw_geom *geom, uint64_t last_seq, scw_slot_read *slot_read) {
    const scw_header *header = map;
    memset(slot_read, 0, sizeof *slot_read);
    if (!scw_same_object(header, geom)) {
        return SCW_E_EPOCH;
    }
    uint64_t seq = SCW_LOAD_ACQUIRE_U64(&header->latest);
    if (!seq || seq == last_seq) {
        return SCW_NONE;
    }
    if (seq >= SCW_SEQ_LIMIT) {
        return SCW_E_TORN;   // gen would overflow: not a frame any writer makes
    }
    uint32_t index = (uint32_t)(seq % geom->nslots);
    const scw_slot *slot = &header->slot[index];
    uint64_t gen = SCW_LOAD_ACQUIRE_U64(&slot->gen);
    if (gen != SCW_GEN_COMPLETE(seq)) {
        return SCW_E_TORN;   // being written, or already reused for a newer frame
    }
    slot_read->seq = seq;
    slot_read->gen = gen;
    slot_read->index = index;
    slot_read->captureNs = SCW_LOAD_RELAXED_U64(&slot->captureNs);
    slot_read->publishNs = SCW_LOAD_RELAXED_U64(&slot->publishNs);
    slot_read->flags = SCW_LOAD_RELAXED_U32(&slot->flags);
    if (SCW_LOAD_RELAXED_U64(&slot->seq) != seq) {
        return SCW_E_TORN;
    }

    // offset = SCW_HDR + index * slotBytes. scw_validate checked SCW_HDR + nslots * slotBytes <= mapBytes and
    // bpr0 * height <= slotBytes.
    slot_read->pixels = (const uint8_t *)map + SCW_HDR + (uint64_t)index * geom->slotBytes;
    return SCW_OK;
}

int scw_slot_end(const void *map, const scw_geom *geom, const scw_slot_read *slot_read) {
    const scw_header *header = map;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);                      // the pixel and meta reads happen before the re-check
    if (SCW_LOAD_RELAXED_U64(&header->slot[slot_read->index].gen) != slot_read->gen) {
        return SCW_E_TORN;
    }
    if (!scw_same_object(header, geom)) {
        return SCW_E_EPOCH;
    }
    return SCW_OK;
}

int scw_read_latest(const void *map, const scw_geom *geom, uint64_t last_seq, void *dst, size_t dst_bpr,
                    size_t dst_len, scw_slot_read *slot_read) {
    uint64_t row_bytes = (uint64_t)geom->width * SCW_BYTES_PER_PIXEL;
    uint64_t dst_need;
    if (!dst || dst_bpr < row_bytes ||
        __builtin_mul_overflow((uint64_t)dst_bpr, (uint64_t)geom->height, &dst_need) || dst_need > dst_len) {
        return SCW_E_ARG;
    }
    int rc = scw_slot_begin(map, geom, last_seq, slot_read);
    if (rc != SCW_OK) {
        return rc;
    }
    const uint8_t *src = slot_read->pixels;
    uint8_t *dst_bytes = dst;
    if (dst_bpr == geom->bpr0) {
        memcpy(dst_bytes, src, (size_t)geom->bpr0 * geom->height);
    } else {
        for (uint32_t y = 0; y < geom->height; y++) {
            memcpy(dst_bytes + (size_t)y * dst_bpr, src + (size_t)y * geom->bpr0, (size_t)row_bytes);
        }
    }
    return scw_slot_end(map, geom, slot_read);
}

// ---- writer

int scw_header_init(scw_header *header, uint32_t width, uint32_t height, uint32_t bpr0, uint32_t nslots,
                    uint64_t epoch, uint32_t fpsMilli, int32_t pid, const char *device) {
    uint64_t slot_bytes;
    uint64_t total_bytes;
    if (!header || !epoch) {
        return SCW_E_ARG;
    }
    if (width < 1 || width > SCW_MAX_DIM || height < 1 || height > SCW_MAX_DIM) {
        return SCW_E_GEOMETRY;
    }
    if ((uint64_t)bpr0 < (uint64_t)width * SCW_BYTES_PER_PIXEL || (bpr0 & SCW_ROW_ALIGN_MASK)) {
        return SCW_E_GEOMETRY;
    }
    total_bytes = scw_object_bytes(height, bpr0, nslots, &slot_bytes);
    if (!total_bytes) {
        return SCW_E_GEOMETRY;
    }
    memset(header, 0, SCW_HDR > sizeof *header ? sizeof *header : SCW_HDR);
    header->version = SCW_VERSION;
    header->headerBytes = SCW_HDR;
    header->state = SCW_STATE_STARTING;
    header->epoch = epoch;
    header->width = width;
    header->height = height;
    header->format = SCW_FORMAT_BGRA;
    header->nslots = nslots;
    header->bpr0 = bpr0;
    header->slotBytes = slot_bytes;
    header->totalBytes = total_bytes;
    header->fpsMilli = fpsMilli;
    header->producerPid = pid;

    if (device) {
        strncpy(header->device, device, SCW_DEVICE_LEN - 1);
        header->device[SCW_DEVICE_LEN - 1] = 0;
    }

    return SCW_OK;
}

int scw_writer_init(scw_writer *writer, void *map, uint64_t map_len, uint32_t width, uint32_t height, uint32_t bpr0,
                    uint32_t nslots, uint64_t epoch, uint32_t fpsMilli, int32_t pid, const char *device) {
    if (!writer) {
        return SCW_E_ARG;
    }
    memset(writer, 0, sizeof *writer);
    if (!map || map_len < SCW_HDR) {
        return SCW_E_ARG;
    }
    scw_header header;                                            // built privately, then copied out
    int rc = scw_header_init(&header, width, height, bpr0, nslots, epoch, fpsMilli, pid, device);
    if (rc != SCW_OK) {
        return rc;
    }
    if (header.totalBytes > map_len) {
        return SCW_E_SIZE;
    }
    writer->header = map;
    writer->mapBytes = map_len;
    writer->width = width;
    writer->height = height;
    writer->bpr0 = bpr0;
    writer->nslots = nslots;
    writer->slotBytes = header.slotBytes;
    writer->totalBytes = header.totalBytes;
    writer->epoch = epoch;
    memset(map, 0, SCW_HDR);
    memcpy(map, &header, sizeof header);                          // magic stays 0 until scw_writer_publish
    return SCW_OK;
}

void scw_writer_publish(scw_writer *writer) {
    SCW_STORE_RELEASE_U32(&writer->header->magic, SCW_MAGIC);
}

static uint8_t *scw_writer_slot(const scw_writer *writer, uint32_t index) {
    // index < nslots, and scw_writer_init checked totalBytes <= mapBytes
    return (uint8_t *)writer->header + SCW_HDR + (uint64_t)index * writer->slotBytes;
}

uint8_t *scw_writer_begin(scw_writer *writer, uint64_t seq) {
    if (!writer->header || seq <= writer->lastSeq || seq >= SCW_SEQ_LIMIT) {
        return NULL;
    }
    writer->lastSeq = seq;
    writer->busySeq = seq;
    uint32_t index = (uint32_t)(seq % writer->nslots);
    SCW_STORE_RELAXED_U64(&writer->header->slot[index].gen, SCW_GEN_WRITING(seq));
    __atomic_thread_fence(__ATOMIC_RELEASE);                      // the odd gen is visible before any pixel store
    return scw_writer_slot(writer, index);
}

void scw_writer_end(scw_writer *writer, uint64_t seq, uint64_t captureNs, uint64_t publishNs, uint32_t flags) {
    if (!writer->header || !seq || seq != writer->busySeq) {
        return;
    }
    writer->busySeq = 0;
    uint32_t index = (uint32_t)(seq % writer->nslots);
    scw_slot *slot = &writer->header->slot[index];
    SCW_STORE_RELAXED_U64(&slot->seq, seq);
    SCW_STORE_RELAXED_U64(&slot->captureNs, captureNs);
    SCW_STORE_RELAXED_U64(&slot->publishNs, publishNs);
    __atomic_store_n(&slot->flags, flags, __ATOMIC_RELAXED);
    SCW_STORE_RELEASE_U64(&slot->gen, SCW_GEN_COMPLETE(seq));   // release: pixels and meta before the even gen
    SCW_STORE_RELEASE_U64(&writer->header->latest, seq);
}

void scw_writer_state(scw_writer *writer, uint32_t state) {
    if (writer->header) {
        SCW_STORE_RELEASE_U32(&writer->header->state, state);
    }
}

void scw_writer_beat(scw_writer *writer, uint64_t now_ns) {
    if (writer->header) {
        SCW_STORE_RELEASE_U64(&writer->header->beatNs, now_ns);
    }
}

void scw_writer_clear(scw_writer *writer) {
    if (!writer->header) {
        return;
    }
    SCW_STORE_RELEASE_U64(&writer->header->latest, 0);
    writer->busySeq = 0;
    for (uint32_t i = 0; i < writer->nslots; i++) {
        scw_slot *slot = &writer->header->slot[i];
        // An odd gen first: a reader never accepts an odd gen, so a reader in the middle of a copy discards it.
        SCW_STORE_RELAXED_U64(&slot->gen, SCW_GEN_WRITING(writer->lastSeq + 1));
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memset(scw_writer_slot(writer, i), 0, (size_t)writer->slotBytes);
        SCW_STORE_RELAXED_U64(&slot->seq, 0);
        SCW_STORE_RELAXED_U64(&slot->captureNs, 0);
        SCW_STORE_RELAXED_U64(&slot->publishNs, 0);
        SCW_STORE_RELEASE_U64(&slot->gen, 0);
    }
}

// ---- names

void scw_cam_hash(const char *home, const char *unique_id, char out[SCW_CAM_HASH_BYTES]) {
    uint64_t hash = SCW_FNV_OFFSET;
    for (const unsigned char *c = (const unsigned char *)(home ? home : ""); *c; c++) {
        hash ^= *c;
        hash *= SCW_FNV_PRIME;
    }
    // the 0 byte between the two strings
    hash ^= 0;
    hash *= SCW_FNV_PRIME;
    for (const unsigned char *c = (const unsigned char *)(unique_id ? unique_id : ""); *c; c++) {
        hash ^= *c;
        hash *= SCW_FNV_PRIME;
    }
    snprintf(out, SCW_CAM_HASH_BYTES, "%08x", (unsigned)(hash & 0xffffffffu));
}

int scw_shm_name(char *out, size_t cap, unsigned long uid, const char *cam_hash, uint32_t version) {
    if (!out || !cap || !cam_hash || strlen(cam_hash) != SCW_CAM_HASH_LEN || !version) {
        return SCW_E_ARG;
    }
    for (int i = 0; i < SCW_CAM_HASH_LEN; i++) {
        char digit = cam_hash[i];
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return SCW_E_ARG;
        }
    }
    char name[64];
    int len = snprintf(name, sizeof name, "/scw.%lu.%s.v%u", uid, cam_hash, version);
    if (len < 0 || (size_t)len > SCW_NAME_MAX || (size_t)len >= cap) {
        return SCW_E_ARG;
    }
    memcpy(out, name, (size_t)len + 1);
    return len;
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
    char list[SCW_LIST_BYTES] = "";
    size_t used = 0;
    for (unsigned i = 0; accepted && i < naccepted && used < sizeof list - SCW_LIST_ITEM_BYTES; i++) {
        int written = snprintf(list + used, sizeof list - used, "%sv%u", i ? ", " : "", accepted[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
    return snprintf(out, cap, "webcam: update stuntcam (ring v%u, engine accepts %s)", ring_version,
                    naccepted ? list : "none");
}

uint32_t scw_pick_version(const uint32_t *first, unsigned nfirst, const uint32_t *second, unsigned nsecond) {
    uint32_t best = 0;
    for (unsigned i = 0; first && i < nfirst; i++) {
        if (first[i] > best && scw_accepts(second, nsecond, first[i])) {
            best = first[i];
        }
    }
    return best;
}

// ---- pid file

static const char *const scw_auth_names[] = {"unknown", "notDetermined", "prompting", "authorized", "denied"};

const char *scw_auth_name(int auth) {
    return auth >= 0 && auth <= SCW_AUTH_DENIED ? scw_auth_names[auth] : "unknown";
}

// Parses a decimal in [0, max]; the whole span must be digits.
static int scw_parse_u64(const char *text, size_t len, uint64_t max, uint64_t *out) {
    uint64_t value = 0;
    if (len == 0 || len > SCW_U64_DIGITS_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        if (__builtin_mul_overflow(value, 10, &value) ||
            __builtin_add_overflow(value, (uint64_t)(text[i] - '0'), &value)) {
            return 0;
        }
    }
    if (value > max) {
        return 0;
    }
    *out = value;
    return 1;
}

static int scw_span_is(const char *span, size_t len, const char *lit) {
    return strlen(lit) == len && memcmp(span, lit, len) == 0;
}

// The pid file's lines, one bit each, to find a line given twice or a required one missing.
enum {
    SCW_PIDF_HEAD = 1,
    SCW_PIDF_PID = 2,
    SCW_PIDF_SHA = 4,
    SCW_PIDF_RINGS = 8,
    SCW_PIDF_AUTH = 16,
    SCW_PIDF_STATE = 32,
    SCW_PIDF_CAMERA = 64,
    SCW_PIDF_STARTED = 128,
};

// sha=: exactly SCW_SHA_LEN lowercase hex digits.
static int scw_parse_sha(const char *value, size_t len, scw_pidfile *out) {
    if (len != SCW_SHA_LEN) {
        return 0;
    }
    for (size_t i = 0; i < SCW_SHA_LEN; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    memcpy(out->sha, value, SCW_SHA_LEN);
    out->sha[SCW_SHA_LEN] = 0;
    return 1;
}

// rings=: a comma-separated list of ring versions, each in [1, SCW_PID_RING_MAX], at most SCW_PID_MAX_RINGS.
static int scw_parse_rings(const char *value, size_t len, scw_pidfile *out) {
    size_t item_start = 0;
    uint64_t number;
    out->nrings = 0;
    while (item_start <= len) {
        size_t item_end = item_start;
        while (item_end < len && value[item_end] != ',') {
            item_end++;
        }
        if (out->nrings >= SCW_PID_MAX_RINGS) {
            return 0;
        }
        if (!scw_parse_u64(value + item_start, item_end - item_start, SCW_PID_RING_MAX, &number)) {
            return 0;
        }
        if (number == 0) {
            return 0;
        }
        out->rings[out->nrings++] = (uint32_t)number;
        item_start = item_end + 1;
    }
    return 1;
}

// auth=: one of scw_auth_names, "unknown" excepted.
static int scw_parse_auth(const char *value, size_t len, scw_pidfile *out) {
    int auth = -1;
    for (int k = SCW_AUTH_NOT_DETERMINED; k <= SCW_AUTH_DENIED; k++) {
        if (scw_span_is(value, len, scw_auth_names[k])) {
            auth = k;
        }
    }
    if (auth < 0) {
        return 0;
    }
    out->auth = auth;
    return 1;
}

// state=: one of the names scw_state_name gives.
static int scw_parse_state(const char *value, size_t len, scw_pidfile *out) {
    uint32_t state = SCW_STATE_COUNT;
    for (uint32_t k = 0; k < SCW_STATE_COUNT; k++) {
        if (scw_span_is(value, len, scw_state_name(k))) {
            state = k;
        }
    }
    if (state == SCW_STATE_COUNT) {
        return 0;
    }
    out->state = state;
    return 1;
}

// camera=: any text without control characters that fits out->camera with its NUL.
static int scw_parse_camera(const char *value, size_t len, scw_pidfile *out) {
    if (len >= sizeof out->camera) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)value[i] < 0x20 || value[i] == 0x7f) {
            return 0;   // a control character
        }
    }
    memcpy(out->camera, value, len);
    out->camera[len] = 0;
    return 1;
}

// One "key=value" line after the first, into *out and *seen. Returns 0 for a malformed line: a known key given twice,
// or a value out of range. Unknown keys are ignored (a newer helper may add fields).
static int scw_pidfile_field(const char *key, size_t key_len, const char *value, size_t value_len, scw_pidfile *out,
                             unsigned *seen) {
    uint64_t number;

    if (scw_span_is(key, key_len, "pid")) {
        if (*seen & SCW_PIDF_PID) {
            return 0;
        }
        if (!scw_parse_u64(value, value_len, INT32_MAX, &number) || number == 0) {
            return 0;
        }
        out->pid = (int32_t)number;
        *seen |= SCW_PIDF_PID;
    } else if (scw_span_is(key, key_len, "sha")) {
        if (*seen & SCW_PIDF_SHA) {
            return 0;
        }
        if (!scw_parse_sha(value, value_len, out)) {
            return 0;
        }
        *seen |= SCW_PIDF_SHA;
    } else if (scw_span_is(key, key_len, "rings")) {
        if (*seen & SCW_PIDF_RINGS) {
            return 0;
        }
        if (!scw_parse_rings(value, value_len, out)) {
            return 0;
        }
        *seen |= SCW_PIDF_RINGS;
    } else if (scw_span_is(key, key_len, "auth")) {
        if (*seen & SCW_PIDF_AUTH) {
            return 0;
        }
        if (!scw_parse_auth(value, value_len, out)) {
            return 0;
        }
        *seen |= SCW_PIDF_AUTH;
    } else if (scw_span_is(key, key_len, "state")) {
        if (*seen & SCW_PIDF_STATE) {
            return 0;
        }
        if (!scw_parse_state(value, value_len, out)) {
            return 0;
        }
        *seen |= SCW_PIDF_STATE;
    } else if (scw_span_is(key, key_len, "camera")) {
        if (*seen & SCW_PIDF_CAMERA) {
            return 0;
        }
        *seen |= SCW_PIDF_CAMERA;
        if (!scw_parse_camera(value, value_len, out)) {
            return 0;
        }
    } else if (scw_span_is(key, key_len, "started")) {
        if (*seen & SCW_PIDF_STARTED) {
            return 0;
        }
        *seen |= SCW_PIDF_STARTED;
        if (!scw_parse_u64(value, value_len, UINT64_MAX, &number)) {
            return 0;
        }
        out->started = number;
    }

    return 1;
}

int scw_pidfile_parse(const char *text, size_t len, scw_pidfile *out) {
    if (!text || !out) {
        return SCW_E_ARG;
    }
    memset(out, 0, sizeof *out);
    out->state = SCW_STATE_COUNT;
    unsigned seen = 0;
    size_t pos = 0;
    int line_no = 0;

    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            end++;
        }
        const char *line = text + pos;
        size_t line_len = end - pos;
        pos = end + 1;
        line_no++;
        if (line_no == 1) {
            if (!scw_span_is(line, line_len, "stuntcam-webcam 1")) {
                return SCW_E_ARG;
            }
            seen |= SCW_PIDF_HEAD;
            continue;
        }
        if (line_len == 0) {
            continue;
        }
        const char *eq = memchr(line, '=', line_len);
        if (!eq) {
            return SCW_E_ARG;
        }
        size_t key_len = (size_t)(eq - line);
        if (!scw_pidfile_field(line, key_len, eq + 1, line_len - key_len - 1, out, &seen)) {
            return SCW_E_ARG;
        }
    }

    unsigned need = SCW_PIDF_HEAD | SCW_PIDF_PID | SCW_PIDF_SHA | SCW_PIDF_RINGS | SCW_PIDF_AUTH | SCW_PIDF_STATE;
    return (seen & need) == need ? SCW_OK : SCW_E_ARG;
}

int scw_pidfile_format(char *out, size_t cap, const scw_pidfile *pidfile) {
    if (!out || !pidfile) {
        return SCW_E_ARG;
    }
    if (pidfile->pid <= 0 || strlen(pidfile->sha) != SCW_SHA_LEN) {
        return SCW_E_ARG;
    }
    if (!pidfile->nrings || pidfile->nrings > SCW_PID_MAX_RINGS) {
        return SCW_E_ARG;
    }
    if (pidfile->auth < SCW_AUTH_NOT_DETERMINED || pidfile->auth > SCW_AUTH_DENIED) {
        return SCW_E_ARG;
    }
    if (pidfile->state >= SCW_STATE_COUNT) {
        return SCW_E_ARG;
    }
    char rings[SCW_LIST_BYTES] = "";
    size_t used = 0;

    for (unsigned i = 0; i < pidfile->nrings; i++) {
        if (!pidfile->rings[i] || pidfile->rings[i] > SCW_PID_RING_MAX) {
            return SCW_E_ARG;
        }
        used += (size_t)snprintf(rings + used, sizeof rings - used, "%s%u", i ? "," : "", pidfile->rings[i]);
    }

    for (const char *c = pidfile->camera; *c; c++) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) {
            return SCW_E_ARG;   // a control character
        }
    }

    int len = snprintf(out, cap,
                       "stuntcam-webcam 1\npid=%d\nsha=%s\nrings=%s\nauth=%s\nstate=%s\ncamera=%s\nstarted=%llu\n",
                       pidfile->pid, pidfile->sha, rings, scw_auth_name(pidfile->auth),
                       scw_state_name(pidfile->state), pidfile->camera, (unsigned long long)pidfile->started);
    if (len < 0 || (size_t)len >= cap) {
        return SCW_E_ARG;
    }
    return len;
}

#undef SCW_LOAD_ACQUIRE_U32
#undef SCW_LOAD_ACQUIRE_U64
#undef SCW_LOAD_RELAXED_U64
#undef SCW_LOAD_RELAXED_U32
#undef SCW_STORE_RELEASE_U32
#undef SCW_STORE_RELEASE_U64
#undef SCW_STORE_RELAXED_U64
#undef SCW_GEN_WRITING
#undef SCW_GEN_COMPLETE
#undef SCW_SEQ_LIMIT
#undef SCW_BYTES_PER_PIXEL
#undef SCW_ROW_ALIGN_MASK
#undef SCW_FNV_OFFSET
#undef SCW_FNV_PRIME
#undef SCW_PID_RING_MAX
#undef SCW_U64_DIGITS_MAX
#undef SCW_LIST_BYTES
#undef SCW_LIST_ITEM_BYTES
