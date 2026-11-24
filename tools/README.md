# tools/

Small helpers for the test scripts (`scripts/`) and for inspecting the engine. The compiled ones are built into `tools/bin/`
(git-ignored) by `tools/build.sh [name ...]` on a Mac with Xcode; each file's header says how to call it.

When a tool can't do its job it usually says so in one line on stderr: called with wrong arguments, it prints its
usage line and exits 2; an input it can't read, it prints `<tool>: <file>: <reason>` and exits 1 (a malformed input
of an unexpected shape can still end in a Python traceback). Two kinds of tools differ:
`orient.py` exits 2 for an unreadable picture as well, because 1 means "doesn't match", and `avinfo`, `avaudio` and
`upright` print an `ERROR` line on stdout, where `scripts/run.sh` and the orient scripts collect their output.

## Build and repo checks

- `build.sh` — compiles the Swift, C and Objective-C tools below into `tools/bin/` (all of them with no arguments).
- `spdx-check.sh` — lists tracked source files without an `SPDX-License-Identifier` first line (POSIX sh; runs
  anywhere).
- `privacy-check.sh` — fails on anything in a tracked file that shouldn't be public: secrets, personal emails, home
  paths, local hostnames, private IPs, symlinks, files over 5 MB, and the maintainer's local denylist when present
  (POSIX sh; CI runs it on every push and pull request).
- `hooks/pre-push` — runs `privacy-check.sh` before each push; enable it with `git config core.hooksPath tools/hooks`.

## Media (macOS, Swift)

- `avinfo.swift` — duration, tracks, dimensions, codec and image orientation of a photo or movie; `scripts/run.sh` runs
  it on every file an app reports.
- `avaudio.swift` — per-track start times, audio format and level, and the A/V start offset of a movie; also from
  `scripts/run.sh`.
- `upright.swift` — a photo or a movie frame as a viewer shows it (EXIF orientation, preferred transform applied), as a
  PNG; for `scripts/orient-lib.sh` and `scripts/orient-plain.sh`.
- `orient.py` — how a picture of the scene is rotated or mirrored, from its coloured top bar (Python with Pillow); for
  the orient scripts and `scripts/reclass.sh`.
- `mkvideo.swift` — an H.264 test movie (magenta, a moving white block); `scripts/golden.sh` makes its video with it,
  and `fixtures/media/README.md` says how the shipped one was made.
- `mkqr.swift` — a 1280x720 PNG with a QR code centred on white.

## Profiles

- `mkcfg.py` — writes an iPhone-like camera profile (format list) from a base profile in `engine/profiles/`.

## Crash reports

- `ipspid.py` — the crash reports of one process for the given pids since a time; `scripts/activation.sh` matches
  reports with it.
- `crashes.py` — the crash reports since a time from processes on the `SIMCAM_UDID_*` sims, and whether the engine was
  loaded in each.
- `ips.py`, `crash.py` — print a report's exception and its faulting (or triggered) thread's frames.
- `rununtilcrash.py` — an lldb command (`rucrash`) that continues a process until it crashes and prints the stack.

## Inside a simulator (Objective-C, `xcrun simctl spawn <udid> tools/bin/<name>`)

- `audiosrc.m` — the audio capture sources the simulator's capture stack reports, with their attributes.
- `dumpkeys.m` — the values of the named CFString constants in the capture frameworks.
- `mrctest.m` — decodes a QR image and rebuilds its descriptor through the private MRC functions.

## Engine inspection

- `inventory.py` — the static inventory of the engine's private dependencies (frameworks, symbols, classes, hooks);
  `docs/inventory.md` is its output (`--md`).
- `m2c/annot.py`, `m2c/stub.py` — disassemble a binary with its GOT binds, strings and selector stubs annotated, and
  resolve stub addresses to imported symbols.
- `pstart.c` — a process's start time (macOS host tool).
