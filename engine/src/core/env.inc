// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included first): the environment variables the engine reads, by name
// (all but the system's TMPDIR and SIMULATOR_UDID).
//
// Provides: ENV_*, the name of every environment variable the engine reads (every fragment)
//           EnvConfig, one of the six arming variables under its new or its 1.1.0 name (every fragment)
// Owns:     nothing: no value is cached here.
// Uses:     nothing.
//
// The engine reads a variable with getenv(ENV_...) where it acts: some at every use, some once (at load, at activation
// or at first use) into a global. Off switches and test faults read at every use go through a named predicate in
// core/switches.inc (PhotoDrainOn(), TestFaultLong(ENV_TEST_...)), which reads the environment each call (the few
// exceptions are listed there). Nothing here caches a value, so this file is an index: what exists, what kind of switch
// it is, and what it does. The constant's prefix says the kind at the point of use: ENV_OFF_ is an off switch,
// ENV_TEST_ a test fault, ENV_DIAG a diagnostic, ENV_LEGACY_ the stuntcam 1.1.0 name of a variable the CLI sets; the
// rest are settings. Four test faults keep variable names without that prefix (ENV_PF_BREAK, ENV_PF_NOSYMOK,
// ENV_SLOT_COUNTS, ENV_WD_TEST). A variable is "set" when it is present, whatever its value, unless its line says
// otherwise.
//
// To add a switch: add it to the group it belongs to (a one-line meaning, then the constant and the variable's name),
// read it where it acts (an off switch or test fault through a predicate in core/switches.inc, a setting with
// getenv(ENV_...)), and regenerate docs/inventory.md (tools/inventory.py lists the names from this file).

// ---- Settings: documented, for users and the CLI's per-launch options
// 0: this process is left alone (the stub and the loader check it first)
#define ENV_SIMCAM                      "SIMCAM"
// 0: arm at load instead of at the first camera call
#define ENV_LAZY                        "SIMCAM_LAZY"
// the first camera call's activation budget in ms, 1-60000 (never scaled)
#define ENV_ACTIVATE_TIMEOUT_MS         "SIMCAM_ACTIVATE_TIMEOUT_MS"
// the picture at launch: scene | image:PATH | video:PATH | webcam[:UNIQUEID] (alone: the camera in
//   $SIMCAM_HOME/webcam/default) | qr:TEXT (a QR code of TEXT, %XX-encoded: providers/qr.c)
#define ENV_SOURCE                      "SIMCAM_SOURCE"
// the virtual device orientation at launch: portrait | upsidedown (or portraitupsidedown) | landscapeleft |
//   landscaperight, or 1-4 (any case); anything else follows the simulator
#define ENV_ORIENT                      "SIMCAM_ORIENT"
// the text of the scene's QR code
#define ENV_QR_TEXT                     "SIMCAM_QR_TEXT"
// the 13 digits of the scene's EAN-13 barcode
#define ENV_EAN13                       "SIMCAM_EAN13"
// 0: no op trace
#define ENV_TRACE                       "SIMCAM_TRACE"
// 0: no crash/hang sentinel; clear: forget this app's records and latches at load
#define ENV_SENTINEL                    "SIMCAM_SENTINEL"
// <feature>: latch that feature by hand (two manual crash records)
#define ENV_SENTINEL_LATCH              "SIMCAM_SENTINEL_LATCH"
// delay of the pump's first tick in ms (default 10)
#define ENV_FIRST_TICK_MS               "SIMCAM_FIRST_TICK_MS"
// the webcam frame path: cg | vt (default: gpu)
#define ENV_WEBCAM_PATH                 "SIMCAM_WEBCAM_PATH"
// 0: the pump is not phase-locked to the webcam ring
#define ENV_WEBCAM_PHASE                "SIMCAM_WEBCAM_PHASE"

// stuntcam's host directory: part of the webcam ring's name, holds webcam/default and webcam/readers/. The CLI does
//   not set it in a simulator (unset or not absolute: derived from SIMCAM_SIM_DIR)
#define ENV_HOME                        "SIMCAM_HOME"

// ---- Paths the CLI sets when it arms a simulator
// path of the control file (source, orientation and event lines), polled while armed
#define ENV_CONTROL                     "SIMCAM_CONTROL"
// this simulator's directory under the host directory (sims/<udid>); the webcam source derives the host directory
//   and the UDID from it when SIMCAM_HOME or SIMULATOR_UDID is unset
#define ENV_SIM_DIR                     "SIMCAM_SIM_DIR"
// where the sentinel keeps <bundle id>/events.log with its latched-* markers, and preflight-fail-<build> (unset or
//   unwritable: a container-tmp fallback)
#define ENV_STATE_DIR                   "SIMCAM_STATE_DIR"

// ---- What the CLI sets when it arms a simulator: the camera itself
// Each of these six also has its stuntcam 1.1.0 name (ENV_LEGACY_...), and is read with EnvConfig(): a simulator
// armed by 1.1.0 carries the old names in its boot or launchd environment until it is re-armed, and must keep its
// camera when a newer engine loads there (`up` on a boot-armed sim, `swap`). The new name wins when both are set.
// The CLI sets both (cli/internal/arm LegacyNames), for older engines. Drop the old names here and there together:
// in 2.0 at the earliest (docs/renames.md, "Upgrading from 1.1.0").
// master switch: the Fig source factories answer with profile sources (presence); also required by the session hook
//   and the profile read
#define ENV_FIG_SOURCES                 "SIMCAM_FIG_SOURCES"
#define ENV_LEGACY_FIG_SOURCES          "M2_FAKE"
// do not wrap attribute/format dicts in the key-logging dictionary (set and not "0")
#define ENV_OFF_KEY_LOG                 "SIMCAM_OFF_KEY_LOG"
#define ENV_LEGACY_OFF_KEY_LOG          "M2_NOLOGDICT"
// camera profile plist path (read only with SIMCAM_FIG_SOURCES); missing: sources with no attributes
//   (built-in formats only), and apps crash at device discovery (the CLI refuses to arm without one:
//   cli/internal/artifacts)
#define ENV_PROFILE                     "SIMCAM_PROFILE"
#define ENV_LEGACY_PROFILE              "M2_PLIST"
// replace the Fig session slots (presence; only with SIMCAM_FIG_SOURCES)
#define ENV_FIG_SESSION                 "SIMCAM_FIG_SESSION"
#define ENV_LEGACY_FIG_SESSION          "M2_SESSION_HOOK"
// image the plain fixture scene draws (only used with SIMCAM_OFF_GENERATED_SCENE)
#define ENV_FIXTURE_IMAGE               "SIMCAM_FIXTURE_IMAGE"
#define ENV_LEGACY_FIXTURE_IMAGE        "M5_IMAGE"
// engine log file path, %p -> pid (the CLI sets it only with the stub log: `up --log`, verify)
#define ENV_ENGINE_LOG                  "SIMCAM_ENGINE_LOG"
#define ENV_LEGACY_ENGINE_LOG           "M5_LOG"

// one of the six above: its value under the new name, else under the 1.1.0 name, else NULL
static inline const char *EnvConfig(const char *name, const char *legacyName) {
    const char *value = getenv(name);
    return value ? value : getenv(legacyName);
}

// ---- Tuning: alternative values and experimental paths, for development
// only one fake source when the profile has no devices list
#define ENV_ONE_SOURCE                  "SIMCAM_ONE_SOURCE"
// "x,y,z" gravity override for the CoreMotion stand-in
#define ENV_GRAVITY                     "SIMCAM_GRAVITY"
// arg a of CAImageQueueInsertImage (CAImageQueue path)
#define ENV_PREVIEW_IMAGE_QUEUE_ARG_A   "SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_A"
// arg b of CAImageQueueInsertImage (CAImageQueue path)
#define ENV_PREVIEW_IMAGE_QUEUE_ARG_B   "SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_B"
// EXIF orientation override for photos
#define ENV_PHOTO_EXIF_ORIENTATION      "SIMCAM_PHOTO_EXIF_ORIENTATION"
// photo width when no dimensions are requested (default 4032)
#define ENV_PHOTO_WIDTH                 "SIMCAM_PHOTO_WIDTH"
// use the CAImageQueue preview path instead of posting IOSurfaces
#define ENV_PREVIEW_IMAGE_QUEUE         "SIMCAM_PREVIEW_IMAGE_QUEUE"
// fixed preview rotation degrees (pump sink + preview-layer hook)
#define ENV_PREVIEW_ROTATION            "SIMCAM_PREVIEW_ROTATION"
// 1 forces the fake microphone source, any other value keeps the stock one (unset: decided at the first query)
#define ENV_FAKE_MIC                    "SIMCAM_FAKE_MIC"
// "got": session hook by GOT rebind instead of the objc swizzle
#define ENV_SESSION_ROUTE               "SIMCAM_SESSION_ROUTE"
// audio source: tone (default) | silence | mic (mic falls back to tone unless the app is authorized or
//   SIMCAM_MIC_IGNORE_PERMISSION is set)
#define ENV_AUDIO_SOURCE                "SIMCAM_AUDIO_SOURCE"
// with SIMCAM_AUDIO_SOURCE=mic: open the mic even if the app is not authorized
#define ENV_MIC_IGNORE_PERMISSION       "SIMCAM_MIC_IGNORE_PERMISSION"
// degrees added to the connection angle in the preview formula used with SIMCAM_OFF_VIRTUAL_ORIENTATION (default
//   270; 90 with SIMCAM_OFF_SENSOR_LAYOUT)
#define ENV_PREVIEW_OFFSET              "SIMCAM_PREVIEW_OFFSET"
// VNDetectBarcodesRequest revision to pin (default 1)
#define ENV_VISION_REVISION             "SIMCAM_VISION_REVISION"

// ---- Diagnostics: extra logging and hooks, for development
// file of CFString symbol names: print each one's value (at activation, in the lookups step)
#define ENV_DIAG_DUMP_KEYS              "SIMCAM_DIAG_DUMP_KEYS"
// log every source property read and write (CopyProperty, SetProperty), not just the first per key
#define ENV_DIAG_EVERY_KEY              "SIMCAM_DIAG_EVERY_KEY"
// diagnostic hooks and lines: capturePhoto settings and exceptions (the shutter hook is installed even with
//   SIMCAM_TRACE=0), DidStopRunning/_setRunning threads, metadata output, didFinishCapture deliveries
#define ENV_DIAG                        "SIMCAM_DIAG"
// log every source SetProperty, not just the first per key
#define ENV_DIAG_SET_PROPERTY           "SIMCAM_DIAG_SET_PROPERTY"

// ---- Off switches: each turns one fix or feature off, so the old behaviour is back for that launch. The mutation
// suite (scripts/mutation.sh) and the activation suite set them to show that a test catches the bug (docs/ledger.md
// rows)
// no session finalize wrapper (teardown not tracked)
#define ENV_OFF_SESSION_FINALIZE        "SIMCAM_OFF_SESSION_FINALIZE"
// no ReadyForResponsiveRequest (didFinishCaptureFor never fires)
#define ENV_OFF_RESPONSIVE_NOTE         "SIMCAM_OFF_RESPONSIVE_NOTE"
// no control-file read or polling
#define ENV_OFF_CONTROL_FILE            "SIMCAM_OFF_CONTROL_FILE"
// no lifecycle / audio-session interruptions at all (other switches change only how they are delivered)
#define ENV_OFF_INTERRUPTIONS           "SIMCAM_OFF_INTERRUPTIONS"
// no virtual device orientation: buffer fixed to portrait, coordinator/gravity follow the UI
#define ENV_OFF_VIRTUAL_ORIENTATION     "SIMCAM_OFF_VIRTUAL_ORIENTATION"
// no UIDeviceOrientationDidChangeNotification after a forced orientation
#define ENV_OFF_ORIENTATION_NOTE        "SIMCAM_OFF_ORIENTATION_NOTE"
// front preview not mirrored
#define ENV_OFF_PREVIEW_MIRROR          "SIMCAM_OFF_PREVIEW_MIRROR"
// the plain fixture picture (image + red bar) instead of the generated scene, and no barcode detection
#define ENV_OFF_GENERATED_SCENE         "SIMCAM_OFF_GENERATED_SCENE"
// stop/interruption does not drain the photo queue (also turns off the stop's wait for in-flight photos'
//   didFinishCapture)
#define ENV_OFF_PHOTO_DRAIN             "SIMCAM_OFF_PHOTO_DRAIN"
// photo encode on the frame pump again
#define ENV_OFF_PHOTO_QUEUE             "SIMCAM_OFF_PHOTO_QUEUE"
// an app start/stop during an interruption no longer ends it
#define ENV_OFF_INTERRUPTION_RESET      "SIMCAM_OFF_INTERRUPTION_RESET"
// maxBracketedCapturePhotoCount reads 8 again
#define ENV_OFF_BRACKET_UNSUPPORTED     "SIMCAM_OFF_BRACKET_UNSUPPORTED"
// supportedMultiCamDeviceSets lists sets again
#define ENV_OFF_MULTICAM_UNSUPPORTED    "SIMCAM_OFF_MULTICAM_UNSUPPORTED"
// activation inline on the caller's thread, no budget (SIMCAM_ACTIVATE_TIMEOUT_MS, default 1 s)
#define ENV_OFF_ACTIVATION_TIMEOUT      "SIMCAM_OFF_ACTIVATION_TIMEOUT"
// no UI orientation snapshot at load
#define ENV_OFF_UI_ORIENTATION_AT_LOAD  "SIMCAM_OFF_UI_ORIENTATION_AT_LOAD"
// app-answerable sends keep the motion key
#define ENV_OFF_APP_SEND_KEY_CLEAR      "SIMCAM_OFF_APP_SEND_KEY_CLEAR"
// ActivationEnsure's main-thread tracker run keeps the key
#define ENV_OFF_ENSURE_KEY_CLEAR        "SIMCAM_OFF_ENSURE_KEY_CLEAR"
// markers written in place, not temp + rename
#define ENV_OFF_MARKER_RENAME           "SIMCAM_OFF_MARKER_RENAME"
// the crash handler writes no latched-motion marker
#define ENV_OFF_HANDLER_MOTION_MARKER   "SIMCAM_OFF_HANDLER_MOTION_MARKER"
// no motion feature key (stand-in crashes count as provider)
#define ENV_OFF_MOTION_KEY              "SIMCAM_OFF_MOTION_KEY"
// CoreMotion ignores latch/preflight markers at load
#define ENV_OFF_MOTION_MARKERS          "SIMCAM_OFF_MOTION_MARKERS"
// tracker upd runs under the motion key (its key clear off)
#define ENV_OFF_TRACKER_KEY_CLEAR       "SIMCAM_OFF_TRACKER_KEY_CLEAR"
// OrientTrackStart keeps the motion key
#define ENV_OFF_TRACKSTART_KEY_CLEAR    "SIMCAM_OFF_TRACKSTART_KEY_CLEAR"
// stop does not wait for in-flight photos' didFinishCapture
#define ENV_OFF_STOP_WAIT               "SIMCAM_OFF_STOP_WAIT"
// photo timestamp stamped after the encode
#define ENV_OFF_GRAB_TIMESTAMP          "SIMCAM_OFF_GRAB_TIMESTAMP"
// trace dir picked at the first flush
#define ENV_OFF_TRACE_DIR_AT_ARM        "SIMCAM_OFF_TRACE_DIR_AT_ARM"
// no trace dir re-pick
#define ENV_OFF_TRACE_REPICK            "SIMCAM_OFF_TRACE_REPICK"
// no crash-handler claim: every crashing thread writes the trace from one buffer
#define ENV_OFF_TRACE_CRASH_CLAIM       "SIMCAM_OFF_TRACE_CRASH_CLAIM"
// EXIF capture times at the encode
#define ENV_OFF_GRAB_EXIF_TIME          "SIMCAM_OFF_GRAB_EXIF_TIME"
// start's flush is a dispatch_sync on the stop queue again
#define ENV_OFF_FLUSH_WAIT              "SIMCAM_OFF_FLUSH_WAIT"
// a timed-out flush clears the stop's flush mark
#define ENV_OFF_FLUSH_MARK_KEPT         "SIMCAM_OFF_FLUSH_MARK_KEPT"
// no RotationCoordinator hooks
#define ENV_OFF_ROTATION_COORDINATOR    "SIMCAM_OFF_ROTATION_COORDINATOR"
// no notifications answering a SetProperty (adjusting focus/exposure/WB, manual completions, torch)
#define ENV_OFF_SET_PROPERTY_NOTES      "SIMCAM_OFF_SET_PROPERTY_NOTES"
// CoreMotion stays stock (no stand-in, no stock traps)
#define ENV_OFF_MOTION                  "SIMCAM_OFF_MOTION"
// no fileDataRepresentation / CGImageRepresentation fix
#define ENV_OFF_PHOTO_DATA              "SIMCAM_OFF_PHOTO_DATA"
// no audio source at all (mic or synthetic): audio data outputs get no buffers, movies no audio track
#define ENV_OFF_AUDIO                   "SIMCAM_OFF_AUDIO"
// no recommendedOutputSettings fix
#define ENV_OFF_RECOMMENDED_SETTINGS    "SIMCAM_OFF_RECOMMENDED_SETTINGS"
// no rotation/mirroring of video data output frames
#define ENV_OFF_DATA_OUTPUT_ROTATION    "SIMCAM_OFF_DATA_OUTPUT_ROTATION"
// no Vision barcode revision pin
#define ENV_OFF_VISION_REVISION_PIN     "SIMCAM_OFF_VISION_REVISION_PIN"
// the old sensor layout (scene top on the buffer's right, video data quarter turns reversed), preview offset 90
//   instead of 270 (SIMCAM_PREVIEW_OFFSET), and no webcam direct path
#define ENV_OFF_SENSOR_LAYOUT           "SIMCAM_OFF_SENSOR_LAYOUT"
// no ExposureTargetBias notification
#define ENV_OFF_BIAS_NOTE               "SIMCAM_OFF_BIAS_NOTE"
// movies ignore the active format (1080p30 H.264)
#define ENV_OFF_ACTIVE_FORMAT           "SIMCAM_OFF_ACTIVE_FORMAT"
// no manual exposure/focus/WB completion notifications
#define ENV_OFF_MANUAL_COMPLETION       "SIMCAM_OFF_MANUAL_COMPLETION"
// photos ignore maxPhotoDimensions
#define ENV_OFF_PHOTO_DIMS              "SIMCAM_OFF_PHOTO_DIMS"
// no preview surface with the photo
#define ENV_OFF_PHOTO_PREVIEW           "SIMCAM_OFF_PHOTO_PREVIEW"
// any torch key posts TorchActive (the old behaviour) instead of TorchLevel only
#define ENV_OFF_TORCH_LEVEL_ONLY        "SIMCAM_OFF_TORCH_LEVEL_ONLY"

// ---- Test faults: each injects a delay, a failure or a deliberate fault at one point. Set only by the test suites
// the preflight fails (forced: no preflight-fail marker)
#define ENV_PF_BREAK                    "SIMCAM_PF_BREAK"
// a missing session vtable symbol defers the slot check to the first session (forced)
#define ENV_PF_NOSYMOK                  "SIMCAM_PF_NOSYMOK"
// the session vtable slot counts the preflight accepts (default "19,24"; forced)
#define ENV_SLOT_COUNTS                 "SIMCAM_SLOT_COUNTS"
// <status>: the first photo fails with it
#define ENV_TEST_PHOTO_ERR              "SIMCAM_TEST_PHOTO_ERR"
// <status>: every app-requested recording stop reports it
#define ENV_TEST_MOVIE_ERR              "SIMCAM_TEST_MOVIE_ERR"
// <at>:<ms>[:<n>]: stalls in the webcam GPU path
#define ENV_TEST_WEBCAM_GPU_STALL       "SIMCAM_TEST_WEBCAM_GPU_STALL"
// hang | pause | crash | crash-other: the conformance suite's watchdog and sentinel cases
#define ENV_WD_TEST                     "SIMCAM_WD_TEST"
// PREPARE waits on the main queue
#define ENV_TEST_ACTIVATE_NEEDS_MAIN    "SIMCAM_TEST_ACTIVATE_NEEDS_MAIN"
// a CoreMedia fault on a global queue n ms after the first call
#define ENV_TEST_FIG_FAULT_AT_MS        "SIMCAM_TEST_FIG_FAULT_AT_MS"
// a fault in a motion timer tick 1 s after the first
#define ENV_TEST_MOTION_FAULT           "SIMCAM_TEST_MOTION_FAULT"
// a forced preflight failure is recorded as real (writes preflight-fail)
#define ENV_TEST_PF_FAIL_REAL           "SIMCAM_TEST_PF_FAIL_REAL"
// pause a prune n ms (pf-prune-kept)
#define ENV_TEST_PRUNE_PAUSE_MS         "SIMCAM_TEST_PRUNE_PAUSE_MS"
// PREPARE calls a trapped entry point
#define ENV_TEST_REENTER                "SIMCAM_TEST_REENTER"
// activation sleeps n ms [@phase]
#define ENV_TEST_SLOW_ACTIVATE_MS       "SIMCAM_TEST_SLOW_ACTIVATE_MS"
// pause a marker write n ms
#define ENV_TEST_SLOW_MARKER_MS         "SIMCAM_TEST_SLOW_MARKER_MS"
// START sleeps n ms
#define ENV_TEST_SLOW_START_MS          "SIMCAM_TEST_SLOW_START_MS"
// SWAP sleeps n ms
#define ENV_TEST_SLOW_SWAP_MS           "SIMCAM_TEST_SLOW_SWAP_MS"
// a fault in the first camera call's trap
#define ENV_TEST_TRAP_FAULT             "SIMCAM_TEST_TRAP_FAULT"
// the first photo stalls the pump n ms
#define ENV_TEST_FIRST_PHOTO_STALL_MS   "SIMCAM_TEST_FIRST_PHOTO_STALL_MS"
// keep the photo delegate queue busy n ms before didFinishCapture
#define ENV_TEST_BUSY_DELEGATE_QUEUE_MS "SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS"
// didFinishCapture handed to its queue n ms late
#define ENV_TEST_FINISH_DELAY_MS        "SIMCAM_TEST_FINISH_DELAY_MS"
// no photo is ever marked done (the stop waits its bound)
#define ENV_TEST_LOSE_PHOTO_DONE        "SIMCAM_TEST_LOSE_PHOTO_DONE"
// the delegate-queue lookup finds nothing (MISMATCH line)
#define ENV_TEST_NO_DELEGATE_QUEUE      "SIMCAM_TEST_NO_DELEGATE_QUEUE"
// force a start's flush into the window before the waiter
#define ENV_TEST_FLUSH_RACE             "SIMCAM_TEST_FLUSH_RACE"
// the stop waiter runs n ms late
#define ENV_TEST_LATE_WAITER_MS         "SIMCAM_TEST_LATE_WAITER_MS"
// preflight | throw | missing | crash | crashfig | swapthrow | crashstart | crash2: activation fails that way
//   (crashstart and crash2 test the trace: a crash before its first flush, two crashing threads at once)
#define ENV_TEST_ACTIVATE_FAIL          "SIMCAM_TEST_ACTIVATE_FAIL"
// start | runmain | ensure, anything else (1): upd: a fault at that point of the orientation tracker
#define ENV_TEST_ORIENT_FAULT           "SIMCAM_TEST_ORIENT_FAULT"
