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
#define SCW_READER_DEAD 4u      // retired epochs a reader remembers and never maps again

// What creation saw; filled on success and on failure.
typedef struct {
    int unlink_errno;          // errno of the initial shm_unlink (0: removed an old object; ENOENT: none)
    int fchmod_errno;          // errno of fchmod(fd, 0600) on the new object (0: it succeeded)
    unsigned mode;             // st_mode & 07777 of the new object, after fchmod
    long owner;                // st_uid of the object found at the name when refused (-1: unknown)
    char msg[160];             // why creation was refused, for the CLI and doctor
} scw_shm_report;

typedef struct {
    char name[SCW_NAME_MAX + 1];
    int fd;
    scw_header *h;             // read-write mapping of the whole object
    uint64_t len;
    scw_writer ring;           // the writer's private geometry and epoch: scw_writer_init(&w->ring, w->h, w->len, ...)
} scw_shm_writer;

typedef struct {
    char name[SCW_NAME_MAX + 1];
    uint32_t accepted[4];
    unsigned naccepted;
    const void *map;           // read-only mapping, or NULL
    uint64_t len;
    scw_geom g;                // private copy of the validated header
    uint64_t next_open_ns;     // next re-open attempt while offline
    int status;                // last open result (SCW_OK or a negative SCW_E_* / -1000-errno for system errors)
    long owner;                // owner of a refused object, or -1
    char why[192];             // why the reader is offline, for the engine log and the offline band
    unsigned opens;            // successful opens (new mappings) so far
    uint64_t dead[SCW_READER_DEAD];  // epochs seen `superseded` or `stopped`
    unsigned ndead;
    uint32_t max_w, max_h;     // set after init to refuse larger rings (either orientation) as SCW_E_LIMIT; 0: no limit
} scw_shm_reader;

// 1 if `st` is owned by `uid` and has no group/other permission bits; else 0 and a reason in msg.
int scw_shm_check_stat(const struct stat *st, uid_t uid, const char *name, char *msg, size_t cap);

// Helper: shm_unlink(name), shm_open(O_CREAT|O_EXCL|O_RDWR, 0600), fchmod 0600, fstat owner/mode check, ftruncate,
// mmap. SCW_OK, or a negative code with rep->msg ("webcam: shm name /scw.… owned by uid N", …). Never publishes:
// the caller then runs scw_writer_init(&w->ring, w->h, w->len, ...) and scw_writer_publish.
int scw_shm_create(scw_shm_writer *w, const char *name, uint64_t bytes, scw_shm_report *rep);
// Helper: state = final_state (STOPPED or SUPERSEDED), zero the slots, unlink the name if it still names this object
// (same epoch: fstat gives shm objects no inode on macOS), unmap and close. Uses only the private w->ring.
// To change geometry, create and publish the new object first (scw_shm_create unlinks the old name, the old mapping
// stays valid), then retire the old one with SCW_STATE_SUPERSEDED: readers that see it re-open at once and find the
// new object already there.
void scw_shm_retire(scw_shm_writer *w, uint32_t final_state);

// Reader. init copies the name and accepted versions; poll does everything else and returns 1 while live.
void scw_shm_reader_init(scw_shm_reader *r, const char *name, const uint32_t *accepted, unsigned naccepted);
int scw_shm_reader_poll(scw_shm_reader *r, uint64_t now_ns, scw_peek_t *peek);
void scw_shm_reader_close(scw_shm_reader *r);
// One open attempt (poll calls this): on SCW_OK *map/*len/*g describe a new read-only mapping.
int scw_shm_open_ro(const char *name, const uint32_t *accepted, unsigned naccepted, const void **map, uint64_t *len,
                    scw_geom *g, long *owner, char *why, size_t cap);

#endif
