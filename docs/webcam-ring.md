# Webcam ring, version 1

The contract between the camera helper (`Stuntcam Camera.app`, the only writer) and the engine's `webcam` source (any
number of read-only readers, one per app process). They are separately signed binaries that may come from different
stuntcam releases, so this document, not either program, defines the format. The reference implementation is
`engine/src/webcam/ring.h`, `ring.c` (layout, validation, seqlock, names, pid file) and `ring_shm.c` (the shm object).
Tests: `engine/src/webcam/test.sh`.

All integers are little-endian (every Mac and every simulator process stuntcam supports). "Host clock ns" is
`mach_absolute_time()` converted with `mach_timebase_info` (the same as `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`).
Simulator processes share it with the Mac.

## 1. The object

One ring is one POSIX shared-memory object holding a 4096-byte header and `nslots` slots:

```
[0, 4096)                              header (§3)
[4096 + i*slotBytes, +slotBytes)       slot i, i < nslots: plane 0, `height` rows of `bpr0` bytes (BGRA)
```

**Name.** `/scw.<uid>.<cam>.v<version>`, for example `/scw.501.1a2b3c4d.v1`.
- `<uid>`: the helper's `getuid()` in decimal. Users never share a name.
- `<cam>`: 8 lowercase hex digits, the low 32 bits of FNV-1a 64 over the bytes of `$SIMCAM_HOME` (absolute, as the CLI
  resolves it), one `0x00` byte, then the camera's `AVCaptureDevice.uniqueID` in UTF-8. Offset basis
  `0xcbf29ce484222325`, prime `0x100000001b3`. Test vectors: (`""`, `""`) → `8601b7df`; (`/h`, `cam`) → `fc04bcb5`.
- `<version>`: the ring version in decimal. A helper serving two versions keeps one object per version.
- macOS limits shm names to 31 bytes (`ENAMETOOLONG` beyond); the longest possible name, uid 4294967295 and version
  65535, is exactly 31.

**Fallback.** With `SIMCAM_WEBCAM_RING=file` the same bytes live in a regular file under `$SIMCAM_HOME/webcam/`,
opened with `O_NOFOLLOW` and held to the same owner and mode checks (§7). It is for a runtime that blocks shm, and is
used only when shm is unavailable, because a file lacks one guarantee shm gives on macOS: **a shm object can't be
resized once sized, but a file can be truncated**, by the helper or by any process of the same user. A reader touching
a page past the new end of a mapped file gets SIGBUS, and readers install no SIGBUS handler (one in an app process
would interfere with the app's own). So:
- the helper never truncates or shrinks a file that may be mapped. Each ring is a new file (`O_CREAT|O_EXCL`, 0600,
  written to its full size) renamed over the name; a new geometry is a new file and a rename, exactly as a new shm
  object replaces the old name;
- readers validate against `fstat`'s size at open and map that size, as for shm; a same-user process that truncates the
  file anyway can crash the reader, which is within the same-user trust boundary (§10) but is why shm is the default.

Everything else in this document applies unchanged.

## 2. Versions

- Every version keeps `magic` at offset 0 and `version` at offset 4, so any reader can tell "not a ring" from "a ring I
  don't understand" before it reads anything else.
- A change to the layout, to a field's meaning, or to the seqlock protocol is a new version. Fields marked reserved
  are written as zero; readers ignore them, except `flags`, which must be 0 in version 1.
- A reader declares the versions it accepts (`rings=` in its reader file, §9). The helper serves the newest version it
  shares with each reader, and keeps serving older versions for older armed engines, each from the same captured frame.
- A reader that finds a version it doesn't accept, or a helper whose pid file (§8) shares no version with it, shows
  the offline band and logs: `webcam: update stuntcam (ring v2, engine accepts v1)`. It never guesses at a layout.

## 3. Header (version 1)

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x52574353` ("SCWR" in memory). Stored last at creation, with release ordering. 0 = still being created. |
| 4 | 4 | `version` | 1 |
| 8 | 4 | `headerBytes` | 4096 |
| 12 | 4 | `state` | §5. Atomic. |
| 16 | 8 | `epoch` | Nonzero, unique per object (the helper uses 64 random bits). Never changes for the object's life. |
| 24 | 4 | `width` | Pixels, 1–8192 |
| 28 | 4 | `height` | Pixels, 1–8192 |
| 32 | 4 | `format` | `0x42475241` (`'BGRA'`, `kCVPixelFormatType_32BGRA`). The only format in version 1. |
| 36 | 4 | `nslots` | 1–8 (the helper uses 4) |
| 40 | 4 | `bpr0` | Bytes per row: ≥ 4·`width`, a multiple of 4 |
| 44 | 4 | `bpr1` | 0 (no second plane in version 1) |
| 48 | 8 | `plane1Off` | 0 (no second plane in version 1) |
| 56 | 8 | `slotBytes` | A multiple of 4096, ≥ `bpr0`·`height` |
| 64 | 8 | `totalBytes` | 4096 + `slotBytes`·`nslots`, at most 1 GiB |
| 72 | 4 | `fpsMilli` | Nominal frame rate × 1000 (informational) |
| 76 | 4 | `producerPid` | The helper's pid (informational) |
| 80 | 4 | `flags` | 0 |
| 84 | 4 | — | reserved |
| 88 | 8 | `latest` | `seq` of the newest complete frame; 0 = none. Atomic. |
| 96 | 8 | `beatNs` | Host clock ns of the helper's last heartbeat. Informational: liveness comes from frames (§6). |
| 104 | 24 | — | reserved |
| 128 | 64 | `device` | The camera's localized name, UTF-8, NUL-terminated (informational). Readers replace each byte that isn't part of well-formed UTF-8, and each control character, with `?`. |
| 192 | 64 | — | reserved |
| 256 | 8 × 64 | `slot[8]` | One 64-byte record per slot (below); records at index ≥ `nslots` are unused |
| 768 | 3328 | — | reserved, to 4096 |

Slot record (64 bytes, one cache line each):

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 8 | `gen` | Seqlock (§4): 2·`seq`+1 while being written, 2·`seq`+2 once complete, 0 = never written or cleared |
| 8 | 8 | `seq` | Frame number in this slot, from 1 |
| 16 | 8 | `captureNs` | Host clock ns of the camera sample's presentation time |
| 24 | 8 | `publishNs` | Host clock ns when the slot became complete |
| 32 | 4 | `flags` | bit 0: synthetic frame |
| 36 | 28 | — | reserved |

## 4. Slots and the seqlock

One writer, any number of readers; a reader never blocks the writer, and no reader can delay another.

**Sequence numbers never reset or repeat within an epoch.** `seq` starts at 1 in a new object and only increases
(by one per frame; clearing the slots doesn't restart it). So a `gen` value, once overwritten, never comes back: a
reader can't see the same even `gen` before and after a copy that a newer frame tore. A new sequence needs a new
object with a new epoch.

The writer keeps its geometry, `epoch` and last `seq` in private memory and never reads them back from the shared
header, which any process of the same user can rewrite: every slot address and index it uses comes from its own copy.

Writer, for frame `seq` (from 1, incrementing by one), slot `i = seq % nslots`:
1. `gen[i] = 2·seq+1` (relaxed), then a release fence: no pixel store becomes visible before the odd `gen`.
2. Write the pixels of slot `i`.
3. Store `seq`, `captureNs`, `publishNs`, `flags` of slot `i`.
4. `gen[i] = 2·seq+2` (release), then `latest = seq` (release).

Reader, given the last `seq` it delivered:
1. `s = latest` (acquire). If 0 or unchanged: nothing new. If `2·s+2` would overflow 64 bits: treat as torn.
2. `i = s % nslots`, using the reader's private `nslots` (§6). `g0 = gen[i]` (acquire). If `g0 ≠ 2·s+2`, the slot is
   being written or already holds a newer frame: drop this tick's read. Also drop it if the slot's `seq ≠ s`.
3. Copy `captureNs`, `publishNs`, `flags` and the pixels (`height` rows of 4·`width` bytes, stride `bpr0`, all from the
   private geometry).
4. Acquire fence, then `g1 = gen[i]`. The copy is good only if `g1 == g0` and `magic`, `version` and `epoch` still
   equal the validated values; otherwise discard it (torn, or the object was replaced: §6).

Timestamps are used only after step 4 passes. The engine clamps `captureNs` so presentation times stay monotonic and
lag the pump by at most 200 ms.

## 5. States

| Value | Name | Meaning | Reader |
|---:|---|---|---|
| 0 | `init` | Header being filled (magic not yet stored) | offline, retry |
| 1 | `starting` | Helper up, camera not yet delivering | offline |
| 2 | `prompting` | Waiting for the user to answer the camera permission prompt | offline; doctor says to answer the dialog |
| 3 | `running` | Frames are published | live while frames are fresh (§6) |
| 4 | `idle` | Camera closed (no active reader); slots cleared | offline |
| 5 | `denied` | Camera access denied or restricted; the helper exits | offline; doctor shows the System Settings path |
| 6 | `stopped` | Helper stopped cleanly; slots cleared; the name is being unlinked | unmap; re-open every 1 s |
| 7 | `superseded` | A new object (new geometry) replaces this one at the same name | unmap; re-open at once; never map this epoch again |

Any other value fails validation. The state only explains why a reader is offline; it never makes a reader live.

## 6. Readers

**Open** (at most once per second while offline, and at once after `superseded` or an identity change):
1. `shm_open(name, O_RDONLY)`, then `fstat`. Refuse unless `st_uid == getuid()` and `(st_mode & 077) == 0`; log
   `webcam: shm name /scw.… owned by uid N` or the offending mode.
2. Refuse unless 4096 ≤ `st_size` ≤ 1 GiB; `mmap` exactly `st_size` bytes, `PROT_READ`, `MAP_SHARED`; close the fd.
3. Validate once, from one snapshot of the header, with every product and sum overflow-checked:
   `magic` (0 → not ready); `version` accepted; `state` < 8; `format == 'BGRA'`; `headerBytes == 4096`;
   `epoch ≠ 0`; `flags == 0`; `1 ≤ nslots ≤ 8`; `1 ≤ width, height ≤ 8192`; `bpr0 ≥ 4·width` and a multiple of 4;
   `slotBytes` a nonzero multiple of 4096; `bpr0·height ≤ slotBytes`; `plane1Off == 0` and `bpr1 == 0`;
   `4096 + slotBytes·nslots == totalBytes ≤ st_size`.
4. Copy the geometry (`width`, `height`, `bpr0`, `nslots`, `slotBytes`, `epoch`, `version`, the mapping length) and a
   sanitized `device` into reader-private memory. Pixel addresses come only from this copy, so a header rewritten
   after validation can produce torn or stale frames but never an out-of-bounds read.

A reader may also refuse a valid ring larger than it is prepared to copy and draw each tick. The engine accepts at
most 4096×2160 (either orientation); a larger ring is unmapped at once and treated as offline, with the reason in the
log and the band (`webcam: ring WxH is larger than this engine accepts (4096x2160)`).

A re-open that finds the same `epoch` keeps the existing mapping. One that finds a different `epoch` replaces it. A
reader remembers the last few epochs it saw `superseded` or `stopped` and never maps them again: until the name holds a
new object it stays offline and retries on the 1 s schedule.

**Each tick** reads only `magic`, `version`, `epoch` (identity: any change means re-open), `state`, `latest`, and the
chosen slot's `gen`, `seq`, `captureNs` and `publishNs`. Never the geometry.

**Live** means `state == running` and the newest slot's `publishNs` (read under the seqlock) is less than 500 ms old.
Otherwise the reader is offline: it freezes the last frame for 1 s (so up to 1.5 s after the last frame), then shows the scene with the offline band,
and re-opens the name every second. That recovers from a helper restart, a crash (the old object keeps `running` but
its frames go stale, and the next helper unlinks it and creates a new one), an idle quit or a `down`.

## 7. Creating and retiring (helper)

**Create:**
1. `shm_unlink(name)`. `ENOENT` is fine. Any other failure: don't publish; report the owner (below).
2. `shm_open(name, O_CREAT|O_EXCL|O_RDWR, 0600)`. `EEXIST` (another process won the name) or `EACCES`: don't publish;
   open it read-only, `fstat` it and report `webcam: shm name /scw.… owned by uid N`.
3. `fchmod(fd, 0600)`, ignoring the result: on macOS it fails with `EINVAL` on a shm descriptor.
4. `fstat`: refuse to publish unless `st_uid == getuid()` and `(st_mode & 077) == 0`. This is the guarantee; it is
   never skipped. On macOS, `shm_open` doesn't apply the umask at all (the mode passed is the mode the object gets),
   and `st_dev`/`st_ino` are 0 for every shm object.
5. `ftruncate(fd, totalBytes)` once. macOS refuses any later resize (`EINVAL`), so a reader's mapping can't be
   truncated under it. `st_size` reads back rounded up to the page size.
6. `mmap` read-write, fill the header with `state = starting`, then store `magic` (release).

**A new geometry is a new object**, never an in-place rewrite, and the new one is ready before the old one says so:
1. create the new object at the same name (step 1 above unlinks the name; the old object stays mapped by the helper
   and the readers), with a new `epoch`, and publish it;
2. then retire the old one with `state = superseded` (below). Its name now holds the new object, so it isn't unlinked.

A reader that sees `superseded` re-opens at once and finds the new object already there.

**Retire** (`stuntcam down`, `source <other>` for the last sim, SIGTERM, idle quit): set `state = stopped`, clear the
slots, store the private `epoch` into this object's header again, then unlink the name only if the object at the name
carries that `epoch` (it is this object, not a newer one), and unmap. The check and the unlink are two steps; that is
safe only because the helper lock (§8) keeps a second helper from creating at the same name in between. Clearing sets
`latest = 0`, then for each slot sets `gen` to an odd value (a reader never accepts an odd `gen`, so one mid-copy discards
it), zeroes the pixels and the timestamps, and sets `gen = 0`. Closing the camera (`idle`) clears the slots the same way without unlinking. No frame
outlives the camera session in shared memory.

## 8. The pid file

`$SIMCAM_HOME/webcam/<cam>.pid`, same `<cam>` as the shm name. Mode 0600, written by the helper, read by the CLI,
`doctor` and engines. Because `open` detaches the helper, this file and the ring header are the CLI's only view of it.

**Lock.** The helper holds `flock(LOCK_EX)` on `$SIMCAM_HOME/webcam/<cam>.lock` (opened `O_CREAT|O_NOFOLLOW`, 0600)
for its whole life. The kernel drops the lock when the helper dies, so a stale pid file never blocks a new helper, and
two helpers can't both replace a stale file.
- **Only helpers take the lock.** The CLI, `doctor`, `gc` and engines never `flock` it, not even to probe: they read
  the pid file and check `kill(pid, 0)`. A probe would make a starting helper's non-blocking attempt fail.
- The helper still tolerates a momentary holder: it retries `LOCK_EX|LOCK_NB` every 10 ms for 250 ms before it
  concludes that another helper runs, and then exits.
- **Nothing ever unlinks a `.lock` file**, `gc` included: a helper holding the lock on an unlinked file and a new
  helper locking a fresh file at the same path would both believe they are alone.

**Format.** Text, `\n`-terminated lines. The first line is exactly `stuntcam-webcam 1`; then `key=value` lines:

| Key | Value | Required |
|---|---|---|
| `pid` | The helper's pid, 1–2147483647 | yes |
| `sha` | The helper's `<sha>` (12 lowercase hex), naming `$SIMCAM_HOME/helper/<sha>/` | yes |
| `rings` | Ring versions served, comma-separated, each 1–65535, at most 8 | yes |
| `auth` | `notDetermined`, `prompting`, `authorized` or `denied`: the camera permission as the helper sees it | yes |
| `state` | The ring state name (§5) | yes |
| `camera` | The camera's `uniqueID` (printable, under 128 bytes) | no |
| `started` | Unix seconds when the helper started | no |

A known key twice, a malformed value or a missing required key makes the file invalid; unknown keys are ignored (a
newer helper may add some). The helper rewrites the file on every change of `auth` or `state` by writing
`<cam>.pid.tmp` and renaming it over the old one, so readers never see a partial file. A pid file counts only while
its `pid` is alive (`kill(pid, 0)`) and the lock is held.

Example:

```
stuntcam-webcam 1
pid=4321
sha=0123456789ab
rings=1
auth=prompting
state=prompting
camera=0x1420000005ac8600
started=1794790000
```

The CLI polls `auth`: while it reads `prompting` it tells the user to answer the dialog, for at most 60 s. If the
running helper's `rings` share no version with the CLI's engine, the CLI stops it and starts its own.

## 8a. The default camera

`$SIMCAM_HOME/webcam/default`: one line, the `uniqueID` of the camera that the source `webcam` (with no `:<uniqueID>`)
means. A camera helper started without a camera argument (not `--synthetic`) writes it (tmp + rename) after it has chosen its default camera;
later the CLI writes it when it resolves `source webcam[:NAME]`. The engine re-reads it every second while `webcam` is
selected and switches to the ring of the camera it names. A value that is empty, 128 bytes or longer, or holds a
control character is ignored. The file is never removed: after a helper stops, it still names the ring a restarted
helper will publish. (The engine can't list Mac cameras, so a bare `webcam` needs a name to hash.)

## 9. Reader files

An engine with the `webcam` source selected rewrites `$SIMCAM_HOME/webcam/readers/<udid>-<pid>` once a second
(same tmp-and-rename), the only channel from readers back to the helper:

```
rings=1
want=1920x1080@30
active=1
```

`rings` lists the versions the engine accepts, `want` its active format (`<width>x<height>@<integer fps>`), and
`active` whether a capture session is running (the engine counts a session as running while its frame pump ticked in
the last 1.5 s). `<udid>` is the simulator's `SIMULATOR_UDID`, `<pid>` the app's pid. The engine writes the file from
its own 1 s timer from the moment the source is applied, and removes it when another source is applied; a process that
exits leaves it behind, and the helper removes a file whose pid is gone once it is 5 s old. The helper counts a file only while its pid is alive
**and** the file was modified within the last 3 s (by mtime; one more than 3 s in the future doesn't count either): a
live engine rewrites its file every second, so a process stopped in a debugger, or a reused pid, can't keep the camera
on. The helper opens the camera only while some reader is active, and closes it 10 s after the last one (so at most
about 13 s after a reader stops).

## 10. Security

- **Other users** can't read or plant a ring: the name carries the uid, the object is created `O_EXCL` with mode 0600
  and checked with `fstat` before publishing, and readers refuse any object not owned by their uid or with group or
  other bits. A planted object at this user's name is refused by owner; one at another name is never opened.
- **The same user** is trusted: any of the user's processes can read the feed from shared memory while the helper runs,
  without camera permission of its own (the same boundary as the user's files). The camera opens only for an active
  reader, and frames are cleared when it closes.
- **The writer trusts nothing it reads back:** a same-user process can open the 0600 object read-write and rewrite
  the header, but the helper takes its geometry, epoch and sequence from private memory, so the worst such a process
  can do is corrupt frames or make the ring look offline.
- **Readers can't write:** the mapping is `PROT_READ` from an `O_RDONLY` descriptor (macOS refuses `PROT_WRITE` on
  it and any later `mprotect` to write).
- **A hostile or broken header** can't crash a reader: validation is overflow-checked and runs once per mapping, the
  geometry used for addressing is private, and the parser is fuzzed (`engine/src/webcam/ring_fuzz.c`).
