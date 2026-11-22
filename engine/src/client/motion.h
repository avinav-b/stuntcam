// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the CoreMotion stand-in's and the orientation tracker's internal
// interface. client/motion.inc includes it first. It defines their state (gMotion, gOrientTracker) and declares the
// functions core/lazy.inc calls.
//
// Who owns what:
//   gMotion: the hooks and the stock traps go in at load (before any other engine thread runs). firstTickNs and
//   stockNoted are atomics that any motion thread may set first. core/lazy.inc reads installedAt and stockTrapCount
//   for the armed line.
//   gOrientTracker: everything is main-queue only except `started`, an atomic any thread may set first (the tracker
//   starts once, whoever comes first). Readers off the main thread read gOrientation.ui (providers/scene.h), which
//   every writer of uiOrientation stores too.
#ifndef SIMCAM_MOTION_H
#define SIMCAM_MOTION_H

@class SimcamSceneGeometryObserver;

struct Motion {
    _Atomic uint64_t firstTickNs;   // the first motion tick (SIMCAM_TEST_MOTION_FAULT faults 1 s after it)
    int hookedCount;                // the stand-in's hooks installed at load
    double installedAt;             // when the hooks went in at load (0 = not installed)
    _Atomic int stockNoted;         // a stock trap queued the motion-stock line
    int stockTrapCount;             // the stock traps installed at load (CoreMotion stock this launch)
};

struct OrientTracker {
    volatile long uiOrientation;   // UIInterfaceOrientation (portrait until read)
    BOOL running;                  // the tracker's timer and notifications are on
    _Atomic int started;           // OrientTrackStart ran (at most once)

    // The UI orientation snapshot at load, until the tracker starts.
    struct {
        char note[256];   // how the snapshot went, logged when the tracker starts
        id sceneObserver;   // the one-shot scene-connection observer
        SimcamSceneGeometryObserver *geometryObserver;   // the KVO observer of the scene's effectiveGeometry
    } uiSnapshot;
};

static struct Motion gMotion;
static struct OrientTracker gOrientTracker = {.uiOrientation = UIInterfaceOrientationPortrait};

// ---- functions other fragments call

// Load and activation (core/lazy.inc): the stand-in's hooks or the stock traps, and the orientation tracker.
static void MotionInstallHooks(void);
static void MotionInstallStockTraps(void);
static void OrientUISnapAtLoad(void);
static void OrientTrackRunMain(void);
static void OrientTrackStartForCamera(void);

#endif
