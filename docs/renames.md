# Renamed names: milestone codes to plain names

Generated from the rename map (one row per rename, the table the mechanical rename commit applied) and
`engine/src/core/env.inc`. Regenerate it from the map instead of editing the tables by hand.

The engine, the CLI, the scripts and the docs used to name things after the milestone or experiment that added them
(`M5Render`, `M16_NO_ACTTIMEOUT`, `e38_test.go`, log lines starting `m12 `). They now have plain names that say what
they are. This file maps every old name to its new one.

## Reading older notes

Older commits, notes and issues use the names of their time, and their file:line references point at the commits
they were written against. Look an old name up below. A
name that is not here was not renamed (see "Not renamed").

## Upgrading from 1.1.0

stuntcam 1.1.0 arms a simulator with six engine settings under their old names (the table below). A simulator
armed by 1.1.0 keeps them in its boot environment (boot mode, until it shuts down) or its launchd environment
(launchd mode, until `stuntcam down`, a re-arm or a shutdown), and a newer build can start running there without a
re-arm: `stuntcam up` on a sim armed in boot mode this boot only points it at the new build (no reboot), and
`stuntcam swap` changes the build and nothing else. What keeps working, and why:

- The engine reads each of the six under its new name first and its 1.1.0 name second (`ENV_LEGACY_*` and
  `EnvConfig` in `engine/src/core/env.inc`), so a newer engine on a sim armed by 1.1.0 still has its camera. No
  other variable has a fallback: a renamed off switch, test fault, tuning or diagnostic variable works only under its
  new name.
- The CLI sets both names for the six (`LegacyNames` in `cli/internal/arm`): the new name, then the old one with the
  same value. A 1.1.0 engine that a newer CLI points a sim at (`stuntcam swap` to a 1.1.0 build, `up --build`, the
  login service running the build it pinned at `service install`) still finds its names. `stuntcam env` prints
  both, `exec` exports both, and arm records list both.
- `stuntcam down` unsets exactly the names an arm record lists: the old six for a 1.1.0 record, both sets for a new
  one. A launchd re-arm (`stuntcam up`) over a 1.1.0 arm sets every new and every old name and unsets none of them
  on the way.
- The runner hook (`engine/runnerhook`) forwards the six under both names on a launchd-armed sim.

The old names go from the engine and the CLI together, in 2.0 at the earliest.

| new name | 1.1.0 name |
|---|---|
| `SIMCAM_FIG_SOURCES` | `M2_FAKE` |
| `SIMCAM_FIG_SESSION` | `M2_SESSION_HOOK` |
| `SIMCAM_OFF_KEY_LOG` | `M2_NOLOGDICT` |
| `SIMCAM_PROFILE` | `M2_PLIST` |
| `SIMCAM_FIXTURE_IMAGE` | `M5_IMAGE` |
| `SIMCAM_ENGINE_LOG` | `M5_LOG` |

## Behaviour notes

- `stuntcam exec` copies the caller's exported `SIMCAM_*` variables to the test runner (as `TEST_RUNNER_*`), except
  its own and a simulator's paths and camera settings. (The runner hook, on a launchd-armed sim, forwards the
  runner's `SIMCAM_*` to the apps a test launches, the six camera settings included, under both names: those are
  the sim's own.) The renamed off switches, test faults, tuning and diagnostic variables now start with
  `SIMCAM_`, so when the shell that runs `exec` exports one, it reaches the test runner; as `M*_` names they never
  did. The six camera settings above are still not taken from the caller's shell: each destination gets its own.
- `M2_PREFLIGHT_BREAK` and `M2_PF_FAKEBREAK`, two aliases of `SIMCAM_PF_BREAK` that nothing set, are no longer read:
  use `SIMCAM_PF_BREAK`.
- `scripts/activation.sh`: the input `PRB=1` is now `MOTION_SUITE=1` (the old name is not read). Case names are
  unchanged.
- Log lines lost their run codes and the scripts that match them changed with them (see "Log strings"); for example
  `MISMATCH: w4 e40` is now `MISMATCH: photo stop`, and the `m12 ` prefix is gone.
- The Objective-C classes the engine registers in an app's process are named `Simcam*` (an app printing the class of
  a CoreMotion stand-in sees `SimcamAttitude`, not `M5Attitude`). Archives are unaffected: the stand-in encodes as
  `CMAttitude`.

## Environment variables (90)

| old | new | kind | ledger | meaning |
|---|---|---|---|---|
| `M10_NO_FINALIZE` | `SIMCAM_OFF_SESSION_FINALIZE` | off switch | L37 | no session finalize wrapper (teardown not tracked) |
| `M10_NO_RESPONSIVE` | `SIMCAM_OFF_RESPONSIVE_NOTE` | off switch | L07 | no ReadyForResponsiveRequest (didFinishCaptureFor never fires) |
| `M12_NO_CONTROL` | `SIMCAM_OFF_CONTROL_FILE` | off switch |  | no control-file read or polling |
| `M12_NO_INTERRUPT` | `SIMCAM_OFF_INTERRUPTIONS` | off switch |  | no lifecycle / audio-session interruptions at all (L21 and L52 are about how they are delivered) |
| `M12_NO_ORIENT` | `SIMCAM_OFF_VIRTUAL_ORIENTATION` | off switch |  | no virtual device orientation: buffer fixed to portrait, coordinator/gravity follow the UI |
| `M12_NO_ORIENTNOTE` | `SIMCAM_OFF_ORIENTATION_NOTE` | off switch |  | no UIDeviceOrientationDidChangeNotification after a forced orientation |
| `M12_NO_PREVIEWMIRROR` | `SIMCAM_OFF_PREVIEW_MIRROR` | off switch | L30 | front preview not mirrored |
| `M12_NO_SCENE2` | `SIMCAM_OFF_GENERATED_SCENE` | off switch |  | the plain fixture picture (image + red bar) instead of the generated scene, and no barcode detection |
| `M13_NO_PHOTODRAIN` | `SIMCAM_OFF_PHOTO_DRAIN` | off switch | L51 | stop/interruption does not drain the photo queue (also turns off L66's wait) |
| `M13_PHOTO_ON_PUMP` | `SIMCAM_OFF_PHOTO_QUEUE` | off switch | L13 | photo encode on the frame pump again |
| `M14_OLD_ENDINT` | `SIMCAM_OFF_INTERRUPTION_RESET` | off switch | L52 | an app start/stop during an interruption no longer ends it |
| `M15_OLD_BRACKET` | `SIMCAM_OFF_BRACKET_UNSUPPORTED` | off switch | L53 | maxBracketedCapturePhotoCount reads 8 again |
| `M15_OLD_MULTICAM` | `SIMCAM_OFF_MULTICAM_UNSUPPORTED` | off switch | L54 | supportedMultiCamDeviceSets lists sets again |
| `M16_ACTIVATE_FAIL` | `SIMCAM_TEST_ACTIVATE_FAIL` | test fault |  | preflight \| throw \| missing \| crash \| crashfig \| swapthrow \| crashstart \| crash2: activation fails that way (crashstart: L69, crash2: L70) |
| `M16_ACTIVATE_NEEDS_MAIN` | `SIMCAM_TEST_ACTIVATE_NEEDS_MAIN` | test fault |  | prepare waits on the main queue |
| `M16_FIG_FAULT_AT_MS` | `SIMCAM_TEST_FIG_FAULT_AT_MS` | test fault |  | a CoreMedia fault on a global queue n ms after the first call |
| `M16_MOTION_FAULT` | `SIMCAM_TEST_MOTION_FAULT` | test fault | L59,L60 | a fault in a motion timer tick 1 s after the first |
| `M16_NO_ACTTIMEOUT` | `SIMCAM_OFF_ACTIVATION_TIMEOUT` | off switch | L56 | activation inline on the caller's thread, no budget (SIMCAM_ACTIVATE_TIMEOUT_MS, default 1 s) |
| `M16_NO_UIORIENT_AT_LOAD` | `SIMCAM_OFF_UI_ORIENTATION_AT_LOAD` | off switch | L57 | no UI orientation snapshot at load |
| `M16_ORIENT_FAULT` | `SIMCAM_TEST_ORIENT_FAULT` | test fault | L61-L63 | start \| runmain \| ensure, anything else (1): upd: a fault at that point of the orientation tracker |
| `M16_PF_FAIL_REAL` | `SIMCAM_TEST_PF_FAIL_REAL` | test fault |  | a forced preflight failure is recorded as real (writes preflight-fail) |
| `M16_PRUNE_PAUSE_MS` | `SIMCAM_TEST_PRUNE_PAUSE_MS` | test fault |  | pause a prune n ms (pf-prune-kept) |
| `M16_REENTER` | `SIMCAM_TEST_REENTER` | test fault |  | prepare calls a trapped entry point |
| `M16_SLOW_ACTIVATE_MS` | `SIMCAM_TEST_SLOW_ACTIVATE_MS` | test fault | L56 | activation sleeps n ms [@phase] |
| `M16_SLOW_MARKER_MS` | `SIMCAM_TEST_SLOW_MARKER_MS` | test fault | L64 | pause a marker write n ms |
| `M16_SLOW_START_MS` | `SIMCAM_TEST_SLOW_START_MS` | test fault |  | START sleeps n ms |
| `M16_SLOW_SWAP_MS` | `SIMCAM_TEST_SLOW_SWAP_MS` | test fault |  | SWAP sleeps n ms |
| `M16_TRAP_FAULT` | `SIMCAM_TEST_TRAP_FAULT` | test fault | L65 | a fault in the first camera call's trap |
| `M17_APP_SEND_KEEP_KEY` | `SIMCAM_OFF_APP_SEND_KEY_CLEAR` | off switch | L65 | app-answerable sends keep the motion key |
| `M17_ENSURE_KEEP_KEY` | `SIMCAM_OFF_ENSURE_KEY_CLEAR` | off switch | L63 | ActEnsure's main-thread tracker run keeps the key |
| `M17_MARKER_INPLACE` | `SIMCAM_OFF_MARKER_RENAME` | off switch | L64 | markers written in place, not temp + rename |
| `M17_NO_HANDLER_MARKER` | `SIMCAM_OFF_HANDLER_MOTION_MARKER` | off switch | L60 | the crash handler writes no latched-motion marker |
| `M17_NO_MOTION_KEY` | `SIMCAM_OFF_MOTION_KEY` | off switch | L59 | no motion feature key (stand-in crashes count as provider) |
| `M17_NO_MOTION_MARKERS` | `SIMCAM_OFF_MOTION_MARKERS` | off switch | L58 | CoreMotion ignores latch/preflight markers at load |
| `M17_TRACKER_MOTION_SCOPE` | `SIMCAM_OFF_TRACKER_KEY_CLEAR` | off switch | L61 | tracker upd runs under the motion key (its key clear off) |
| `M17_TRACKSTART_KEEP_KEY` | `SIMCAM_OFF_TRACKSTART_KEY_CLEAR` | off switch | L62 | OrientTrackStart keeps the motion key |
| `M2_DUMPKEYS` | `SIMCAM_DIAG_DUMP_KEYS` | diagnostic |  | file of CFString symbol names: print each one's value (PREPARE 2) |
| `M2_FAKE` | `SIMCAM_FIG_SOURCES` | camera setting the CLI sets |  | master switch: the Fig source factories answer with profile sources (presence); also required by the session hook and the profile read |
| `M2_NOLOGDICT` | `SIMCAM_OFF_KEY_LOG` | camera setting the CLI sets |  | do not wrap attribute/format dicts in the key-logging dictionary (set and not "0") |
| `M2_ONE_SOURCE` | `SIMCAM_ONE_SOURCE` | tuning |  | only one fake source when the profile has no devices list |
| `M2_PLIST` | `SIMCAM_PROFILE` | camera setting the CLI sets | L24,L25 | camera profile plist path (read only with SIMCAM_FIG_SOURCES); missing: sources with no attributes (built-in formats only), and apps crash at device discovery (the CLI refuses to arm without one: cli/internal/artifacts) |
| `M2_SESSION_HOOK` | `SIMCAM_FIG_SESSION` | camera setting the CLI sets |  | replace the Fig session slots (presence; only with SIMCAM_FIG_SOURCES) |
| `M2_VERBOSE` | `SIMCAM_DIAG_EVERY_KEY` | diagnostic |  | log every source property read and write (CopyProperty, SetProperty), not just the first per key |
| `M38_FIRST_PHOTO_STALL_MS` | `SIMCAM_TEST_FIRST_PHOTO_STALL_MS` | test fault |  | the first photo stalls the pump n ms |
| `M40_BUSY_DELEGATEQ_MS` | `SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS` | test fault |  | keep the photo delegate queue busy n ms before didFinishCapture |
| `M40_FINISH_DELAY_MS` | `SIMCAM_TEST_FINISH_DELAY_MS` | test fault | L66 | didFinishCapture handed to its queue n ms late |
| `M40_LOSE_DONE` | `SIMCAM_TEST_LOSE_PHOTO_DONE` | test fault | L66 | no photo is ever marked done (the stop waits its bound) |
| `M40_NO_DELEGATEQ` | `SIMCAM_TEST_NO_DELEGATE_QUEUE` | test fault | L66 | the delegate-queue lookup finds nothing (MISMATCH line) |
| `M40_OLD_STOP` | `SIMCAM_OFF_STOP_WAIT` | off switch | L66 | stop does not wait for in-flight photos' didFinishCapture |
| `M48_OLD_PTS` | `SIMCAM_OFF_GRAB_TIMESTAMP` | off switch | L68 | photo timestamp stamped after the encode |
| `M54_FLUSH_RACE` | `SIMCAM_TEST_FLUSH_RACE` | test fault | L73 | force a start's flush into the window before the waiter |
| `M54_LATE_TRACE_DIR` | `SIMCAM_OFF_TRACE_DIR_AT_ARM` | off switch | L69 | trace dir picked at the first flush |
| `M54_NO_REPICK` | `SIMCAM_OFF_TRACE_REPICK` | off switch | L71 | no trace dir re-pick |
| `M54_OLD_CRASH` | `SIMCAM_OFF_TRACE_CRASH_CLAIM` | off switch | L70 | no crash-handler claim: every crashing thread writes the trace from one buffer |
| `M54_OLD_EXIF` | `SIMCAM_OFF_GRAB_EXIF_TIME` | off switch | L72 | EXIF capture times at the encode |
| `M54_OLD_FLUSH` | `SIMCAM_OFF_FLUSH_WAIT` | off switch | L73 | start's flush is a dispatch_sync on the stop queue again |
| `M59_LATE_WAITER` | `SIMCAM_TEST_LATE_WAITER_MS` | test fault | L74 | the stop waiter runs n ms late |
| `M59_OLD_FLUSHMARK` | `SIMCAM_OFF_FLUSH_MARK_KEPT` | off switch | L74 | a timed-out flush clears the stop's flush mark |
| `M5_DIAG` | `SIMCAM_DIAG` | diagnostic |  | diagnostic hooks and lines: capturePhoto settings and exceptions (the shutter hook is installed even with SIMCAM_TRACE=0), DidStopRunning/_setRunning threads, metadata output, didFinishCapture deliveries |
| `M5_GRAVITY` | `SIMCAM_GRAVITY` | tuning |  | "x,y,z" gravity override for the CoreMotion stand-in |
| `M5_IMAGE` | `SIMCAM_FIXTURE_IMAGE` | camera setting the CLI sets |  | image the plain fixture scene draws (only used with SIMCAM_OFF_GENERATED_SCENE) |
| `M5_IQ_A` | `SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_A` | tuning |  | arg a of CAImageQueueInsertImage (CAImageQueue path) |
| `M5_IQ_B` | `SIMCAM_PREVIEW_IMAGE_QUEUE_ARG_B` | tuning |  | arg b of CAImageQueueInsertImage (CAImageQueue path) |
| `M5_LOG` | `SIMCAM_ENGINE_LOG` | camera setting the CLI sets |  | engine log file path, %p -> pid (the CLI sets it only with the stub log: `up --log`, verify) |
| `M5_LOGSET` | `SIMCAM_DIAG_SET_PROPERTY` | diagnostic |  | log every source SetProperty, not just the first per key |
| `M5_NO_COORD` | `SIMCAM_OFF_ROTATION_COORDINATOR` | off switch | L33 | no RotationCoordinator hooks |
| `M5_NO_METERING` | `SIMCAM_OFF_SET_PROPERTY_NOTES` | off switch | L01 | no notifications answering a SetProperty (adjusting focus/exposure/WB, manual completions, torch) |
| `M5_NO_MOTION` | `SIMCAM_OFF_MOTION` | off switch | L40 | CoreMotion stays stock (no stand-in, no stock traps) |
| `M5_NO_PHOTOFIX` | `SIMCAM_OFF_PHOTO_DATA` | off switch | L05 | no fileDataRepresentation / CGImageRepresentation fix |
| `M5_PHOTO_ORIENT` | `SIMCAM_PHOTO_EXIF_ORIENTATION` | tuning |  | EXIF orientation override for photos |
| `M5_PHOTO_W` | `SIMCAM_PHOTO_WIDTH` | tuning |  | photo width when no dimensions are requested (default 4032) |
| `M5_PREVIEW_IQ` | `SIMCAM_PREVIEW_IMAGE_QUEUE` | tuning |  | use the CAImageQueue preview path instead of posting IOSurfaces |
| `M5_PREVIEW_ROT` | `SIMCAM_PREVIEW_ROTATION` | tuning |  | fixed preview rotation degrees (pump sink + preview-layer hook) |
| `M6_FAKE_MIC` | `SIMCAM_FAKE_MIC` | tuning |  | 1 forces the fake microphone source, any other value keeps the stock one (unset: decided at the first query) |
| `M6_NO_MIC` | `SIMCAM_OFF_AUDIO` | off switch |  | no audio source at all (mic or synthetic): audio data outputs get no buffers, movies no audio track |
| `M6_SESSHOOK` | `SIMCAM_SESSION_ROUTE` | tuning |  | "got": session hook by GOT rebind instead of the objc swizzle |
| `M7_AUDIO_SRC` | `SIMCAM_AUDIO_SOURCE` | tuning |  | audio source: tone (default) \| silence \| mic (mic falls back to tone unless the app is authorized or SIMCAM_MIC_IGNORE_PERMISSION is set) |
| `M7_MIC_FORCE` | `SIMCAM_MIC_IGNORE_PERMISSION` | tuning |  | with SIMCAM_AUDIO_SOURCE=mic: open the mic even if the app is not authorized |
| `M7_NO_RECFIX` | `SIMCAM_OFF_RECOMMENDED_SETTINGS` | off switch | L17 | no recommendedOutputSettings fix |
| `M7_NO_VDOROT` | `SIMCAM_OFF_DATA_OUTPUT_ROTATION` | off switch | L30 | no rotation/mirroring of video data output frames |
| `M7_NO_VISIONFIX` | `SIMCAM_OFF_VISION_REVISION_PIN` | off switch | L35 | no Vision barcode revision pin |
| `M7_PREVIEW_OFS` | `SIMCAM_PREVIEW_OFFSET` | tuning |  | degrees added to the connection angle in the preview formula used with SIMCAM_OFF_VIRTUAL_ORIENTATION (default 270; 90 with SIMCAM_OFF_SENSOR_LAYOUT) |
| `M7_SRC_LEGACY` | `SIMCAM_OFF_SENSOR_LAYOUT` | off switch | L10 | the old sensor layout (scene top on the buffer's right, video data quarter turns reversed), preview offset 90 instead of 270 (SIMCAM_PREVIEW_OFFSET), and no webcam direct path |
| `M7_VISION_REV` | `SIMCAM_VISION_REVISION` | tuning | L35 | VNDetectBarcodesRequest revision to pin (default 1) |
| `M8_NO_BIASNOTE` | `SIMCAM_OFF_BIAS_NOTE` | off switch | L03 | no ExposureTargetBias notification |
| `M8_NO_FORMATDIMS` | `SIMCAM_OFF_ACTIVE_FORMAT` | off switch | L16 | movies ignore the active format (1080p30 H.264) |
| `M8_NO_MANUALCOMPLETE` | `SIMCAM_OFF_MANUAL_COMPLETION` | off switch | L03 | no manual exposure/focus/WB completion notifications |
| `M8_NO_PHOTODIMS` | `SIMCAM_OFF_PHOTO_DIMS` | off switch | L06 | photos ignore maxPhotoDimensions |
| `M8_NO_PREVIEWSURFACE` | `SIMCAM_OFF_PHOTO_PREVIEW` | off switch | L08 | no preview surface with the photo |
| `M8_OLD_TORCH` | `SIMCAM_OFF_TORCH_LEVEL_ONLY` | off switch | L02 | any torch key posts TorchActive (the old behaviour) instead of TorchLevel only |
| `M2_PREFLIGHT_BREAK` | (removed) | test fault | L44 | an alias of `SIMCAM_PF_BREAK`: use that |
| `M2_PF_FAKEBREAK` | (removed) | test fault | L44 | an alias of `SIMCAM_PF_BREAK`: use that |

## Engine symbols (340)

Functions, globals, constants, macros and the Objective-C classes and selectors the engine defines, with the file
under `engine/src/` that defines them.

| old | new | in | |
|---|---|---|---|
| `gM11Lazy` | `gActLazy` | core/init.inc |  |
| `gM16Fail` | `gTestActivateFail` | core/act.inc |  |
| `gM16FigFaultMs` | `gTestFigFaultAtMs` | core/act.inc |  |
| `gM16MotionFault` | `gTestMotionFault` | core/act.inc |  |
| `gM16NeedsMain` | `gTestActivateNeedsMain` | core/act.inc |  |
| `gM16NoTimeout` | `gOffActivationTimeout` | core/act.inc |  |
| `gM16NoUIOrientAtLoad` | `gOffUIOrientationAtLoad` | core/act.inc |  |
| `gM16OrientFault` | `gTestOrientFault` | core/act.inc |  |
| `gM16PfReal` | `gTestPfFailReal` | core/act.inc |  |
| `gM16PrunePauseMs` | `gTestPrunePauseMs` | core/safety.inc |  |
| `gM16Reenter` | `gTestReenter` | core/act.inc |  |
| `gM16SlowAt` | `gTestSlowActivateAt` | core/act.inc |  |
| `gM16SlowMarkerMs` | `gTestSlowMarkerMs` | core/safety.inc |  |
| `gM16SlowMs` | `gTestSlowActivateMs` | core/act.inc |  |
| `gM16SlowStartMs` | `gTestSlowStartMs` | core/act.inc |  |
| `gM16SlowSwapMs` | `gTestSlowSwapMs` | core/act.inc |  |
| `gM16TrapFault` | `gTestTrapFault` | core/act.inc |  |
| `gM17AppSendKeepKey` | `gOffAppSendKeyClear` | core/act.inc |  |
| `gM17EnsureKeepKey` | `gOffEnsureKeyClear` | core/act.inc |  |
| `gM17MarkerInplace` | `gOffMarkerRename` | core/safety.inc |  |
| `gM17NoHandlerMarker` | `gOffHandlerMotionMarker` | core/safety.inc |  |
| `gM17TrackerMotionScope` | `gOffTrackerKeyClear` | core/act.inc |  |
| `gM17TrackStartKeepKey` | `gOffTrackStartKeyClear` | core/act.inc |  |
| `gM40BusyMs` | `gTestBusyDelegateQueueMs` | client/photo.inc |  |
| `gM40Cond` | `gStopWaitCond` | fig/session_state.inc |  |
| `gM40DelayMs` | `gTestFinishDelayMs` | client/photo.inc |  |
| `gM40DelegateQKey` | `gStopWaitDelegateQKey` | fig/session_state.inc |  |
| `gM40Flushed` | `gStopWaitFlushed` | fig/session_state.inc |  |
| `gM40Hooked` | `gStopWaitHooked` | fig/session_state.inc |  |
| `gM40LoseDone` | `gTestLosePhotoDone` | client/photo.inc |  |
| `gM40MainDelegateQ` | `gStopWaitMainDelegateQ` | fig/session_state.inc |  |
| `gM40NoDelegateQ` | `gTestNoDelegateQueue` | client/photo.inc |  |
| `gM40Pending` | `gStopWaitPending` | fig/session_state.inc |  |
| `gM40StopGen` | `gStopWaitStopGen` | fig/session_state.inc |  |
| `gM40Stops` | `gStopWaitStops` | fig/session_state.inc |  |
| `gM40StopsPosted` | `gStopWaitStopsPosted` | fig/session_state.inc |  |
| `gM54Go` | `gTestCrashGo` | core/lazy.inc |  |
| `gM5DiagCap` | `gDiagCapture` | client/photo.inc |  |
| `gM5FirstTick` | `gMotionFirstTick` | client/motion.inc |  |
| `gM5StockNoted` | `gMotionStockNoted` | client/motion.inc |  |
| `gOrigM40DidFinish` | `gOrigHandleDidFinishCapture` | client/photo.inc |  |
| `gOrigM40WillBegin` | `gOrigHandleWillBeginCapture` | client/photo.inc |  |
| `kM10SlotWant` | `kPreflightSessionSlots` | core/preflight.inc |  |
| `kM12FigInterrupted` | `kFigInterruptedStatus` | core/control.inc |  |
| `kM2BaseClass` | `kSrcBaseClass` | fig/sources.inc |  |
| `kM2DerivedClass` | `kSrcDerivedClass` | fig/sources.inc |  |
| `kM2VTable` | `kSrcVTable` | fig/sources.inc |  |
| `kM40WaitS` | `kStopWaitBoundS` | fig/session_state.inc |  |
| `kM5Sels` | `kMotionSels` | client/motion.inc |  |
| `m10_micIsConnected` | `hook_audioIsConnected` | fig/sources.inc |  |
| `M10CheckSessionDerived` | `PreflightCheckSessionSlots` | core/preflight.inc |  |
| `M10FindLocalSymbol` | `PreflightFindLocalSymbol` | core/preflight.inc |  |
| `M10ForgetSession` | `SessForget` | fig/session_hook.inc |  |
| `m11_lazy_install` | `ActInstallAtLoad` | core/lazy.inc |  |
| `m11_trap` | `ActInstallTrap` | core/lazy.inc |  |
| `m12_coordCapAngle` | `hook_coordinatorCaptureAngle` | client/rotation.inc |  |
| `M12AllSessions` | `SessAll` | core/control.inc |  |
| `M12ApplyOrient` | `OrientApply` | core/control.inc |  |
| `M12ApplyOrientF` | `OrientApplyPendingF` | core/control.inc |  |
| `M12ApplySource` | `SceneApplySource` | providers/scene.inc |  |
| `M12CtlRead` | `CtlReadAtPrepare` | core/control.inc |  |
| `M12DetectBarcodes` | `BarcodeDetectScene` | providers/scene.inc |  |
| `M12DevOrient` | `OrientDevice` | providers/scene.inc |  |
| `M12DrawEAN13` | `SceneDrawEAN13` | providers/scene.inc |  |
| `M12DrawFill` | `SceneDrawFill` | providers/scene.inc |  |
| `M12DrawImg` | `SceneDrawImage` | providers/scene.inc |  |
| `M12DrawScene` | `SceneDrawGenerated` | providers/scene.inc |  |
| `M12EndInterrupt` | `InterruptEnd` | core/control.inc |  |
| `M12Enqueue` | `CtlEnqueue` | core/control.inc |  |
| `M12Event` | `CtlHandleEvent` | core/control.inc |  |
| `M12Flip` | `MirrorFlip` | providers/scene.inc |  |
| `M12Interrupt` | `InterruptBegin` | core/control.inc |  |
| `M12LoadImage` | `SceneLoadImage` | providers/scene.inc |  |
| `M12MRCString` | `BarcodeMRCString` | providers/scene.inc |  |
| `M12OneDAttrs` | `BarcodeOneDAttrs` | providers/scene.inc |  |
| `M12OrientOn` | `OrientVirtualOn` | providers/scene.inc |  |
| `M12ParseOrient` | `OrientParse` | core/control.inc |  |
| `M12PollControl` | `CtlPoll` | core/control.inc |  |
| `M12PreviewNote` | `PreviewNoteFor` | providers/scene.inc |  |
| `M12PreviewR` | `PreviewRotationFor` | client/rotation.inc |  |
| `M12PrimaryEngine` | `ActIsPrimaryEngine` | core/init.inc |  |
| `M12Pt` | `BarcodePoint` | providers/scene.inc |  |
| `M12QR` | `SceneQRImage` | providers/scene.inc |  |
| `M12QRWarm` | `SceneQRWarm` | providers/scene.inc |  |
| `M12QRWarmF` | `SceneQRWarmF` | core/control.inc |  |
| `M12Render` | `SceneRenderSource` | providers/scene.inc |  |
| `M12Resolve` | `CtlResolve` | core/control.inc |  |
| `M12RuntimeError` | `SessInjectRuntimeError` | core/control.inc |  |
| `M12Scene2On` | `SceneGeneratedOn` | providers/scene.inc |  |
| `M12SetSource` | `SceneSetSource` | providers/scene.inc |  |
| `M12Start` | `CtlStart` | core/control.inc |  |
| `M12StopAllRecordings` | `MovieStopAll` | core/control.inc |  |
| `M12StopRec` | `MovieStop` | providers/pump.inc |  |
| `M12Swap` | `CtlSwap` | core/control.inc |  |
| `M12Sym` | `BarcodeSymbol` | providers/scene.inc |  |
| `M12VideoAdvance` | `SceneVideoAdvance` | providers/scene.inc |  |
| `M12VideoClose` | `SceneVideoClose` | providers/scene.inc |  |
| `M12VideoOpen` | `SceneVideoOpen` | providers/scene.inc |  |
| `M12VNSym` | `BarcodeVisionSymbol` | providers/scene.inc |  |
| `M13Ns` | `WcNowNs` | providers/webcam.inc |  |
| `M13WcAdvance` | `WcAdvance` | providers/webcam.inc |  |
| `M13WcApply` | `WcApply` | providers/webcam.inc |  |
| `M13WcBuf` | `WcFrameBuf` | providers/webcam.inc |  |
| `M13WcBufGet` | `WcFrameBufGet` | providers/webcam.inc |  |
| `M13WcBufsFree` | `WcFrameBufsFree` | providers/webcam.inc |  |
| `M13WcCached` | `WcCached` | providers/webcam.inc |  |
| `M13WcCanvas` | `WcCanvas` | providers/webcam.inc |  |
| `M13WcCloseRing` | `WcCloseRing` | providers/webcam.inc |  |
| `M13WcCopy` | `WcCopy` | providers/webcam.inc |  |
| `M13WcCrop` | `WcCrop` | providers/webcam.inc |  |
| `M13WcDrawBand` | `WcDrawBand` | providers/webcam.inc |  |
| `M13WcDropFrame` | `WcDropFrame` | providers/webcam.inc |  |
| `M13WcDue` | `WcFrameDue` | providers/webcam.inc |  |
| `M13WcFree` | `WcFrameBufRelease` | providers/webcam.inc |  |
| `M13WcGpu` | `WcGpu` | providers/webcam.inc |  |
| `M13WcGpuLateEnd` | `WcGpuLateEnd` | providers/webcam.inc |  |
| `M13WcGpuTimeout` | `WcGpuTimeout` | providers/webcam.inc |  |
| `M13WcGraph` | `WcGraph` | providers/webcam.inc |  |
| `M13WcKey` | `WcKey` | providers/webcam.inc |  |
| `M13WcLeave` | `WcLeave` | providers/webcam.inc |  |
| `M13WcMovieOut` | `WcMovieOut` | providers/webcam.inc |  |
| `M13WcOn` | `WcIsOn` | providers/webcam.inc |  |
| `M13WcOp` | `WcOp` | providers/webcam.inc |  |
| `M13WcOut` | `WcRenderOutput` | providers/webcam.inc |  |
| `M13WcOutC` | `WcOutCache` | providers/webcam.inc |  |
| `M13WcOutClear` | `WcOutClear` | providers/webcam.inc |  |
| `M13WcOutFree` | `WcOutFree` | providers/webcam.inc |  |
| `M13WcOutOf` | `WcOutOf` | providers/webcam.inc |  |
| `M13WcPathChoose` | `WcPathChoose` | providers/webcam.inc |  |
| `M13WcPathFree` | `WcPathFree` | providers/webcam.inc |  |
| `M13WcPathName` | `WcPathName` | providers/webcam.inc |  |
| `M13WcPathOn` | `WcPathOn` | providers/webcam.inc |  |
| `M13WcPathReset` | `WcPathReset` | providers/webcam.inc |  |
| `M13WcPaths` | `WcFindHome` | providers/webcam.inc |  |
| `M13WcPhase` | `WcPhaseLock` | providers/webcam.inc |  |
| `M13WcPoolBuf` | `WcPoolBuf` | providers/webcam.inc |  |
| `M13WcPts` | `WcPts` | providers/webcam.inc |  |
| `M13WcReadDefault` | `WcReadDefault` | providers/webcam.inc |  |
| `M13WcReaderStart` | `WcReaderStart` | providers/webcam.inc |  |
| `M13WcReaderStop` | `WcReaderStop` | providers/webcam.inc |  |
| `M13WcReaderWrite` | `WcReaderWrite` | providers/webcam.inc |  |
| `M13WcResolve` | `WcResolve` | providers/webcam.inc |  |
| `M13WcRestorePump` | `WcRestorePump` | providers/webcam.inc |  |
| `M13WcSinkOut` | `WcSinkOut` | providers/webcam.inc |  |
| `M13WcTickEnd` | `WcTickEnd` | providers/webcam.inc |  |
| `M13WcToCG` | `WcToCG` | providers/webcam.inc |  |
| `M13WcVt` | `WcVt` | providers/webcam.inc |  |
| `M13WcWarm` | `WcWarm` | providers/webcam.inc |  |
| `M13WcWhy` | `WcOfflineReason` | providers/webcam.inc |  |
| `m15_multiCamSets` | `hook_supportedMultiCamDeviceSets` | client/photo.inc |  |
| `m15_updBracket` | `hook_updateMaxBracketedCount` | client/photo.inc |  |
| `m15features` | `PhotoResolveUnsupported` | client/photo.inc |  |
| `m16_audioSources` | `hook_audioNewFigCaptureSources` | fig/sources.inc |  |
| `M16Fail` | `ActTestFailIs` | core/act.inc |  |
| `M16ParseEnv` | `ActParseTestEnv` | core/act.inc |  |
| `M16Sleep` | `ActSleepMs` | core/act.inc |  |
| `M16SlowAt` | `ActTestSlowAt` | core/act.inc |  |
| `m2_newFigCaptureSources` | `hook_videoNewFigCaptureSources` | fig/sources.inc |  |
| `M2Attributes` | `SrcAttributes` | fig/sources.inc |  |
| `M2BaseClass` | `BaseObjClass` | fig/sources.inc |  |
| `M2CopySources` | `SrcCopyAll` | fig/sources.inc |  |
| `m2dumpkeys` | `ActDumpKeys` | core/init.inc |  |
| `M2FigResolve` | `ActResolveSourceHooks` | core/init.inc |  |
| `M2Formats` | `SrcFormats` | fig/sources.inc |  |
| `M2LogDict` | `SimcamLogDict` | fig/sources.inc | ObjC class |
| `M2Lookups` | `ActLoadFrameworks` | core/init.inc |  |
| `M2LQMsg` | `LocalQueueMsg` | fig/session_state.inc |  |
| `M2Post` | `SrcPost` | fig/sources.inc |  |
| `M2Preflight` | `PreflightRun` | core/preflight.inc |  |
| `M2SourceStorage` | `SrcStorage` | fig/sources.inc |  |
| `M2VTable` | `BaseObjVTable` | fig/sources.inc |  |
| `M2Wrap` | `SrcWrapLogged` | fig/sources.inc |  |
| `m40_didFinish` | `hook_handleDidFinishCapture` | client/photo.inc |  |
| `m40_willBegin` | `hook_handleWillBeginCapture` | client/photo.inc |  |
| `M40BeginStop` | `StopWaitBeginStop` | fig/session_state.inc |  |
| `M40DelegateQueue` | `StopWaitDelegateQueue` | client/photo.inc |  |
| `M40DidFinishNow` | `StopWaitDidFinishNow` | client/photo.inc |  |
| `M40EndStop` | `StopWaitEndStop` | fig/session_state.inc |  |
| `M40FlushStop` | `StopWaitFlush` | fig/session_state.inc |  |
| `M40Forget` | `StopWaitForget` | fig/session_state.inc |  |
| `M40HasPending` | `StopWaitHasPending` | fig/session_state.inc |  |
| `M40Init` | `StopWaitInit` | fig/session_state.inc |  |
| `M40OnDelegateQueue` | `StopWaitOnDelegateQueue` | fig/session_state.inc |  |
| `M40PendingFor_locked` | `StopWaitPendingFor_locked` | fig/session_state.inc |  |
| `M40PhotoBegin` | `StopWaitPhotoBegin` | fig/session_state.inc |  |
| `M40PhotoDone` | `StopWaitPhotoDone` | fig/session_state.inc |  |
| `m40photostop` | `StopWaitResolveHooks` | client/photo.inc |  |
| `M40SettingsID` | `StopWaitSettingsID` | client/photo.inc |  |
| `M40StillPending_locked` | `StopWaitStillPending_locked` | fig/session_state.inc |  |
| `M40StopQ` | `StopWaitQueue` | fig/session_state.inc |  |
| `M40StopsOf_locked` | `StopWaitStopsOf_locked` | fig/session_state.inc |  |
| `M40WaitPhotoCallbacks` | `StopWaitForPhotos` | fig/session_state.inc |  |
| `m48shutter` | `PhotoResolveShutter` | client/photo.inc |  |
| `M54CrashStart` | `ActTestCrashInStart` | core/lazy.inc |  |
| `M54CrashThread` | `ActTestCrashThread` | core/lazy.inc |  |
| `m5_accActive` | `hook_isAccelerometerActive` | client/motion.inc |  |
| `m5_accData` | `hook_accelerometerData` | client/motion.inc |  |
| `m5_accStart` | `hook_startAccelerometerUpdatesToQueue` | client/motion.inc |  |
| `m5_accStartPull` | `hook_startAccelerometerUpdates` | client/motion.inc |  |
| `m5_accStop` | `hook_stopAccelerometerUpdates` | client/motion.inc |  |
| `M5_APP_DO` | `MOTION_APP_DO` | client/motion.inc | macro |
| `M5_APP_SEND` | `MOTION_APP_SEND` | client/motion.inc | macro |
| `m5_attFrame` | `hook_attitudeReferenceFrame` | client/motion.inc |  |
| `m5_availFrames` | `hook_availableAttitudeReferenceFrames` | client/motion.inc |  |
| `m5_availMOT` | `hook_diagAvailableMetadataObjectTypes` | client/photo.inc |  |
| `M5_CALL_APP` | `MOTION_CALL_APP` | client/motion.inc | macro |
| `m5_capturePhoto` | `hook_capturePhotoWithSettings` | client/photo.inc |  |
| `m5_cgi` | `hook_CGImageRepresentation` | client/photo.inc |  |
| `m5_coordAngle` | `hook_coordinatorPreviewAngle` | client/rotation.inc |  |
| `m5_coordInit` | `hook_coordinatorInit` | client/rotation.inc |  |
| `m5_diagDidStop` | `hook_diagHandleDidStopRunning` | client/photo.inc |  |
| `m5_diagSetRunning` | `hook_diagSetRunning` | client/photo.inc |  |
| `m5_didUpdD` | `hook_previewDidUpdateDouble` | client/rotation.inc |  |
| `m5_didUpdF` | `hook_previewDidUpdateFloat` | client/rotation.inc |  |
| `m5_dmActive` | `hook_isDeviceMotionActive` | client/motion.inc |  |
| `m5_dmData` | `hook_deviceMotion` | client/motion.inc |  |
| `m5_dmStart` | `hook_startDeviceMotionUpdatesToQueue` | client/motion.inc |  |
| `m5_dmStartF` | `MotionStartDeviceMotionPush` | client/motion.inc |  |
| `m5_dmStartPull` | `hook_startDeviceMotionUpdates` | client/motion.inc |  |
| `m5_dmStartPullF` | `MotionStartDeviceMotionPull` | client/motion.inc |  |
| `m5_dmStartRef` | `hook_startDeviceMotionUpdatesUsingReferenceFrameToQueue` | client/motion.inc |  |
| `m5_dmStartRefPull` | `hook_startDeviceMotionUpdatesUsingReferenceFrame` | client/motion.inc |  |
| `m5_dmStop` | `hook_stopDeviceMotionUpdates` | client/motion.inc |  |
| `m5_fdr` | `hook_fileDataRepresentation` | client/photo.inc |  |
| `m5_fdrc` | `hook_fileDataRepresentationWithCustomizer` | client/photo.inc |  |
| `m5_FigCaptureSessionCreate` | `hook_FigCaptureSessionCreate` | fig/session_hook.inc |  |
| `M5_MAXSRC` | `SRC_MAXSOURCES` | fig/sources.inc |  |
| `m5_mrcInit` | `hook_diagMRCInit` | client/photo.inc |  |
| `m5_setMOT` | `hook_diagSetMetadataObjectTypes` | client/photo.inc |  |
| `m5_yes` | `hook_motionAvailable` | client/motion.inc |  |
| `M5Accel` | `SimcamAccelerometerData` | client/motion.inc | ObjC class |
| `M5Adjust` | `SrcPostAdjusting` | fig/sources.inc |  |
| `M5AdoptSession` | `SessAdopt` | fig/session_hook.inc |  |
| `M5AngleFor` | `CoordAngleFor` | providers/scene.inc |  |
| `M5Attitude` | `SimcamAttitude` | client/motion.inc | ObjC class |
| `M5AttitudeFor` | `MotionAttitudeFor` | client/motion.inc |  |
| `M5AudioDeliver` | `MicDeliver` | providers/audio.inc |  |
| `M5AudioSessionPrepare` | `MicAudioSessionPrepare` | providers/audio.inc |  |
| `M5BytesSurface` | `PhotoSurfaceFromBytes` | providers/photo.inc |  |
| `M5Convert` | `SinkConvert` | providers/video.inc |  |
| `M5ConvertScale` | `SinkConvertScale` | providers/video.inc |  |
| `m5coord` | `CoordResolveHooks` | client/rotation.inc |  |
| `M5CoordOrientationChanged` | `CoordOrientationChanged` | client/motion.inc |  |
| `M5CoordPublish` | `CoordPublish` | client/rotation.inc |  |
| `M5Coords` | `CoordTable` | client/motion.inc |  |
| `M5Desc` | `DescribeShort` | fig/session_state.inc |  |
| `M5DetectBarcodes` | `BarcodeDetectFixture` | providers/video.inc |  |
| `m5diag` | `DiagResolveSessionHooks` | client/photo.inc |  |
| `m5diag2` | `DiagResolveMetadataHooks` | client/photo.inc |  |
| `M5DumpProps` | `DiagDumpProps` | fig/session_state.inc |  |
| `M5EncodePhoto` | `PhotoEncode` | providers/photo.inc |  |
| `M5Enqueue` | `MotionEnqueue` | client/motion.inc |  |
| `M5FaultNow` | `TestFaultNullWriteInline` | client/motion.inc |  |
| `M5Gravity` | `MotionGravity` | client/motion.inc |  |
| `M5KVC` | `SafeKVC` | fig/session_state.inc |  |
| `M5MakeAccel` | `MotionMakeAccelerometerData` | client/motion.inc |  |
| `M5MakeMotion` | `MotionMakeDeviceMotion` | client/motion.inc |  |
| `M5Method` | `MotionMethod` | client/motion.inc |  |
| `M5MicCB` | `MicInputCallback` | providers/audio.inc |  |
| `M5MicStart` | `MicStart` | providers/audio.inc |  |
| `M5MicStop` | `MicStop` | providers/audio.inc |  |
| `M5Motion` | `SimcamDeviceMotion` | client/motion.inc | ObjC class |
| `M5MotionClass` | `MotionClass` | client/motion.inc |  |
| `M5MotionFaultCheck` | `MotionTestFaultCheck` | client/motion.inc |  |
| `M5MotionHooks` | `MotionInstallHooks` | client/motion.inc |  |
| `M5MotionStockTraps` | `MotionInstallStockTraps` | client/motion.inc |  |
| `M5MotionTimer` | `MotionTimer` | client/motion.inc |  |
| `M5OrientEarlyFault` | `OrientTestEarlyFault` | client/motion.inc |  |
| `M5OrientFaultCheck` | `OrientTestFaultCheck` | client/motion.inc |  |
| `M5OrientTrack` | `OrientTrack` | client/motion.inc |  |
| `M5OrientTrackRunMain` | `OrientTrackRunMain` | client/motion.inc |  |
| `M5OrientTrackRunMainF` | `OrientTrackRunMainF` | client/motion.inc |  |
| `M5OrientTrackStart` | `OrientTrackStart` | client/motion.inc |  |
| `M5OrientTrackStartForCamera` | `OrientTrackStartForCamera` | client/motion.inc |  |
| `M5OrientUpd` | `OrientUpdate` | client/motion.inc |  |
| `M5PhotoBytes` | `PhotoBytesFromSurface` | client/photo.inc |  |
| `m5photofix` | `PhotoResolveDataFix` | client/photo.inc |  |
| `M5PhotoQ` | `PhotoQueue` | fig/session_state.inc |  |
| `M5PreviewPush` | `PreviewPush` | providers/video.inc |  |
| `m5prevrot` | `PreviewResolveRotation` | client/rotation.inc |  |
| `M5PumpFps` | `PumpFps` | providers/video.inc |  |
| `M5QMatrix` | `MotionQuatMatrix` | client/motion.inc |  |
| `M5QMul` | `MotionQuatMul` | client/motion.inc |  |
| `M5ReadActiveFormat` | `PumpReadActiveFormat` | providers/pump.inc |  |
| `M5Rec` | `SimcamRecording` | fig/session_state.inc | ObjC class |
| `M5RecAppend` | `MovieAppendVideo` | providers/audio.inc |  |
| `M5RecAppendAudio` | `MovieAppendAudio` | providers/audio.inc |  |
| `M5RecFinish` | `MovieFinish` | providers/pump.inc |  |
| `m5Recompute` | `simcam_recomputeAngles` | client/motion.inc | ObjC selector |
| `M5Render` | `SceneRenderFixture` | providers/video.inc |  |
| `M5S` | `SessLookup` | fig/session_state.inc |  |
| `M5SBuf` | `SinkSampleBuffer` | providers/video.inc |  |
| `M5SceneGeomObs` | `SimcamSceneGeometryObserver` | client/motion.inc | ObjC class |
| `M5Sess` | `SimcamSession` | fig/session_state.inc | ObjC class |
| `M5SessPost` | `SessPost` | fig/session_state.inc |  |
| `M5SetAudioFormat` | `AudioSourceSetFormat` | providers/audio.inc |  |
| `M5SetFrame` | `MotionSetFrame` | client/motion.inc |  |
| `M5Sink` | `SimcamSink` | fig/session_state.inc | ObjC class |
| `M5StartPump` | `PumpStart` | providers/pump.inc |  |
| `M5StopTimer` | `MotionStopTimer` | client/motion.inc |  |
| `M5Tick` | `PumpTick` | providers/pump.inc |  |
| `M5UIOrientAny` | `OrientUIAny` | providers/scene.inc |  |
| `M5UIOrientRecord` | `OrientUIRecord` | client/motion.inc |  |
| `M5UIOrientSnapAtLoad` | `OrientUISnapAtLoad` | client/motion.inc |  |
| `M5UIOrientSnapF` | `OrientUISnapF` | client/motion.inc |  |
| `M5UIOrientUnwatch` | `OrientUIUnwatch` | client/motion.inc |  |
| `M5UIOrientWatch` | `OrientUIWatch` | client/motion.inc |  |
| `M5UpdateMic` | `MicUpdate` | providers/audio.inc |  |
| `M5WireSinks` | `PumpWireSinks` | providers/pump.inc |  |
| `m6_createFigCaptureSession` | `hook_createFigCaptureSession` | fig/session_hook.inc |  |
| `M6_MAXGOT` | `SESS_MAXGOT` | fig/session_hook.inc | macro |
| `m6_newAudioFigCaptureSources` | `MicNewFakeSources` | fig/sources.inc |  |
| `M6Deg` | `SettingsRotationDeg` | fig/session_state.inc |  |
| `M6FindFigSession` | `SessFindFig` | fig/session_hook.inc |  |
| `M6FindGOTSlots` | `SessFindGOTSlots` | fig/session_hook.inc |  |
| `M6GOTSwap` | `SessGOTSwap` | fig/session_hook.inc |  |
| `M6IsSimSession` | `SessIsSimulatorSession` | fig/session_hook.inc |  |
| `M6ProcStart` | `ActProcessStartTime` | core/init.inc |  |
| `M6SessResolve` | `SessResolveHook` | fig/session_hook.inc |  |
| `m7_recSettings` | `hook_recommendedOutputSettings` | client/output_settings.inc |  |
| `m7_recSettings6` | `hook_recommendedOutputSettings6` | client/output_settings.inc |  |
| `m7_vnbInit` | `hook_barcodeRequestInit` | client/vision.inc |  |
| `m7_vnbInitCH` | `hook_barcodeRequestInitWithHandler` | client/vision.inc |  |
| `M7AudioSrcWanted` | `AudioSourceWanted` | providers/audio.inc |  |
| `M7FixRecSettings` | `RecSettingsFix` | client/output_settings.inc |  |
| `M7ImageAdded` | `VisionImageAdded` | client/vision.inc |  |
| `M7MicPermitted` | `MicPermitted` | providers/audio.inc |  |
| `M7PinRev` | `VisionPinRevision` | client/vision.inc |  |
| `M7PreviewOfs` | `PreviewOffsetWithoutVirtualOrientation` | client/photo.inc |  |
| `m7recfix` | `RecSettingsResolveFix` | client/output_settings.inc |  |
| `M7Rotate` | `SinkRotate` | providers/video.inc |  |
| `M7ToneQ` | `ToneQueue` | providers/audio.inc |  |
| `M7ToneStart` | `ToneStart` | providers/audio.inc |  |
| `m7visionfix` | `VisionResolveFix` | client/vision.inc |  |
| `M7VisionInstall` | `VisionInstall` | client/vision.inc |  |
| `M7VisionInstallOnce` | `VisionInstallOnce` | client/vision.inc |  |
| `M7VisionStart` | `VisionStart` | client/vision.inc |  |
| `M8Dims` | `DimsFromValue` | fig/session_state.inc |  |
| `M9AddAttributesObject` | `SrcAddAttributesObject` | fig/sources.inc |  |
| `M9CopyStockSources` | `PreflightCopyStockSources` | core/preflight.inc |  |

## Go, shell and files (10)

| kind | old | new | where |
|---|---|---|---|
| file | `cli/internal/verify/e38_test.go` | `cli/internal/verify/handoff_test.go` | moved |
| file | `cli/internal/verify/e40_test.go` | `cli/internal/verify/confirm_fail_test.go` | moved |
| go | `e40Crash` | `resultCrashFail` | cli/internal/verify/confirm_fail_test.go (test constants) |
| go | `e40Fail` | `resultContentFail` | cli/internal/verify/confirm_fail_test.go (test constants) |
| go | `e40Fail2` | `resultContentFail2` | cli/internal/verify/confirm_fail_test.go (test constants) |
| go | `e40Inc` | `resultInconclusive` | cli/internal/verify/confirm_fail_test.go (test constants) |
| go | `e40Pass` | `resultPass` | cli/internal/verify/confirm_fail_test.go (test constants) |
| shell | `E54_CASES` | `TRACE_STOP_CASES` | scripts/activation.sh |
| shell | `PRB` | `MOTION_SUITE` | scripts/activation.sh (an input: `MOTION_SUITE=1`) |
| shell | `PRB_CASES` | `MOTION_CASES` | scripts/activation.sh |

## Log strings, labels and queue names (23)

Text inside engine strings: a log line's run code, a hook label, a dispatch queue label, an exception name. Rows
that are not a whole name replace that exact text in the files named.

| old text | new text | where |
|---|---|---|
| `(m10 finalize wrapper)` | `(the session finalize wrapper)` | core/preflight.inc; PREFLIGHT FAIL reason; no consumer |
| `M11 LAZY armed by` | `LAZY armed by` | core/lazy.inc; log line; consumer by hand: fixtures/motion-probe/summarize.py regex (the scripts grep "LAZY armed by") |
| `@"m12 ` | `@"` | client/motion.inc, core/control.inc, providers/barcodes.inc, providers/photo.inc, providers/scene.inc; log-line prefix; consumers by hand: scripts/bgfg.sh, scripts/webcam-test.sh, scripts/activation/*.sh, fixtures/bgfg-probe/condense.py |
| `"m12.control"` | `"simcam.control"` | core/control.inc; dispatch queue label; no consumer |
| `M16ActivateThrow` | `SimcamTestActivateThrow` | engine/src/core/lazy.inc exception name (events.log exception=, engine log); scripts/activation/failures.sh matches it |
| `m16NoSuchMethod` | `simcamTestNoSuchMethod` | engine/src/core/lazy.inc made-up selector (SIMCAM_TEST_ACTIVATE_FAIL=missing); tools/inventory.py's hooks list skips it (edited by hand) |
| `M16StartThrow` | `SimcamTestStartThrow` | engine/src/core/lazy.inc exception name (events.log exception=, engine log); scripts/activation/failures.sh matches it |
| `M16SwapThrow` | `SimcamTestSwapThrow` | engine/src/core/lazy.inc exception name (events.log exception=, engine log); scripts/activation/failures.sh matches it |
| `M2FakeFigCaptureSource` | `SimcamFigCaptureSource` | engine/src/fig/sources.inc CopyDebugDescription text |
| `M38 first-photo stall` | `SIMCAM_TEST_FIRST_PHOTO_STALL_MS: first-photo stall` | providers/photo.inc; log line names its switch; no consumer |
| `M40 test fault:` | `test fault:` | client/photo.inc; log lines; no consumer. Two of the three then name their switch instead (by hand): SIMCAM_TEST_BUSY_DELEGATE_QUEUE_MS: …, SIMCAM_TEST_FINISH_DELAY_MS: … |
| `"m5.pump"` | `"simcam.pump"` | fig/session_hook.inc; dispatch queue label; no consumer |
| `"m5.session.notify"` | `"simcam.session.notify"` | fig/session_hook.inc; dispatch queue label; no consumer |
| `"m7.tone"` | `"simcam.tone"` | providers/audio.inc; dispatch queue label; no consumer |
| `m8 format read: ` | `active format: ` | providers/pump.inc; log line; no consumer |
| `m8 sourceID` | `active format: sourceID` | providers/pump.inc; log line; no consumer |
| `"simcam.m40fault"` | `"simcam.testfault"` | client/photo.inc; dispatch queue label; no consumer |
| `"w4 e22 bracket"` | `"bracket unsupported"` | client/photo.inc; hook label; shows in docs/inventory.md (generated) |
| `"w4 e22 multicam"` | `"multicam unsupported"` | client/photo.inc; hook label; shows in docs/inventory.md (generated) |
| `w4 e22: ` | `features: ` | client/photo.inc; log lines; no consumer |
| `"w4 e40 ` | `"` | client/photo.inc; hook labels "photo done", "photo queue"; shows in docs/inventory.md (generated) |
| `w4 e40: ` | `photo stop: ` | client/photo.inc; log lines; consumers by hand: scripts/run.sh, scripts/mutation.sh (MISMATCH: photo stop), docs/ledger.md L66, TODO.md |
| `"w4 e48 trace shutter"` | `"trace shutter"` | client/photo.inc; hook label; shows in docs/inventory.md (generated) |

## Not renamed

- Bundle ids (`dev.simcam.spike.m10canary`, `dev.simcam.spike.m10plain`, ...), and the product and file names of
  shipped or fixture build products (`libsimcam*.dylib`, `m10canary.app`, `m5app`, `p12.app`, `m1bcam`).
- Camera profile files (`cfg-m10.plist`, `cfg-m7.plist`, `cfg-m9.plist`), `tools/m2c/`, the `.w4-blocked` marker.
- The fixtures' own inputs and log prefixes: `M5APP_*` and `M5APP| ` (plain), `P12_*` and `P12| ` (plain-orient),
  `M1B|` and `m1b.log` (expo), `M7_TEST`, `M7_AUDIO`, `M7|` (flutter), `M11_OUT`, `M11_MODE`, `M11_MARK` and the
  `M11App` Xcode names (u4diag).
- Every documented `SIMCAM_*` / `STUNTCAM_*` variable, everything the boot stub reads, `SIMCAM_LAZY` and the text `LAZY
  armed by` in the log, ledger ids (`L01`..) and design ids, and the default QR payload `https://simcam.dev/m12-qr`.
