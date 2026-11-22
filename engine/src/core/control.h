// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the control file's internal interface. It declares the control state
// (gControl), which only core/control.inc touches, and the functions other fragments call (core/safety.inc's
// latches, providers/pump.inc, core/lazy.inc).
//
// Who owns what:
//   The control setup is split over activation's phases. The lookups step reads the control file once (atPrepare),
//   the resolve step turns it and the per-launch env into atSwap, SWAP stores those (plain stores only) and START
//   starts the poll. From then on the poll's fields belong to the control queue's timer.
//   orientBeforeForce is touched only on the main queue.
#ifndef SIMCAM_CONTROL_H
#define SIMCAM_CONTROL_H

struct Control {
    long orientBeforeForce;   // the device orientation when forcing began; `auto` goes back to it

    // The control file as the poll last saw it.
    struct {
        NSString *lastEvent;     // the last event= line handled (or seen at start)
        struct timespec mtime;   // the file's mtime at the last read
        BOOL isFirst;            // the first poll: an event already in the file is not replayed
    } poll;

    // Read once in the lookups step, applying nothing. source and event are CFBridgingRetain'd strings.
    struct {
        BOOL read;
        struct timespec mtime;
        void *source;
        void *event;
        long orient;
        BOOL hasOrient;
    } atPrepare;

    // Resolved in the resolve step, stored at SWAP: the first frame after ARMED already uses them.
    struct {
        void *sourceSpec;                // a CFBridgingRetain'd spec, handed to the scene's request at SWAP
        long orientation;                // the forced orientation (0: none)
        const char *orientationReason;   // "control file" or "SIMCAM_ORIENT"
        int photoError;                  // SIMCAM_TEST_PHOTO_ERR's status for the first photo
        BOOL photoErrorSet;
    } atSwap;
};

// ---- functions other fragments call

static void SessInjectRuntimeError(int status, BOOL serverDied);   // core/safety.inc, providers/pump.inc
static void MovieStopAll(int status);                              // core/safety.inc

// The control file's part of each activation step (core/lazy.inc).
static void ControlReadAtPrepare(void);
static void ControlResolve(void);
static void ControlSwap(void);
static void ControlStart(void);
static void ControlEnqueue(void);

#endif
