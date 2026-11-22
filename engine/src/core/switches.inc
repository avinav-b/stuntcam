// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included right after core/env.inc): the off switches and test faults
// that are read where they act, as named predicates, so the code that acts reads as the behaviour it guards
// ("if (PhotoDrainOn())") instead of a negated environment read.
//
// Provides: one predicate per off switch, SensorLayoutOn to FlushMarkKeptOn (called where the switch acts)
//           TestFaultOn, TestFaultInt, TestFaultLong, for the test faults
// Owns:     nothing.
// Uses:     core/env.inc
//
// Each predicate reads the environment on every call, exactly as the getenv it replaces did: nothing is cached and
// nothing is read earlier. What each switch does is in core/env.inc. An off switch's predicate is named for the
// behaviour the switch turns off and is true while the switch is unset.
//
// Read elsewhere, at the moment their subsystem starts: switches parsed once into globals by ActivationParseTestEnv
// (core/act.inc, at load), SentinelParseTestEnv (core/markers.inc, at load), TraceArm (core/trace.inc, at arming),
// ControlResolveTestFaults (core/control.inc) and StopWaitParseTestFaults (client/photo.inc) at the resolve step, and
// WebcamParseGpuStallTestFault (providers/webcam_path.inc, when the webcam frame path is first chosen); SIMCAM_WD_TEST,
// acted on at once by SentinelStartWatchdogTest (core/sentinel_activate.inc, in START); SIMCAM_TEST_MOVIE_ERR, read
// once into sess_FileStop's static (providers/pump.inc); SIMCAM_SLOT_COUNTS, a list the preflight reads at each run
// (core/preflight.inc); and SIMCAM_OFF_KEY_LOG, also under its stuntcam 1.1.0 name, through EnvConfig
// (fig/sources.inc).

// ---- off switches: true while the switch is unset (the fix is on) ----
// the sensor layout: the scene's top on the buffer's left, video data quarter turns clockwise, preview offset 270,
// the webcam direct path
static BOOL SensorLayoutOn(void) { return getenv(ENV_OFF_SENSOR_LAYOUT) == NULL; }

// video data output frames turned and mirrored as the connection asks
static BOOL DataOutputRotationOn(void) { return getenv(ENV_OFF_DATA_OUTPUT_ROTATION) == NULL; }

// the virtual device orientation (else the buffer stays portrait and the coordinator follows the UI)
static BOOL OrientVirtualOn(void) { return getenv(ENV_OFF_VIRTUAL_ORIENTATION) == NULL; }

// the generated scene (else the plain fixture picture, and no barcode detection)
static BOOL SceneGeneratedOn(void) { return getenv(ENV_OFF_GENERATED_SCENE) == NULL; }

// movies follow the active format's size, rate and codec (else 1080p30 H.264)
static BOOL ActiveFormatOn(void) { return getenv(ENV_OFF_ACTIVE_FORMAT) == NULL; }

// an app's start or stop during an interruption ends the interruption for that session
static BOOL InterruptionResetOn(void) { return getenv(ENV_OFF_INTERRUPTION_RESET) == NULL; }

// a stop or interruption drains the photo queue (and a stop, with StopWaitOn, also waits for the photos' delegate
// callbacks)
static BOOL PhotoDrainOn(void) { return getenv(ENV_OFF_PHOTO_DRAIN) == NULL; }

// a stop is reported after in-flight photos' didFinishCapture has run
static BOOL StopWaitOn(void) { return getenv(ENV_OFF_STOP_WAIT) == NULL; }

// any audio at all (mic or synthetic)
static BOOL AudioOn(void) { return getenv(ENV_OFF_AUDIO) == NULL; }

// photos are encoded on their own queue, not on the frame pump
static BOOL PhotoQueueOn(void) { return getenv(ENV_OFF_PHOTO_QUEUE) == NULL; }

// a photo's size falls back to its outputDimensions, then the format's maximum photo dimensions
static BOOL PhotoDimsOn(void) { return getenv(ENV_OFF_PHOTO_DIMS) == NULL; }

// a photo carries a preview image when its settings ask for one
static BOOL PhotoPreviewOn(void) { return getenv(ENV_OFF_PHOTO_PREVIEW) == NULL; }

// ReadyForResponsiveRequest after each photo (without it AVF never runs didFinishCapture)
static BOOL ResponsiveNoteOn(void) { return getenv(ENV_OFF_RESPONSIVE_NOTE) == NULL; }

// a photo's timestamp is the instant its frame is grabbed (else after the encode)
static BOOL GrabTimestampOn(void) { return getenv(ENV_OFF_GRAB_TIMESTAMP) == NULL; }

// a photo's EXIF capture times are the frame grab's wall clock (else the encode's)
static BOOL GrabExifTimeOn(void) { return getenv(ENV_OFF_GRAB_EXIF_TIME) == NULL; }

// the AVCapturePhoto fileDataRepresentation / CGImageRepresentation fix
static BOOL PhotoDataFixOn(void) { return getenv(ENV_OFF_PHOTO_DATA) == NULL; }

// maxBracketedCapturePhotoCount reads 0 (stuntcam delivers one photo per request)
static BOOL BracketUnsupportedOn(void) { return getenv(ENV_OFF_BRACKET_UNSUPPORTED) == NULL; }

// supportedMultiCamDeviceSets is empty (no format is multi-cam)
static BOOL MultiCamUnsupportedOn(void) { return getenv(ENV_OFF_MULTICAM_UNSUPPORTED) == NULL; }

// the RotationCoordinator hooks
static BOOL RotationCoordinatorFixOn(void) { return getenv(ENV_OFF_ROTATION_COORDINATOR) == NULL; }

// a front preview is mirrored, as on a device
static BOOL PreviewMirrorOn(void) { return getenv(ENV_OFF_PREVIEW_MIRROR) == NULL; }

// the recommendedOutputSettings fix
static BOOL RecommendedSettingsFixOn(void) { return getenv(ENV_OFF_RECOMMENDED_SETTINGS) == NULL; }

// the Vision barcode request revision pin
static BOOL VisionRevisionPinOn(void) { return getenv(ENV_OFF_VISION_REVISION_PIN) == NULL; }

// the notifications answering a SetProperty (adjusting focus/exposure/white balance, completions, torch)
static BOOL SetPropertyNotesOn(void) { return getenv(ENV_OFF_SET_PROPERTY_NOTES) == NULL; }

// the ExposureTargetBias notification
static BOOL BiasNoteOn(void) { return getenv(ENV_OFF_BIAS_NOTE) == NULL; }

// the manual exposure/focus/white balance completion notifications
static BOOL ManualCompletionOn(void) { return getenv(ENV_OFF_MANUAL_COMPLETION) == NULL; }

// only a TorchLevel key posts TorchActive
static BOOL TorchLevelOnlyOn(void) { return getenv(ENV_OFF_TORCH_LEVEL_ONLY) == NULL; }

// the session finalize wrapper (teardown tracked)
static BOOL SessionFinalizeOn(void) { return getenv(ENV_OFF_SESSION_FINALIZE) == NULL; }

// the host control file: read at activation and polled while armed
static BOOL ControlFileOn(void) { return getenv(ENV_OFF_CONTROL_FILE) == NULL; }

// lifecycle and audio-session interruptions
static BOOL InterruptionsOn(void) { return getenv(ENV_OFF_INTERRUPTIONS) == NULL; }

// UIDeviceOrientationDidChangeNotification after a forced orientation
static BOOL OrientationNoteOn(void) { return getenv(ENV_OFF_ORIENTATION_NOTE) == NULL; }

// the CoreMotion stand-in (or, after a latch, its stock traps)
static BOOL MotionStandInOn(void) { return getenv(ENV_OFF_MOTION) == NULL; }

// the thread-specific feature key that attributes stand-in crashes to `motion`
static BOOL MotionKeyOn(void) { return getenv(ENV_OFF_MOTION_KEY) == NULL; }

// CoreMotion goes stock on this build's latch or preflight-fail markers
static BOOL MotionMarkersOn(void) { return getenv(ENV_OFF_MOTION_MARKERS) == NULL; }

// the trace directory is picked when the camera arms (else at the first flush)
static BOOL TraceDirAtArmOn(void) { return getenv(ENV_OFF_TRACE_DIR_AT_ARM) == NULL; }

// a trace write that finds its directory gone picks again, once
static BOOL TraceRepickOn(void) { return getenv(ENV_OFF_TRACE_REPICK) == NULL; }

// only the first thread in the crash handler writes the trace
static BOOL TraceCrashClaimOn(void) { return getenv(ENV_OFF_TRACE_CRASH_CLAIM) == NULL; }

// a start's flush waits until each outstanding stop has queued its DidStopRunning
static BOOL FlushWaitOn(void) { return getenv(ENV_OFF_FLUSH_WAIT) == NULL; }

// a timed-out flush keeps the stops' flush marks until their waiters end
static BOOL FlushMarkKeptOn(void) { return getenv(ENV_OFF_FLUSH_MARK_KEPT) == NULL; }

// ---- test faults read where they act: unset reads as off (0) ----
static BOOL TestFaultOn(const char *name) { return getenv(name) != NULL; }

static int TestFaultInt(const char *name) {
    const char *value = getenv(name);
    return value ? atoi(value) : 0;
}

static long TestFaultLong(const char *name) {
    const char *value = getenv(name);
    return value ? atol(value) : 0;
}
