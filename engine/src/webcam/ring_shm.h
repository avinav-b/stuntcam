// SPDX-License-Identifier: Apache-2.0
// ring_shm.h: the POSIX shm object behind a webcam ring (docs/webcam-ring.md, "Shared memory").
// Helper side: create a fresh object owned by this user with mode 0600, publish, retire (zero the slots, then unlink).
// Reader side: open read-only, refuse any object not owned by this user or with group/other mode bits, map, validate,
// and re-open the name every second while offline.
#ifndef SCW_RING_SHM_H
#define SCW_RING_SHM_H
#include "ring.h"
#include <sys/stat.h>
#include <sys/types.h>

// A system error as a result code: -1000 - errno.
#define SCW_E_SYS(e) (-1000 - (e))
#define SCW_READER_DEAD 4u           // retired epochs a reader remembers and never maps again
#define SCW_WHY_BYTES 192            // a reader's offline reason
#define SCW_REPORT_MSG_BYTES 160     // why creation was refused
#define SCW_READER_ACCEPTED_MAX 4    // the ring versions one reader can accept

// What creation saw; filled on success and on failure.
typedef struct {
    int unlinkErrno;           // errno of the initial shm_unlink (0: removed an old object; ENOENT: none)
    int fchmodErrno;           // errno of fchmod(fd, 0600) on the new object (0: it succeeded)
    unsigned mode;             // st_mode & 07777 of the new object, after fchmod
    long owner;                // st_uid of the object found at the name when refused (-1: unknown)
    char msg[SCW_REPORT_MSG_BYTES];   // why creation was refused, for the CLI and doctor
} scw_shm_report;

typedef struct {
    char name[SCW_NAME_MAX + 1];
    int fd;
    scw_header *header;        // read-write mapping of the whole object
    uint64_t len;
    scw_writer ring;           // the writer's private geometry and epoch: scw_writer_init(&ring, header, len, ...)
} scw_shm_writer;

typedef struct {
    char name[SCW_NAME_MAX + 1];
    uint32_t accepted[SCW_READER_ACCEPTED_MAX];   // the ring versions this reader understands
    unsigned naccepted;
    const void *map;           // read-only mapping, or NULL
    uint64_t len;
    scw_geom geom;             // private copy of the validated header
    uint64_t nextOpenNs;       // next re-open attempt while offline
    int status;                // last open result (SCW_OK or a negative SCW_E_* / -1000-errno for system errors)
    long owner;                // owner of a refused object, or -1
    char why[SCW_WHY_BYTES];   // why the reader is offline, for the engine log and the offline band
    unsigned opens;            // successful opens (new mappings) so far
    uint64_t dead[SCW_READER_DEAD];  // epochs seen `superseded` or `stopped`
    unsigned ndead;
    uint32_t maxWidth;         // set after init to refuse larger rings (either orientation) as SCW_E_LIMIT; 0: no limit
    uint32_t maxHeight;        // likewise
} scw_shm_reader;

// What one open attempt found (scw_shm_open_ro).
typedef struct {
    const void *map;           // on SCW_OK, a new read-only mapping of len bytes; else NULL
    uint64_t len;
    scw_geom geom;             // on SCW_OK, the private copy of the validated header
    long owner;                // the owner of a refused object, or -1
    char why[SCW_WHY_BYTES];   // why the attempt failed
} scw_shm_open_result;

// 1 if `info` is owned by `uid` and has no group/other permission bits; else 0 and a reason in msg.
int scw_shm_check_stat(const struct stat *info, uid_t uid, const char *name, char *msg, size_t cap);

// Helper: shm_unlink(name), shm_open(O_CREAT|O_EXCL|O_RDWR, 0600), fchmod 0600, fstat owner/mode check, ftruncate,
// mmap. SCW_OK, or a negative code with report->msg ("webcam: shm name /scw.… owned by uid N", …). Never publishes:
// the caller then runs scw_writer_init(&writer->ring, writer->header, writer->len, ...) and scw_writer_publish.
int scw_shm_create(scw_shm_writer *writer, const char *name, uint64_t bytes, scw_shm_report *report);
// Helper: state = final_state (STOPPED or SUPERSEDED), zero the slots, unlink the name if it still names this object
// (same epoch: fstat gives shm objects no inode on macOS), unmap and close. Uses only the private writer->ring.
// To change geometry, create and publish the new object first (scw_shm_create unlinks the old name, the old mapping
// stays valid), then retire the old one with SCW_STATE_SUPERSEDED: readers that see it re-open at once and find the
// new object already there.
void scw_shm_retire(scw_shm_writer *writer, uint32_t final_state);

// Reader. init copies the name and accepted versions; poll does everything else and returns 1 while live.
void scw_shm_reader_init(scw_shm_reader *reader, const char *name, const uint32_t *accepted, unsigned naccepted);
int scw_shm_reader_poll(scw_shm_reader *reader, uint64_t now_ns, scw_peek_t *peek);
void scw_shm_reader_close(scw_shm_reader *reader);
// One open attempt (poll calls this): on SCW_OK result->map, ->len and ->geom describe a new read-only mapping; on
// failure result->why says why and result->owner names a refused object's owner.
int scw_shm_open_ro(const char *name, const uint32_t *accepted, unsigned naccepted, scw_shm_open_result *result);

#endif
