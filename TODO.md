# TODO

Follow-ups from importing the prototype (spikes m1b–m12) into this repo.

- [ ] (W4 e56) Two installed runtimes can share a version (27.2 24B8117 usable, 27.2 24B91016s device-only and unusable on
      Xcode 27.0): `verify --runtime 27.2` and the default pick depend on simctl's order. Prefer runtimes with a device type
      this Xcode can create (or match by build), and say which build was picked. iOS 18.x: Xcode 27.0's default (arm64) download
      request says not available; Apple's index lists 18.x as universal-only — try `-architectureVariant universal` (8.9 GB;
      needs free disk above the 40 GB floor) before concluding U6 needs another Xcode.
- [ ] (W4 e57) The canary's trace-stage check `timestamp - slot12 (0..50 ms)` missed once in the 200-launch loop under load
      (27.0 L200-41: 98.4 ms). Always the first traced photo (photo 3); photos 4/5 stay ~0.2 ms; 4 of 88 first photos were
      over 10 ms. Between the slot-12 mark and the frame grab, `sess_IrisCapture` logs `M5Desc(settings, 2500)`, runs
      `M5DumpProps` and several KVC reads, then queues on the busy pump queue. Measure where the time goes, and move the long
      log/dump after the grab or the mark later, or the check will flake under load (design §9 asks ≤ 50 ms).
- [ ] (review r2, harness only) `late-waiter`/`mutation-L74` (activation.sh ~1108) assume the flush starts within
      ~150 ms of dispatch; under starvation they fail loudly (false FAIL / "not caught"): raise `M59_LATE_WAITER` (~1200) or log
      the flush start and check the gap. Its `*"still due"*` match also passes on "0 photo(s) still due": match `[1-9]`.
      santally.py: take the expected launch count and fail on fewer dirs or an empty console.txt (e57's first TSAN pass
      tallied 0, rc 0); the lane drivers print `santally rc=` but don't fail on it. Commit the zsh exit-path test (130/143)
      for `repick_seq`'s `always` block with the run's evidence.
- [ ] (W4 e49 release gate) Follow-ups: `vcfull`'s `1080p60.fps` missed 1–2 times in 50 (44.4 fps at load ~96; one 53.8 fps
      miss without a load sample; 20/20 exact at load ~20; W4 e57: also `1080p30.fps` 25.7 and `4k30.fps` 17.3 once each, and `1080p60.fps` 46.3, all at load 260–740, 20/20 exact at load ~15) — watch it, and move it to the known-timing list if it keeps missing; expo-camera
      touches UIKit off the main thread in `takePicture` (`SceneGeometry.windowScene`), so `MTC_CRASH_ON_REPORT=1` aborts expo
      apps (not stuntcam's code; reporting it upstream is an outward-facing action — ask the owner first); a rare rsync race
      in scripts/env.sh when ~5 scripts start at once.
- [x] **Before publishing the package (review)**, done in w4 e53 (measured internally): the canary
      unpack runs under an flock on `$SIMCAM_HOME/canary/.lock` and never renames away a matching dir; `bin/stuntcam` is
      built after `share/` is staged and embeds the sha256 of `manifest.txt` and `payload.tar` (integrity + authenticity
      relative to the binary); an installed binary uses a cwd checkout only with `STUNTCAM_USE_CHECKOUT=1` (or `--build`),
      never the executable's own checkout, and names the build/canary/profile/rules it uses; `--build
      $SIMCAM_HOME/builds/<h>` takes the package's canary, profile and rules; `treeMatches` compares the exec bit;
      `.DS_Store` ignored; `gc` removes old `canary/<id>` dirs and stale `.stage-*`/`.old-*`; `version --check` checks the
      binary's own package; the action fails on an unbalanced quote (`xargs -r` to a file under `$RUNNER_TEMP`).
- [x] **Before publishing the package (review)**, done in w4 e52 (measured internally): the Mach-O
      files ship in `share/stuntcam/payload.tar`, which the CLI checks against `manifest.txt` and unpacks into
      `$SIMCAM_HOME` at every use (with every other share file); checkout trust from an installed layout; any local cache
      entry skips the shipped verdict; shipped verdicts record the packager's Xcode; action.yml fixes (SIGPIPE, per-run cache
      key, udid default, quoted args, version from VERSION); `build.sh` uses `pwd -P`; `grep -F` leak check; module path
      `github.com/avinav-b/stuntcam/cli`; formula and action generated into `build/package/`.
- [ ] **Still before publishing:** run `brew install --build-from-source` + `brew test` of the generated formula once in a
      throwaway prefix or CI (e52 only simulated the relinking with `install_name_tool`/`codesign` on copies); ship verdicts
      only from the release-gate run on the release commit (`package.sh --verdicts <gate dir>`); the action's first run on
      a hosted `macos-26` runner (the fresh-runner `stuntcam-ci` path and actions/cache restore/save were never run);
      accounts, public repo, URLs, notarization (measured internally, W4 e51 "What remains").
- [x] Trace follow-ups (review), done in w4 e54: the dir is picked when the camera arms (L69); only the first
      crashing thread writes (L70); a dir removed or unwritable mid-run is re-picked once and logged once (L71); EXIF
      DateTimeOriginal (+ SubsecTime, OffsetTime) from the grab-time wall clock (L72); the canary's trace stage skips failed
      photos.
- [ ] Trace follow-ups left from review: `AVCapturePhoto.timestamp` could take the grabbed frame's own time (then
      timestamp − slot12 may go negative by up to a frame; make the canary/§9 bound two-sided); `will-capture`/`still-complete`
      are recorded when queued to the session queue, not when posted. A crash during PREPARE (before ARMED) leaves no trace by
      design (recording starts at ARMED; events.log has the `crash activate` line).
- [ ] Runner hook follow-ups (review r2): Guard Malloc (`libgmalloc`) and scheme/plan `DYLD_INSERT_LIBRARIES` are
      dropped from the runner (and, for hosted tests, from the app under test) with only a note: keep them in the hook's
      insert list or decline as for mixed sanitizers. Forwarded `SIMCAM_*` settings reach apps under test only on the
      named-destination launchd path, not the `id=` path: pick one behaviour. `env -C dir` isn't applied to the scheme /
      `-xctestrun` lookup. SwiftPM `.swiftpm/xcode` schemes' plans read as unreadable (the hook safely declines).
- [ ] **Rename env vars `SIMCAM_*` → `STUNTCAM_*`** (`SIMCAM`, `SIMCAM_DYLIB`, `SIMCAM_STUB_LOG`, `SIMCAM_PF_BREAK`,
      `SIMCAM_SLOT_COUNTS`, `SIMCAM_PF_NOSYMOK`, `SIMCAM_ORIENT`, `SIMCAM_CONTROL`, `SIMCAM_SOURCE`, and the scripts'
      `SIMCAM_UDID`, `SIMCAM_UDID_<rt>`). This covers the engine, the stub, scripts/ and tools/. Keep the old names as
      fallbacks for one release. Decide separately what happens to the per-spike off switches (`M2_*` … `M12_*`).
- [x] Split `engine/src/simcam.m` into fragments by concern (`fig/`, `core/`, `providers/`, `client/`), still one
      translation unit (unity build), so the compiled code is unchanged. Checked against `a5a7716`: every `__TEXT`,
      `__DATA_CONST` and `__DATA` section and `nm -m` are identical for arm64 and x86_64, normal and `--tsan` builds.
- [ ] Turn the fragments into separate compilation units (a private header for shared state; statics become
      `__attribute__((visibility("hidden")))` symbols) once the conformance matrix can catch regressions.
- [x] `fixtures/expo/App.tsx` replaced with m12's. Besides `-AUDIO 1` and
      `vcorient` / `expoorient` it also brought the `vccycle` / `expocycle` soak modes (`-CYCLES n`).
- [x] `fixtures/flutter/lib/main.dart` replaced with m12's (QR + EAN-13 scanner). No
      other source file of the app differed.
- [x] `canary/run.sh` (stale m8 copy) removed; `canary/README.md` documents `scripts/run.sh canary …`. `canary/build.sh`'s
      header fixed.
- [x] `tools/build.sh` builds the helpers into `tools/bin/`; scripts call `tools/bin/<name>`. `tools/mkcfg.py` takes the
      base profile as an argument (default `engine/profiles/cfg-iphone.plist`).
- [x] `scripts/build.sh` (m12's prototype build) removed; `./build.sh`
      replaces it.
- [x] m12's `plain12/` imported as `fixtures/plain-orient/`, its drivers (`p12.sh`, `lib.sh`, `orient-plain.sh`,
      `orient-lib.sh`, `lc-hook.sh`, `src-hook.sh`, `reclass.sh`) as `scripts/`.
- [x] Metro port is an env var: `EXPO_PORT` (`metro.sh`, `run.sh`: default 8099).
- [x] Dev scripts work from any clone location (W1 e1b): simulator processes can't see TCC-protected folders
      (`~/Documents`, `~/Desktop`, `~/Downloads`, iCloud Drive; a clone there loads nothing). `scripts/env.sh` (sourced by
      `./build.sh` and `scripts/*.sh`) puts every sim-visible path under `$SIMCAM_HOME` (default
      `~/Library/Application Support/stuntcam`): `builds/<hash>/` (copies made by `./build.sh`), `dev/<hash>/profiles/`
      and `dev/<hash>/media/` (staged once per content, so parallel clones never share a profile; `dev/profiles/` and
      `dev/media/` are rsync'd for the CLI's fallback only), `logs/` (boot logs), `results/` (the clone's `results` is a symlink to it),
      `sims/<udid>/chain.txt`. `boot.sh` warns about any path it passes that's in a protected folder.
- [x] Builds are reproducible for the same clone path and toolchain (`2b5b4cb`: fixed object path, `ZERO_AR_DATE=1`); the
      `-g` debug map holds absolute paths, so another clone path gives another hash. [ ] Prune old `$SIMCAM_HOME/builds/`
      (the CLI's `stuntcam gc` does, W3).
- [ ] `EXPO_PORT` defaults disagree: `lib.sh` / `lc-hook.sh` keep m12's 8101, while `metro.sh` serves `fixtures/expo` on
      8099. Until that's unified, run the orientation drivers with `EXPO_PORT=8099` (or start Metro with `EXPO_PORT=8101`).
- [ ] `scripts/src-hook.sh` needs `fixtures/media/still.png` (m12's 3.6 MB still, not committed). Commit a smaller
      still or have `tools/` generate one.
- [ ] `scripts/run.sh canary` doesn't copy crash reports into the result dir as the old `canary/run.sh` did
      (`tools/crashes.py <start>` lists them). Consider doing that for every app.
- [ ] `tools/build.sh`'s commands were reconstructed from m12's binaries (platform, min OS, libs); the spikes didn't
      record them. `tools/upright.swift` uses `AVAssetImageGenerator.copyCGImage`, deprecated since macOS 15.
- [ ] Bundle ids are still spike names (`dev.simcam.spike.m10canary`, `…m10plain`, `…m12plain`, `dev.spike.m1bcam`,
      `dev.spike.m7cam`). Rename them together with the `SIMCAM_*` env vars.
- [x] The earlier full-check harness removed before merging to `main`;
      replaced by a generic exhaustive VisionCamera mode (`-CAMTEST vcfull`, `scripts/run.sh vcfull|vcfull-audio`;
      check list and pre-registered result in `fixtures/expo/README.md`), against VisionCamera 5.2.4 / expo-camera 58.0.14.
- [x] Merge m11 arming: restore rules + chain file (now in `engine/loader/loader.c`); lazy arming (variant c, default on,
      `SIMCAM_LAZY=0` to arm at load) in `engine/src/core/lazy.inc`. Needs the W1 exit test.
- [x] Design §4.1 / U14: frozen stub (`engine/stub/stub.c`, 58 lines with comments) reads `$SIMCAM_SIM_DIR/current` ("<proto> <loader>") and dlopens
      `libsimcamloader.dylib` (restore rules, chain, engine next to it); `scripts/swap.sh` upgrades without a reboot; engine
      singleton (`simcam_engine_v1`). Tested in w1 e7 (swap 20/20, skew, duplicate engine, hosted tests 30/30).
- [ ] `proto` in the other host files (config, control, status, verdicts) once they exist (design §4.1 item 4).
- [x] CoreMotion stand-in installed at load (`w1/order`).
- [ ] Design §4.1 step 2: trampolines on the 3 private entry points instead of (or as well as) m11's public-API traps
      (the U2 ordering fixtures decide).
- [x] `tools/inventory.py`: static inventory of the engine's dependencies (frameworks, dlsym'd functions, local symbols,
      vtable slots, classes, hooked methods, ivars, selectors, contract strings, env switches). `--sdk` classifies
      public/private from SDK headers (run it on the Mac). [ ] Runtime half (§10.7): fingerprint of what the preflight
      checks, without code hashes; Xcode 27.0 insert list → `restore-rules/27A266a.json`.
- [x] Pause-aware op watchdog + crash sentinel (`engine/src/core/safety.inc`, design §5.4): ops around
      configure/start/stop and photo; active clock restarts deadlines after a stop; traced hangs not counted; chained
      signal handler attributes crashes by PC; events file; latch at activation (2 in 20 activations / 24 h, per build);
      `SIMCAM_SENTINEL=0|clear`, `SIMCAM_SENTINEL_LATCH=<feature>`; test hook `SIMCAM_WD_TEST=hang|pause|crash|crash-other`.
- [x] In-process latch wired: photo/movie/session error-complete with -16800 (apps see -11800); the photo latch is tested
      (w2 e10a, `conformance.sh latch-photo`). Session and movie latches aren't covered by a conformance case yet (design §5.4
      asks for one per feature), and the movie latch can't trigger in-process: no movie op is bracketed with SCOpBegin.
- [ ] Sentinel/watchdog follow-ups (W2): movie ops (writer finalisation deadline scaled to backlog), which also makes the
      in-process movie latch reachable; conformance cases latching the session and movie features mid-operation; hang latch "1 hang + our frames in a sample"; attribution by thread backtrace or a
      queue-specific key instead of PC only (an AVF exception inside our op aborts in libsystem and counts as
      crash-other); decay is by activation count and age only, not by verdict key; `stuntcam sentinel` CLI.
- [x] Photo encoding moved off the frame-pump queue to a serial `simcam.photo` queue (`w2/photo-queue`); needs the photo
      checks re-run (canary photo stages, the `vcfull` burst, plain photo + didFinishCapture).
- [x] (`w2/photo-queue`, w2 e8) Canary tri-state verdict (design §5.2) before CI gates PRs: timing-window failures are `inconclusive` and retried
      once (U7: 3/23 hosted-runner canary launches failed on runner slowness only).
- [x] (`w2/photo-queue`) `tools/crashes.py` only matched reports by our sims' library paths, so it misses simulator daemon crashes (e3b found
      35 by text search, none during soaks). Also match on the sim UDID in the report's paths and process names.
- [ ] Sanitizer runs: the stub can't load a `--tsan`/`--asan` engine into an uninstrumented app (the runtime must load at
      process start). Document the method that works (sanitizer-built app + stub, `scripts/torture.sh`) next to `build.sh`.
      Never put a sanitizer runtime into the boot-wide insert list: it crashed notifyd/syslogd and the boot never finished.
- [ ] Tone audio is recorded into movies while the app's own mic permission is still "not determined" (expo, w1 e6); a
      device would record nothing until granted. Follow the app's AVCaptureDevice.authorizationStatus(for: .audio) (W4).
- [ ] Test harness: an unanswered iOS permission alert outlives its app and covers later runs until the sim reboots
      (w1 e6). Fixtures that hit "not determined" should reboot or dismiss via `simctl privacy` grant afterwards.
- [x] Canary: a "longest frame gap during a photo ≤ 100 ms" timing check, so the pump stall the photo queue fixed
      can't come back unnoticed (w2 e8: base engine 125–283 ms gaps, 2 s during a burst; canary still passed).
- [x] **Background/foreground left AVFoundation and the engine out of step** (w2 e15; measured in w3 e20). The trigger is
      an app stop that lands while the engine has the session interrupted (a stop from `UIApplication` notifications, as
      plugins and the canary do; Apple's `sceneDidEnterBackground` stop arrives before the interruption and was fine).
      `M12EndInterrupt` then restarted the stopped session and AVF forwarded none of its later calls. Fixed: a stop or start
      during an interruption ends it for that session (sess_StopRunning / sess_StartRunning), so EndInterrupt restarts only
      sessions still interrupted; `M14_OLD_ENDINT` brings the bug back. Guard: `scripts/bgfg.sh` (L52; w3 e21).
- [ ] `M12EndInterrupt` doesn't check the session latch: a start, then an interruption before the latch's stop runs, leaves
      a latched session that the end of the interruption restarts (predates L52's fix; PR review).
- [ ] The canary's `fgSeen` is set at launch and never reset before BG-REQUEST, so "foreground notification after
      background" passes without a real foreground (9/11 runs in e15) and bgfg's `startRunning` races the relaunch. The
      canary's photo-then-stop can move back after its bgfg stage now (L52 fixed).
- [ ] Not measured (w3 e20): an app calling `startRunning` while interrupted in the background (the engine now runs it and
      ends the interruption; a device would keep it interrupted until foreground).
- [ ] A control-file interruption with engine reason 3 reaches the app as interruption reason 2 (w2 e15; not looked into).
- [ ] An interruption (`M12Interrupt`, core/control.inc) waits for in-flight photo encodes while it holds the pump queue, so
      in-flight photos complete before its DidStopRunning. A session op queued behind it (the app's `stopRunning` in
      `sceneDidEnterBackground` right after a photo) counts that encode time against its 2 s deadline: on a starved machine
      (7.6 s encodes, w1 U7) that records a session hang, and two latch the gate. Moving the wait off the pump queue needs the
      app's own stop/start and the interruption's DidStopRunning kept in order (an interruption, then the app's stop, then
      the end of the interruption must not restart a session the app stopped; today EndInterrupt doesn't check that either).
- [ ] Callers that gate on the canary's exit code must treat 3 (watchdog) like 4 (INCONCLUSIVE); `run.sh` reads the RESULT
      text, so it already does.
- [x] **An in-flight photo's `didFinishCaptureFor` could arrive after the session's stop** (17.5; e39 idle-175-3 cached a
      `fail(photo-then-stop: …)`). The engine's Fig notifications were in order; AVF delivers the photo delegate on the main
      queue and reports the stop from the `stopRunning` thread once its own DidStopRunning handling ran, unordered with it.
      Fixed in w4 e40 (ledger L66): a stop with photos of its session pending posts DidStopRunning, from a private queue,
      once AVF has run their didFinishCapture (≤ 1 s); a stop on a photo delegate queue (the main thread, or a photo
      callback) doesn't wait. `M40_OLD_STOP=1` brings the old order back; `M40_FINISH_DELAY_MS` makes it deterministic.
- [x] (w4 e40) A content `fail(…)` is confirmed by one more canary launch before it is cached (Go CLI and `scripts/verify.sh`); a
      pass after it is a short-lived pass with a `note:` line (`Result.FailedFirst`).
- [ ] (w4 e40) The interruption's DidStopRunning (`M12Interrupt`, core/control.inc) drains the photo queue but doesn't wait for
      the photos' delegate callbacks as `sess_StopRunning` now does (L66), so an interruption right after a shutter can still be
      reported before `didFinishCaptureFor` on 17.5. It can't wait the same way: it runs on the pump queue, and the end of the
      interruption must stay after it.
- [x] (w4 e40, fixed in e44) A stop with a photo pending in `sessionQueue.sync { session.stopRunning() }` from main waited the
      full 1 s for callbacks main can't run. e44 (review M1): a stop on the main thread doesn't wait (the main queue is
      AVF's photo delegate queue); plain `M5APP_PHOTOSTOP=mainsync`. Still bounded only by the 1 s: a stop on a thread main
      waits for some other way (a semaphore, a lock, or the common AVCam shape: main does `sessionQueue.sync {}` while a
      `sessionQueue.async { stopRunning() }` runs on a worker): a 1 s UI hitch there. w4 e54: the bound is 500 ms (covers the
      300 ms test fault; usual waits are 1.4-10 ms), the canary's stop timing check < 250 ms.
- [x] (review r2, done in w4 e54) The flush on startRunning (M40FlushStop) had a window and could wait behind another
      session's waiter: it now waits on gM40Cond until the session's deferred stop has queued its DidStopRunning (bounded), and
      the stop queue is concurrent (L73; `activation.sh flush-race`). Still unreachable through AVF (e44).
- [x] (review r2, done in w4 e54) A failed delegate-queue lookup's `MISMATCH: w4 e40` now fails the run (run.sh's
      canary summary, mutation.sh's baseline; mutant `M40_NO_DELEGATEQ`); tools/inventory.py tracks ActHookResolve hooks
      (with labels) and KVC keys/paths, docs/inventory.md regenerated. Only that one MISMATCH is fatal; the CLI's `verify`
      doesn't read the engine log for it yet.
- [ ] (w4 e40) With L66 fixed, L13 (encode back on the pump) is caught by the frame-gap check only; on 27.0 only the HEVC stage
      misses (212 ms). A canary check that doesn't depend on the encode time would make it robust.
- [x] (w4 e40, fixed in e44) `retriedLine` said "cached for 1 h only" also when the verdict wasn't cached at all (exported
      `SIMCTL_CHILD_*`); `verify.sh` too.
- [ ] (w4 e44) A `startRunning` from another thread while a `stopRunning` still waits for its DidStopRunning is dropped by AVF
      (17.5, 26.3, 27.0: no `_setRunning:YES`, the engine's StartRunning isn't called, the session ends stopped). The
      engine's photo wait (≤ 1 s, normally a few ms) widens that window. `sess_StartRunning`'s flush of a deferred stop (L4)
      is defensive: no AVF path reaching it was found.
- [ ] (w4 e48) `stuntcam trace` follow-ups: the ring has no preview/VDO/ADO/metadata ops (deliberately nothing per frame) and
      no op for an interruption or a runtime error; `gc` doesn't remove trace files of deleted apps (the engine keeps the
      newest 8 per bundle, and `gc` already drops deleted simulators' state); two ops written to one slot at once (64 ops
      recorded during one write) can mix fields; the CLI's `running` asks `launchctl list` in the sim, so a pid the sim
      reused for another launch of the same app reads as that launch. The ring under TSan was measured with the RN torture
      (5 launches) and the canary only, not with the plain app's `threads` order. 17.5: measured in boot mode only (its
      state dir was writable there; the container-tmp form was measured on 26.3/27.0).
- [ ] (w4 e48, L68) The photo timestamp is now taken when the photo's source frame is taken (the newest pumped frame),
      not that frame's own presentation time (up to one frame interval earlier). A device stamps the sensor frame; close
      enough for design §9 (≤ 50 ms), but `gLatest` could carry its PTS for an exact match.
- [ ] A photo-ordering fixture in the repo: e8's throwaway probe (5-tap burst + 15 spaced JPEG/HEIC captures; delivery, order,
      unique ids, callback order) was never committed.

## Known departures from the design, and week-1 items not done (review)
- [x] Lazy activation lacks design §4.1's guarantees: no 1 s self-timeout that forwards to stock, no "any failure → stock
      forever". It runs the preflight, the sentinel's file reads and every swizzle inside the app's first camera call, on the
      caller's thread. A per-thread re-entrancy guard exists (`lazy.inc`). Needed before v1.0 (W2/W3). **W4 PR A**: bounded
      activation on a private queue, the `activate` latch and camera markers (the lazy-activation plan,
      measured internally). `orient-bg` fixed with the owner's option (b) (a UI orientation snapshot at load, plan §8;
      ledger L57); `act-traced` (lldb SB API) and `act-tsan`'s threads x100 ran in the follow-up.
- [x] `stuntcam sentinel clear` can't reach a container-tmp `events.log` or marker (review item 1). When the state dir can't
      be opened, the engine falls back to `<container>/tmp/simcam-state/<bundle>/` (`preflight-fail-<build>` one level up),
      and the CLI's clear (state dir only) left that latch: the app stayed stock for up to 24 h while `status` said not
      latched. **Fixed in the CLI** (`w4/cli-ctmp`, measured internally, W4 e33): `status`, `sentinel ls` and
      `sentinel clear` find each app's data container (`simctl get_app_container <udid> <bundle> data`; bundles in the state
      dir, plus `--bundle`) and read/clear the fallback too (cli/internal/state/fallback.go): clear appends the clear line to
      an existing fallback `events.log` and removes `latched-*` and `preflight-fail-*` there, nothing else. A shut-down sim or
      an uninstalled app is reported and skipped; exit 1 only when an app could be cleared nowhere. `status`'s "a passing
      preflight or `stuntcam sentinel clear` resets it" is now true for the fallback as well; the engine comment
      (safety.inc, PREPARE 1 step 6) that said the CLI's clear works on the state dir only is corrected. 26.3 and 17.5 PASS.
- [ ] The CLI finds an app's container-tmp fallback only for bundles it already knows: a directory in the state dir, or
      `--bundle`. If the engine couldn't even create `<state>/<bundle>/` (on 17.5 the sim may not write `$SIMCAM_HOME` at all),
      `status` and a bare `sentinel clear` never look in that app's container. Option: list installed apps with
      `simctl listapps <udid>` (it includes each data container) and check `tmp/simcam-state/` in each (w4 e33). A
      shut-down sim's fallback isn't checked either (simctl answers only for booted devices); `status` says so. Also missed
      (review): app extensions (their own bundle id and TMPDIR; `get_app_container` fails for them, reported as not
      installed) and a scheme that overrides `TMPDIR` (the engine honours it). `simctl listapps` would also replace the
      per-bundle simctl calls with one per sim.
- [x] The CoreMotion stand-in installed at load sits outside the preflight and the sentinel until the app's first camera
      call: an app that never uses the camera keeps it even on a broken runtime or a latched build, and a crash in it is
      never attributed (no `motion` sentinel feature). In camera apps it switches to stock CoreMotion mid-run if the gate
      closes. The install at load also starts a 0.5 s main-thread orientation timer and device-orientation notifications in
      every armed app, camera or not. Options: a cheap latch check at load, a `motion` feature, or arming the stand-in at activation in non-camera
      apps only when the app queries motion. **W4 PR A** split off the orientation tracker (started by the first motion
      start/data call or the first camera call; no timer in apps that use neither) and removed the mid-run switch to stock
      (D2). Left for **PR B**: the load-time marker decision (stock from the next launch after a failed or latched camera
      activation), the one-shot "CoreMotion is stock" line, and `motion` crash attribution. Until then an app whose camera
      activation fails keeps the stand-in on every launch. **W4 PR B** (`w4/lazy-b`, measured internally): CoreMotion is
      decided once at load from this build's `latched-`, `latched-motion-` and `preflight-fail-` markers (stock with one-shot
      forwarding traps that print one line); a stand-in crash is `crash motion` through the thread-specific feature key and
      never closes the camera; the second strike's crash handler writes `latched-motion-<build>`; `preflight-fail` expires
      after 24 h. Ledger L58-L61.
- [x] PR B review round 2 lows, done in w4 e29 (measured internally): (1) the extra motion-key clears in `M5OrientTrackStart` (motion.inc:192) and
      ActEnsure's main-thread tracker run (lazy.inc:322) are untested (orient-crash/L61 still pass without them, since
      `upd` clears the key itself): add an `M16_ORIENT_FAULT` variant that faults in `M5OrientTrackRunMain` before `upd`.
      (2) The `preflight-fail` prune (safety.inc:494) can unlink a marker another app of the same build is writing (open
      with O_TRUNC sets mtime to now, `futimens` sets the until after): write via temp + rename, or prune only when
      `st_mtime <= now - 60`. (3) App code can run inside the motion scope through dynamic sends (an overridden
      `accelerometerUpdateInterval`, an `NSOperationQueue` subclass's `addOperationWithBlock:`, motion.inc:70-82): clear
      the key around those sends. (4) The stock line can print a re-written `latched-` marker's features with the
      load-time until (log text only). Done: (1) `M16_ORIENT_FAULT=start|runmain|ensure`, cases orient-crash-main and
      orient-crash-ensure, mutants L62/L63; (2) markers via temp file + rename (handler too), case pf-prune-race, mutant L64;
      (3) the key is cleared around those sends; (4) the stock line reads the features only from the load-time version.
      (3) had no guard of its own in e29 (only lazy.inc's clear, by injection, L63): w4 e31 added one, case app-send-crash
      (a CMMotionManager subclass's interval getter makes the first camera call; `M16_TRAP_FAULT` faults in its trap) and
      mutant L65 (`M17_APP_SEND_KEEP_KEY`). It exercises the interval getter only; the handler copy and
      `addOperationWithBlock:` go through the same macro and mutant but no case drives them.
- [x] review round 1 (w4 e31, measured internally): activation.sh matches crash reports by pid (the fault
      lines' pids, DiagnosticReports + Retired, `tools/ipspid.py`), and a case whose engine side passes but whose crash got no
      report is UNSURE, not FAIL (the host stops writing reports for an app after many crashes); the attitude archives as a
      plain CMAttitude; the preflight-fail / stale-marker prune renames an expired marker aside and re-checks it before the
      unlink. [ ] Left from the prune fix: while a fresh marker is aside (a few syscalls, only when it was re-written between
      the prune's stat and its rename), another process's load-time stat misses it for one launch; there is no case for this
      window (it needs two processes inside the same few microseconds). The re-check's "kept" branch itself (a fresh marker
      renamed in after the stat goes back) has a case since w4 e34: activation.sh pf-prune-kept (M16_PRUNE_PAUSE_MS).
- [x] review round 2 (w4 e34, measured internally): M1, while the stand-in is active every archived
      CMAttitude decodes as the stand-in (a decoded real CMAttitude's multiply by a stand-in segfaulted in CoreMotion, its
      isEqual: answered NO; measured on 27.0 and 26.3); `tools/ipspid.py` also requires the report's capture time >= the
      case's start (a reused pid can't match an old report) and prints tab-separated lines.
- [ ] Crash cases come out UNSURE on a Mac whose DiagnosticReports already holds 25 reports of the app (m5app, p12): macOS
      writes no new one until older ones move to Retired/ (w4 e29, e31: 12 of 14 crash-report case runs on e31's final head
      were UNSURE, every engine-side check passing). In e31 orient-crash-ensure's second crash also went without a report
      while only 5 m5app reports were present (cause unknown). A CI host needs either a clean DiagnosticReports per run, or
      the case's report checks moved to the engine's own evidence (the per-pid dylib log ends at the fault line).
- [x] Session ops (configure/start/stop, 2 s) included time spent waiting on the frame-pump queue behind a photo encode, so
      a slow encode could count as a session hang (U7 saw one). Fixed: photo encoding has its own serial queue.
- [ ] Design §1/§6.5 say the profiles offer 720p–4K; they offer 1920×1440, 1080p and 4K (no 720p; vcfull, W4).
- [x] U4 gaps from e26, fixed in e28 (measured internally, W4 e28): sanitizer runtimes and libgmalloc are never
      dlopened (ASan hosted tests 0/6 → pass; one TSan runtime); the "stuntcam boot mode displaced <tool>" line (Detox args,
      `IDB_*`, sanitizer/Guard Malloc settings without their runtime); the Xcode build recorded at arm time
      (`sims/<udid>/config.json`), `restore-rules/<build>.json` staged and read by the loader, the "restore rules unverified
      for Xcode <build>" line; `doctor`'s Xcode checks; `status`/`doctor` read boot mode from launchd_sim's environment.
- [ ] Not implemented from design §4.1: the `disabled` flag. Xcode's libraries load from `SIMULATOR_ROOT` (as Xcode 27.0
      does); the launching Xcode's root can't be derived under ⌘R (e26), so a future Xcode that loads one from its own tree
      needs a rules-file field for it.
- [ ] `tools/inventory`: the insert-list diff of design §4.1 (Xcode's insert list captured in launchd mode, before the libs
      remove themselves, against `restore-rules/<build>.json`), so a fifth library fails the beta ritual. Not built.
- [ ] U4 part 2 (needs a second Xcode, an Apple ID download; e26 "For the owner"): Xcode 27.1/26.6 side by side, an Xcode
      update with a sim booted; then add `restore-rules/<build>.json` for each measured build. Until then every other
      build gets the 27A266a rules and the "unverified" line.
- [ ] idb: not installed on the Mac, so the `IDB_*` displaced signal is unmeasured (e26, e28).
- [ ] The displaced-line signals are heuristics (review L9): any `IDB_*` variable, and `ASAN_OPTIONS`/`TSAN_OPTIONS`/
      Guard Malloc's `MALLOC_*` set for every launch, could print a false "displaced" line. e30 removes the boot-env case
      (`boot_signals` in `config.json`: signal variables that were in the boot environment are ignored); a variable a user
      sets globally per launch (e.g. in a CI wrapper) without the tool still prints it.
- [ ] `gc` of `$SIMCAM_HOME/restore-rules/<hash>/` dirs no armed sim's `config.json` names any more, and of the flat
      `restore-rules/*.json` files staged before e30 (review M2). Neither `stuntcam gc` nor the scripts remove them.
- [ ] Restore-rules staging nits (review r2): the hash is computed from one read of the files and the copy is a
      second read (an edit in between stages content that doesn't match its dir name); zsh's `(.N)` glob skips symlinked
      rules files while Go's os.Stat follows them; TestRulesHashMatchesEnvSh copies env.sh's pipeline instead of sourcing
      it and skips without zsh. A mismatch only duplicates a dir with the same content, so nothing breaks.
- [ ] XCUITest under ASan in boot mode fails (review L10, measured in e30, measured internally, W4 e30): the
      UI-test runner (`M11AppUITests-Runner`) gets the ASan runtime only through Xcode's per-launch insert, which boot mode
      displaces; the loader rightly doesn't dlopen it (skipped, "displaced AddressSanitizer" said once), and the runner then
      aborts when XCTest loads the instrumented UI-test bundle ("Interceptors are not working"); xcodebuild hangs. 0/2 (27.0,
      26.3). Not fixable after launch in boot mode; the line points at `--mode launchd`, where the app under test's camera is
      unmeasured. **Decided (2026-11-13): a documented boot-mode limit for v1.0** (docs/TROUBLESHOOTING.md); hosted
      sanitizer tests and ⌘R are unaffected. W4 e32 measured launchd mode: ASan ×2 and TSan ×1 UI tests run and pass per
      runtime, but with no camera and no stuntcam line (Xcode's per-launch insert replaces launchd's value), so steering
      them there would trade a hang for a silent camera-less run. A camera for sanitizer UI tests needs a new mechanism. The loader's displaced line suggests `--mode launchd`, which is
      right for Detox but not for a skipped sanitizer runtime: word it per cause.
- [x] `stuntcam exec -- xcodebuild test` gave a UI test's app under test no camera on launchd-armed and booted-unarmed sims
      (W4 e41: the runner hook was never built). **Built and measured in W4 e42 (measured internally)**:
      `libsimcamrunnerhook.dylib` (`engine/runnerhook/`), inserted by exec with `TEST_RUNNER_DYLD_INSERT_LIBRARIES` (approach A).
      27.0 + 26.3: UI tests' apps get 41-60 frames launchd-armed and unarmed (`launch`, `activate`, a test's own insert kept),
      boot-armed unchanged (hook inert), hosted tests pass in every mode (the hook restores `libXCTestBundleInject`), ASan UI
      tests get the camera (exec inserts the toolchain's ASan runtime into the runner; without it the runner aborted and the run
      hung). Left: the hook gives a hosted test's host app no camera on launchd/unarmed sims (could dlopen the stub there; not
      done); TSan through the same path unmeasured; exec's summary counts activations, not frames delivered (the state files
      have no frame count); a CI script that runs xcodebuild inside isn't recognized (exec warns for launchd/unarmed
      destinations it can see); private `_combinedLaunchEnvironment` is Xcode 27.0's: other Xcodes fall back to the public
      `launch`/`activate` swizzle (measured once, 27.0) and want a re-check per Xcode (U4 part 2).
      review r1, W4 e43 (measured internally): ASan through a **test plan** now detected (the e42 hang)
      and measured, 60/59 frames; an `.xctestrun` or plan whose runners need different sanitizer runtimes (ASan + TSan
      configurations, or one only in some) gets no hook and a warning (one variable can't differ per runner: such runs get
      no camera on launchd/unarmed sims; boot mode or per-configuration runs do); iOS simulators only; the hook only with a
      verified `test-runner` rule (`STUNTCAM_HOOK_FORCE=1`); `test-without-building` measured. Still left: TSan through the
      hook; another Xcode's `test-runner` rule; a tvOS/watchOS destination measured live (no such runtime on the Mac; unit
      tests only).
- [ ] Week-1 checklist items not done: ordering fixtures on 17.5 (done on 26.3/27.0 only); `ledger.md` and the per-runtime
      lifecycle record (the ledger was added in W2); the U17 re-arm log (needs days of real use); watermark checks for
      VisionCamera and expo on 17.5 (plain only), and expo's check is QR + EAN-13 decoding, not 18/18 orientation.
- [ ] `docs/inventory.md` is regenerated with `--sdk` on the Mac; `tools/inventory.py` now also counts constants resolved
      through a `dlsym` helper (`data-syms`).
- [ ] Engine gaps found by `vcfull` (week 1, e14a), W4: no 720p format in `cfg-m10.plist`/`cfg-iphone.plist`; zoom ramps
      (`rampToVideoZoomFactor`, VisionCamera's `startZoomAnimation`) never reach the engine; front photos with mirroring on
      keep EXIF Orientation 6 (VisionCamera reports `isMirrored=false`); focus locking not offered.
- [ ] VisionCamera 5.2.4 crashes on three un-awaited `capturePhotoToFile()` calls (settings reach AVFoundation out of order;
      also on a device). `vcfull` works around it; report upstream (ask the maintainer before filing).
- [ ] A simulator armed in launchd mode by the CLI and later re-armed by `scripts/launchd-arm.sh` keeps the CLI's
      SIMCAM_CONTROL (the script unsets only what it set), so script runs on it follow the user's control file; the
      scripts should unset SIMCAM_CONTROL or set their own. `gc` doesn't remove staged videos under `data/`.

## Follow-ups from the W3 Mac runs (e16b-e19b)
- [ ] The movie-race probe should wait for the engine's "stopping active recordings" line, not a fixed 150-250 ms: on 27.0
      the engine's stop always finished first, so the race wasn't exercised (e16b).
- [ ] The preview angle lags an orientation change by up to 0.5 s (motion.inc's timer); photos are right (e19b).
- [ ] The login service arms a booted sim in 6-8 s (e19, parallel setenv), above the ~5 s target; launches in that window
      get no camera.
- [ ] Untested: whether a DeviceHub window (Xcode 27's Simulator replacement) changes lastUsedAt, which `status` reads as
      the boot time (e17).
- [ ] `stuntcam env` on a sim whose `current` came from the scripts writes no record, so `source` refuses it (e19).
- [ ] `orient auto`'s "Simulator rotated meanwhile" branch needs a GUI rotation to test; a relaunched app sees portrait for
      ~0.5 s before the saved orientation lands (e19, e19b).
- [x] Device motion (kept available, W4): the stand-in's CMDeviceMotion doesn't supply `attitude` or `magneticField`
      (stock objects with no data behind them); an app reading attitude (expo-sensors) may get nil or zeros. Stand in an
      identity attitude and test reading it; also `availableAttitudeReferenceFrames` and the queue-less reference-frame
      start are unhooked (review). Done in w4 e29: an upright, still attitude consistent with gravity (Apple's
      conventions, checked against CoreMotion's own CMAttitude), zero uncalibrated magneticField, both hooks; cases
      motion-attitude and motion-attitude-stock.
- [ ] `stuntcam orient` (the control file's `orientation=`) reaches an app only once its camera arms: the engine reads the
      control file at activation (PREPARE 2) and polls it from START. An app that uses CoreMotion alone keeps following the
      UI/simulator orientation, so its gravity and attitude never follow `stuntcam orient` (w4 e29). Read the control file's
      orientation for motion too (at the first motion call, then polled while a motion stream runs), or document it.
- [ ] Attitude stand-in follow-ups (w4 e29): `attitude` is always yaw 0 and still (no rotation simulated); only
      xArbitraryZVertical is advertised, but a start in another frame gets the same samples; the stand-in's CMDeviceMotion
      and CMAccelerometerData still don't encode for NSSecureCoding. The attitude itself archives as a plain CMAttitude
      (e31: Apple's keys, byte-identical archive, decodes without the engine). With the stand-in active, a decode gives the
      stand-in (e34, design §6.1); a real CMAttitude obtained some other way (none from the public API then) still faults in
      `multiply(byInverseOf:)` and answers NO from `isEqual:` when the argument is a stand-in.
- [x] Production-app acceptance (design §9 step 3): check an app's motion recorder with device motion available (zero rotation rate,
      zero user acceleration, a still upright attitude since w4 e29) before calling U2's motion-recorder part done. **w4 e55:** 40/40
      motion recordings: accel + a device-motion stream, rotation/user-accel/mag all 0.0, no gyro/mag streams.
- [x] w4 e55 F1: EXIF DateTimeOriginal was stamped at encode time in whole seconds. Fixed (L72:
      grab-time wall clock, SubsecTime/OffsetTime, canary check); sub-second EXIF checked again in e57. Re-check against the
      production app pending.
- [x] w4 e55 step 7 (w4 e58, measured internally): attributed. **Engine leak found and fixed (L75):**
      every session leaked each converting sink's pixel buffer pool + VT transfer session (~3 MB; plain AVF +3.1–3.2 MB per
      open on 26.3/27.0, flat after the fix; guard `scripts/memcycle.sh` in `torture.sh … MEM`). The production app never hits it
      (preview + photo/movie sinks, BGRA, no conversion): its per-mount growth is app/RN-side (Instruments: Hermes `JSError`
      stacks, Fabric shadow nodes, VisionCamera session start; engine 1.6 KB in 13 mounts), 0.1–0.4 MB/cycle in Release and
      tapering, 0.5–0.9 in Debug. e55's accelerating growth didn't reproduce in 9 runs (Debug/Release, main/fixed/e55 engine,
      e55's own loops); Debug-only single-cycle jumps of +24 to +80 MB were the only outliers.
- [ ] w4 e58 follow-ups: `memcycle.sh` movie/both variants grow 0.3–0.5 MB/cycle on 26.3 in the system H.264 encoder's
      teardown (not gated).
      review: the preview sink's CAImageQueue + registered IOSurfaces (opt-in `M5_PREVIEW_IQ` path, video.inc) are not
      released in `M5Sink -dealloc`; `memcycle.sh` only covers the conversion pool/transfer session — add a mirrored/rotated
      video-data-output variant so the rotation pool and flip sessions' release is exercised too.
- [ ] w4 e55 F2: zoom (`VideoZoomFactor`, and on 27.0 `rampToVideoZoomFactor` too — contradicting the vcfull note below) is
      accepted but never rendered; README's "Device behaviour: … zoom" overclaims. Crop the source by the zoom factor, or
      document it as unrendered and fix README.
- [ ] Design flag names: §4 `up` synopsis `--mic tone|silence|mic`, §5 source `synthetic[:seed]`, the CI line and §9
      `--source synthetic --mic tone` don't match the CLI (`--source scene|image:|video:`, no `--mic`; the tone is the
      default). Fix the design or add `--mic`.
- [ ] Re-run §9 against the production app ×3 per runtime on the final head (after the memory work), and the release-gate
      run in boot mode with the T3 loader chained.
- [ ] w4 e55 F3: TROUBLESHOOTING line for T3's device panel replacing the stub in launchd's env (run `up` again).
- [ ] Motion stand-in rate is capped at 100 Hz (10 ms timer floor): document, or allow faster if an app needs it.
- [ ] U5 follow-ups (w4 e25): (1) a recording across a long freeze holds one frame for the freeze (frames stamped with
      the host clock); check what a device does and drop or re-time frames if needed. (2) done (w4 e27): the debugger rule
      holds on 27.0 and 26.3, 3/3 each with a counting control (`scripts/wd-traced.sh`). (3) Real Mac sleep is untested
      (the watchdog assumes CLOCK_MONOTONIC counts through sleep): `pmset sleepnow` with a scheduled wake and a session
      running, only with the maintainer's OK. (4) A recording deadline: see the movie-op item in the sentinel follow-ups above.
      (5) done (w4 e27): pp_waitn timeouts fail the case; the interrupt handling didn't work on a Mac and was fixed
      (`fixtures/pause-probe/selftest.sh`, 10/10 on 27.0). Still open from it: the harness's simctl calls have no timeout,
      so a sim whose app services hang (seen once in e27) makes a case hang rather than fail; selftest.sh ran on 27.0 only.
- [ ] `gc` should remove old `$SIMCAM_HOME/dev/<hash>/` profile/media stagings (scripts/env.sh stages one per content
      since w4; each holds every media file, ~4 MB with an untracked still.png) and `dev/.stage-*` left by a script
      killed mid-staging.
