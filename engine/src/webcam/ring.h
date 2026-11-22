// SPDX-License-Identifier: Apache-2.0
// ring.h: the webcam frame ring, version 1 (docs/webcam-ring.md is the specification).
// Shared by the camera helper (the only writer) and the engine's `webcam` source (any number of read-only readers).
// Plain C11, no Objective-C and no Apple-only API, so it builds and fuzzes on any POSIX host.
//
// Layout of one ring object (a POSIX shm object, or a file in the fallback), mapped MAP_SHARED:
//   [0, 4096)                          header: identity, geometry, state, `latest`, and the slot table
//   [4096 + i*slotBytes, +slotBytes)   slot i: plane 0 (BGRA rows of bpr0 bytes)
// Readers validate the header once per mapping (scw_validate), keep the geometry in private memory, and per tick read
// only state, epoch, latest and one slot's seqlock and timestamps (scw_peek, scw_slot_begin/scw_slot_end). The writer
// (scw_writer) keeps its own private geometry and never reads it back.
#ifndef SCW_RING_H
#define SCW_RING_H
#include <stddef.h>
#include <stdint.h>

#define SCW_MAGIC 0x52574353u          // "SCWR" in memory on a little-endian host
#define SCW_VERSION 1u                 // the version this header describes
#define SCW_HDR 4096u                  // header bytes; slot 0 starts here
#define SCW_MAX_SLOTS 8u
#define SCW_MAX_DIM 8192u
#define SCW_SLOT_ALIGN 4096u           // slotBytes is a multiple of this
#define SCW_MAX_OBJECT (1ull << 30)    // a ring object is at most 1 GiB
#define SCW_FORMAT_BGRA 0x42475241u    // 'BGRA', CoreVideo kCVPixelFormatType_32BGRA
#define SCW_DEVICE_LEN 64u
#define SCW_NAME_MAX 31u               // macOS PSHMNAMLEN: shm names longer than this fail with ENAMETOOLONG
#define SCW_LIVE_NS 500000000ull       // a ring is live while its newest frame is younger than this
#define SCW_REOPEN_NS 1000000000ull    // an offline reader re-opens the shm name this often
#define SCW_CAM_HASH_LEN 8             // hex digits of a camera hash (scw_cam_hash)
#define SCW_CAM_HASH_BYTES (SCW_CAM_HASH_LEN + 1)   // a camera hash and its NUL
#define SCW_SHA_LEN 12                 // hex digits of the helper's sha in the pid file
#define SCW_PID_CAMERA_BYTES 128       // the pid file's camera= value (a uniqueID) and its NUL

// header.state
enum {
    SCW_STATE_INIT = 0,        // being created; readers wait (the magic is written last)
    SCW_STATE_STARTING = 1,    // helper running, camera not yet delivering
    SCW_STATE_PROMPTING = 2,   // waiting for the user to answer the camera permission prompt
    SCW_STATE_RUNNING = 3,     // frames are being published
    SCW_STATE_IDLE = 4,        // camera closed (no active reader); slots cleared
    SCW_STATE_DENIED = 5,      // camera permission denied or restricted; the helper exits
    SCW_STATE_STOPPED = 6,     // helper stopped cleanly; slots cleared; the name is about to be unlinked
    SCW_STATE_SUPERSEDED = 7,  // replaced by a new object at the same name (new geometry); re-open now
    SCW_STATE_COUNT
};

// slot meta flags
#define SCW_SLOT_SYNTHETIC 1u

// One slot's metadata: 64 bytes, so each slot's seqlock has its own cache line.
typedef struct {
    uint64_t gen;              // seqlock: 2*seq+1 while slot is being written, 2*seq+2 once complete, 0 = never written
    uint64_t seq;              // frame number written to this slot (from 1)
    uint64_t captureNs;        // host clock ns of the camera sample's presentation time
    uint64_t publishNs;        // host clock ns when the slot was complete
    uint32_t flags;            // SCW_SLOT_*
    uint32_t reserved0;
    uint64_t reserved[3];
} scw_slot;

// The header. Fixed offsets (checked below); every version keeps magic at 0 and version at 4.
typedef struct {
    uint32_t magic;            //   0  SCW_MAGIC, stored last at creation (release)
    uint32_t version;          //   4  ring version (1)
    uint32_t headerBytes;      //   8  SCW_HDR
    uint32_t state;            //  12  SCW_STATE_* (atomic)
    uint64_t epoch;            //  16  nonzero, unique per object; never changes for the life of the object
    uint32_t width;            //  24  pixels
    uint32_t height;           //  28
    uint32_t format;           //  32  SCW_FORMAT_BGRA
    uint32_t nslots;           //  36  1..SCW_MAX_SLOTS
    uint32_t bpr0;             //  40  bytes per row of plane 0
    uint32_t bpr1;             //  44  bytes per row of plane 1 (0: single-plane formats)
    uint64_t plane1Off;        //  48  offset of plane 1 inside a slot (0: single-plane formats)
    uint64_t slotBytes;        //  56  bytes per slot, a multiple of SCW_SLOT_ALIGN
    uint64_t totalBytes;       //  64  SCW_HDR + slotBytes*nslots
    uint32_t fpsMilli;         //  72  nominal frame rate * 1000 (informational)
    int32_t  producerPid;      //  76  helper pid (informational)
    uint32_t flags;            //  80  reserved, 0
    uint32_t reserved0;        //  84
    uint64_t latest;           //  88  seq of the newest complete frame, 0 = none (atomic)
    uint64_t beatNs;           //  96  host clock ns of the helper's last heartbeat (informational; not liveness)
    uint64_t reserved1[3];     // 104
    char     device[SCW_DEVICE_LEN]; // 128  camera name, UTF-8, NUL-terminated (informational)
    uint8_t  reserved2[64];    // 192
    scw_slot slot[SCW_MAX_SLOTS];    // 256 .. 768
} scw_header;

// The layout is the ring's wire format, shared with every helper and engine build: each field's offset is checked
// here, so a change to these structs that moves one fails to compile.
#define SCW_FIELD_AT(type, field, offset) _Static_assert(offsetof(type, field) == (offset), #type "." #field)
SCW_FIELD_AT(scw_slot, gen, 0);
SCW_FIELD_AT(scw_slot, seq, 8);
SCW_FIELD_AT(scw_slot, captureNs, 16);
SCW_FIELD_AT(scw_slot, publishNs, 24);
SCW_FIELD_AT(scw_slot, flags, 32);
SCW_FIELD_AT(scw_slot, reserved0, 36);
SCW_FIELD_AT(scw_slot, reserved, 40);
_Static_assert(sizeof(scw_slot) == 64, "slot meta is one cache line");
SCW_FIELD_AT(scw_header, magic, 0);
SCW_FIELD_AT(scw_header, version, 4);
SCW_FIELD_AT(scw_header, headerBytes, 8);
SCW_FIELD_AT(scw_header, state, 12);
SCW_FIELD_AT(scw_header, epoch, 16);
SCW_FIELD_AT(scw_header, width, 24);
SCW_FIELD_AT(scw_header, height, 28);
SCW_FIELD_AT(scw_header, format, 32);
SCW_FIELD_AT(scw_header, nslots, 36);
SCW_FIELD_AT(scw_header, bpr0, 40);
SCW_FIELD_AT(scw_header, bpr1, 44);
SCW_FIELD_AT(scw_header, plane1Off, 48);
SCW_FIELD_AT(scw_header, slotBytes, 56);
SCW_FIELD_AT(scw_header, totalBytes, 64);
SCW_FIELD_AT(scw_header, fpsMilli, 72);
SCW_FIELD_AT(scw_header, producerPid, 76);
SCW_FIELD_AT(scw_header, flags, 80);
SCW_FIELD_AT(scw_header, reserved0, 84);
SCW_FIELD_AT(scw_header, latest, 88);
SCW_FIELD_AT(scw_header, beatNs, 96);
SCW_FIELD_AT(scw_header, reserved1, 104);
SCW_FIELD_AT(scw_header, device, 128);
SCW_FIELD_AT(scw_header, reserved2, 192);
SCW_FIELD_AT(scw_header, slot, 256);
#undef SCW_FIELD_AT
_Static_assert(sizeof(scw_header) == 768, "header layout");
_Static_assert(sizeof(scw_header) <= SCW_HDR, "header fits its page");

// Results. Negative values are failures; scw_strerror names each one.
enum {
    SCW_OK = 0,
    SCW_NONE = 1,              // no new complete frame (latest is 0 or unchanged)
    SCW_E_SHORT = -1,          // mapping shorter than the header
    SCW_E_NOTREADY = -2,       // magic not yet stored (object being created)
    SCW_E_MAGIC = -3,          // not a ring
    SCW_E_VERSION = -4,        // a ring version this reader doesn't accept
    SCW_E_GEOMETRY = -5,       // header fields out of range or inconsistent
    SCW_E_FORMAT = -6,         // pixel format other than BGRA
    SCW_E_SIZE = -7,           // object smaller than the geometry needs, or larger than SCW_MAX_OBJECT
    SCW_E_STATE = -8,          // unknown state value
    SCW_E_EPOCH = -9,          // magic, version or epoch changed under the mapping: re-open
    SCW_E_TORN = -10,          // the slot was overwritten while it was read: drop that copy
    SCW_E_ARG = -11,           // caller error (destination too small, bad name input, ...)
    SCW_E_LIMIT = -12,         // a valid ring larger than this reader accepts (scw_shm_reader.maxWidth/maxHeight)
};

// Reader-private copy of a validated header. Only this is used to locate pixels.
typedef struct {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t nslots;
    uint32_t bpr0;
    uint64_t epoch;
    uint64_t slotBytes;
    uint64_t totalBytes;
    uint64_t mapBytes;         // length of the reader's mapping
    uint32_t fpsMilli;
    int32_t producerPid;
    char device[SCW_DEVICE_LEN];
} scw_geom;

// What one tick sees: the header's live fields and the newest slot's timestamps.
typedef struct {
    uint32_t state;
    uint64_t latest;           // 0: no frame yet
    uint64_t captureNs;        // of `latest`, valid only when hasTimestamps is 1 (the seqlock check passed)
    uint64_t publishNs;        // likewise
    int hasTimestamps;         // 1: captureNs and publishNs are of `latest`
} scw_peek_t;

// A slot read in progress (scw_slot_begin .. scw_slot_end).
typedef struct {
    uint64_t seq;
    uint64_t gen;              // the slot's even gen when the read began; scw_slot_end checks it is unchanged
    uint64_t captureNs;
    uint64_t publishNs;
    uint32_t flags;
    uint32_t index;            // the slot read
    const uint8_t *pixels;     // plane 0 of the slot, bpr0 * height bytes, inside the mapping
} scw_slot_read;

// ---- reader
// Validates a mapping of `map_len` bytes (the object's st_size, mapped whole). On SCW_OK fills *geom. On
// SCW_E_VERSION, *found_version is the header's version. `accepted` lists the ring versions this reader understands.
int scw_validate(const void *map, uint64_t map_len, const uint32_t *accepted, unsigned naccepted, scw_geom *geom,
                 uint32_t *found_version);
// The live fields for one tick. Returns SCW_OK, or SCW_E_EPOCH if the object's identity changed (re-open).
int scw_peek(const void *map, const scw_geom *geom, scw_peek_t *out);
// Starts reading the newest frame if its seq differs from `last_seq`. SCW_OK: slot_read->pixels may be copied, then
// scw_slot_end decides whether the copy is good. SCW_NONE: nothing new. SCW_E_TORN: the slot is being rewritten.
int scw_slot_begin(const void *map, const scw_geom *geom, uint64_t last_seq, scw_slot_read *slot_read);
// SCW_OK if the slot still holds the frame scw_slot_begin saw (the copy is consistent), else SCW_E_TORN or SCW_E_EPOCH.
int scw_slot_end(const void *map, const scw_geom *geom, const scw_slot_read *slot_read);
// begin + row copy into dst (dst_bpr >= 4*width, dst_len >= dst_bpr*height) + end.
int scw_read_latest(const void *map, const scw_geom *geom, uint64_t last_seq, void *dst, size_t dst_bpr,
                    size_t dst_len, scw_slot_read *slot_read);
// Liveness: a peek with a fresh frame and state RUNNING.
int scw_is_live(const scw_peek_t *peek, uint64_t now_ns);

// ---- writer (the helper)
// Fills a header in memory the caller owns (also used to build test headers). Returns SCW_OK or SCW_E_GEOMETRY /
// SCW_E_ARG; never stores the magic.
int scw_header_init(scw_header *header, uint32_t width, uint32_t height, uint32_t bpr0, uint32_t nslots,
                    uint64_t epoch, uint32_t fpsMilli, int32_t pid, const char *device);
// Bytes for an object of this geometry (SCW_HDR + slotBytes*nslots), or 0 if out of range. *slot_bytes gets slotBytes.
uint64_t scw_object_bytes(uint32_t height, uint32_t bpr0, uint32_t nslots, uint64_t *slot_bytes);

// The writer's private view of its ring. Any process of the same user can open the 0600 object read-write and
// rewrite the header, so the writer never reads geometry, epoch or sequence numbers back from shared memory: every
// address and index comes from this struct, and the shared header is only ever written.
typedef struct {
    scw_header *header;        // the shared mapping (write-only from the writer's point of view)
    uint64_t mapBytes;         // length of the mapping
    uint32_t width;
    uint32_t height;
    uint32_t bpr0;
    uint32_t nslots;
    uint64_t slotBytes;
    uint64_t totalBytes;
    uint64_t epoch;
    uint64_t lastSeq;          // newest seq begun; seq only ever increases within an epoch
    uint64_t busySeq;          // seq between scw_writer_begin and scw_writer_end, 0 if none
} scw_writer;
// Fills the shared header (state STARTING, no magic) and the private copy. SCW_E_SIZE if map_len is too short.
int scw_writer_init(scw_writer *writer, void *map, uint64_t map_len, uint32_t width, uint32_t height, uint32_t bpr0,
                    uint32_t nslots, uint64_t epoch, uint32_t fpsMilli, int32_t pid, const char *device);
void scw_writer_publish(scw_writer *writer);               // stores the magic: the last step of creation (release)
// Marks slot seq%nslots busy and returns its pixels (bpr0*height bytes), or NULL unless seq > every earlier seq.
uint8_t *scw_writer_begin(scw_writer *writer, uint64_t seq);
// Completes the frame begun with the same seq (ignored otherwise) and makes it `latest`.
void scw_writer_end(scw_writer *writer, uint64_t seq, uint64_t captureNs, uint64_t publishNs, uint32_t flags);
void scw_writer_state(scw_writer *writer, uint32_t state);
void scw_writer_beat(scw_writer *writer, uint64_t now_ns);
// latest = 0; each slot marked busy, zeroed and reset to gen 0 (camera closed, stopped, superseded).
void scw_writer_clear(scw_writer *writer);

// ---- names
// FNV-1a 64 over home, one 0 byte, then unique_id; the low 32 bits as SCW_CAM_HASH_LEN lowercase hex digits and a NUL.
void scw_cam_hash(const char *home, const char *unique_id, char out[SCW_CAM_HASH_BYTES]);
// "/scw.<uid>.<camhash>.v<version>". Returns its length, or SCW_E_ARG if it would exceed SCW_NAME_MAX or `cap`.
int scw_shm_name(char *out, size_t cap, unsigned long uid, const char *cam_hash, uint32_t version);

// ---- messages
const char *scw_strerror(int rc);
const char *scw_state_name(uint32_t state);
// "webcam: update stuntcam (ring v2, engine accepts v1)". Returns the snprintf length.
int scw_version_message(char *out, size_t cap, uint32_t ring_version, const uint32_t *accepted, unsigned naccepted);

// ---- pid file ($SIMCAM_HOME/webcam/<camhash>.pid), written by the helper, read by the CLI and engines
enum { SCW_AUTH_UNKNOWN = 0, SCW_AUTH_NOT_DETERMINED, SCW_AUTH_PROMPTING, SCW_AUTH_AUTHORIZED, SCW_AUTH_DENIED };
#define SCW_PID_MAX_RINGS 8u
typedef struct {
    int32_t pid;
    char sha[SCW_SHA_LEN + 1];     // the helper's <sha> (lowercase hex)
    uint32_t rings[SCW_PID_MAX_RINGS];
    unsigned nrings;
    int auth;                  // SCW_AUTH_*
    uint32_t state;            // SCW_STATE_*
    char camera[SCW_PID_CAMERA_BYTES];   // the camera's uniqueID
    uint64_t started;          // unix seconds
} scw_pidfile;
// Parses `len` bytes (need not be NUL-terminated). SCW_OK or SCW_E_ARG (missing or malformed fields).
int scw_pidfile_parse(const char *text, size_t len, scw_pidfile *out);
// Formats into out; returns the length, or SCW_E_ARG if it doesn't fit or a field is invalid.
int scw_pidfile_format(char *out, size_t cap, const scw_pidfile *pidfile);
const char *scw_auth_name(int auth);
// Highest version present in both lists, or 0.
uint32_t scw_pick_version(const uint32_t *first, unsigned nfirst, const uint32_t *second, unsigned nsecond);

#endif
