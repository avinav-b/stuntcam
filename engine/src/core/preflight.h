// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the preflight's internal interface. core/preflight.inc includes it first.
// It names the session vtable's slots, defines the CoreMedia queue and session functions the preflight's lookups
// resolve (the session hook and the providers call them) and the slot count it found, and declares the functions other
// fragments call.
//
// Who owns what: the preflight runs in PREPARE, before any session exists, and sets the pointers and gSessSlots once;
// the session hook and the providers only read them after that.
#ifndef SIMCAM_PREFLIGHT_H
#define SIMCAM_PREFLIGHT_H

// ---- the session vtable's slots

// The simulator session class's derived vtable (the derivedClass of _kFigCaptureSessionSimulatorVTable): word 0 is the
// class version, then one function per slot. fig/session_hook.inc replaces the named slots below, and
// PreflightCheckSessionSlots checks each named slot's symbol name before anything is replaced. Slots 10 and 11 are
// neither replaced nor checked. Known layouts: 19 slots (17.5, 26.3) and 24 slots (27.0: slots 19-22 are NULL, 23 is
// VisionDataSinkTriggerBurst and 24 MetadataSinkCaptureFaceIDBracket).
enum {
    kSessSlotVersion = 0,                   // not a function: the derived class's version
    kSessSlotSetSectionProperty = 1,        // sets a property of one section (sink) of the session
    kSessSlotCopySectionProperty = 2,       // reads a section's property (a movie's recorded duration and size)
    kSessSlotSetConfiguration = 3,          // commits a new configuration: the session's sinks and connections
    kSessSlotStartRunning = 4,              // starts the session
    kSessSlotStopRunning = 5,               // stops the session
    kSessSlotFileSinkStartRecording = 6,    // a movie file output starts a recording
    kSessSlotFileSinkStopRecording = 7,     // a movie file output stops its recording
    kSessSlotFileSinkPauseRecording = 8,    // a movie file output pauses its recording
    kSessSlotFileSinkResumeRecording = 9,   // a movie file output resumes its recording
    kSessSlotIrisCaptureImage = 12,         // a photo output captures a photo
    kSessSlotIrisPrepareToCapture = 13,     // a photo output prepares for a capture with given settings
    kSessNamedSlotCount = 14,               // slots 0..13: the ones kPreflightSessionSlots names
};

// The slot check reads at most this many words of the derived vtable: well above the known 19 and 24 slots, and
// below the size of the engine's copies (gSessionLayer.derived, gSessionLayer.originalDerived).
enum { kSessSlotScanLimit = 40 };
// The CMBaseObject class versions the engine was written against: the base class (BaseObjClass.version) and the
// derived class (its word 0).
enum { kFigBaseClassVersion = 2, kFigDerivedClassVersion = 1 };

// ---- what the lookups resolve

// The CoreMedia local queue (providers/pump.inc, providers/audio.inc) and the session factory (fig/session_hook.inc).
static OSStatus (*pFigLocalQueueCreate)(CFAllocatorRef, long, CFTypeRef *);
static OSStatus (*pFigLocalQueueEnqueue)(CFTypeRef, void *, void *);
static OSStatus (*pFigCaptureSessionCreate)(CFAllocatorRef, void *, void *, CMBaseObjectRef *);
static int gSessSlots;   // the number of derived slots (not counting the version word); 0: unknown

// ---- functions other fragments call

static BOOL PreflightRun(void);   // core/lazy.inc
// The session slot check, which fig/session_hook.inc runs again on the session class it adopts.
static NSString *PreflightCheckSessionSlots(const void *const *derived, int *slotsOut);
// The load-command walk (core/crash_handler.inc, fig/session_hook.inc).
static void MachOForEachCommand(const struct mach_header_64 *header, BOOL (^visit)(const struct load_command *command));

#endif
