// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the switchable sources' and the virtual orientation's internal
// interface. providers/scene.inc includes it first. It defines their state (gScene, gOrientation) and declares the
// functions other fragments call.
//
// Who owns what:
//   gScene.request is written on any thread. core/control.inc makes its lock, and SceneSetSource writes the spec
//   under it; ControlSwap stores the spec unlocked at SWAP, before any session exists. The lock also guards the preview
//   notes (providers/mirror.inc).
//   The rest of gScene belongs to the pump queue. The qr source's code and the scene's QR are made on a background
//   queue and handed back (SceneQRSourceMake, SceneQRWarm).
//   gOrientation is atomic: the main thread, the control file and the motion hooks write it; any thread reads it.
#ifndef SIMCAM_SCENE_H
#define SIMCAM_SCENE_H

// The values of gScene.applied.kind: the source the pump draws.
enum { kSourceScene = 0, kSourceImage = 1, kSourceVideo = 2, kSourceWebcam = 3, kSourceQR = 4 };

struct SceneState {
    // The requested source. A change bumps gen; the pump applies the spec when gen differs from applied.gen.
    struct {
        NSString *spec;                  // guarded by @synchronized(lock)
        NSObject *lock;                  // made by core/control.inc
        _Atomic long gen;
    } request;

    // The applied source.
    struct {
        long gen;                        // the request.gen applied; -1 before the first
        int kind;                        // kSourceScene, kSourceImage, kSourceVideo, kSourceWebcam or kSourceQR
        NSString *path;                  // the image or video file
        CGImageRef stillImage;           // the image source's picture
        CGImageRef qrImage;              // the qr source's code, one pixel per module; NULL while it is being made
    } applied;

    // The video source: the reader, its output, the newest frame and the timing.
    struct {
        AVAssetReader *reader;
        AVAssetReaderOutput *output;
        CGImageRef frame;
        double startTime;
        double nextPTS;
        double firstPTS;
        long frames;
    } video;

    // The generated scene's QR code. It is made on a background queue and stored in `ready`; the pump queue takes it
    // into `image` the first time it draws the scene after that.
    struct {
        CGImageRef image;
        _Atomic(CGImageRef) ready;
    } sceneQR;

    // When barcode detection runs again (providers/barcodes.inc).
    struct {
        _Atomic long gen;                // bumped when the picture changes enough to run barcode detection again
        long lastOrientation;            // the device orientation the barcodes were last scanned in; -1 before any
    } scan;
};

// The sources' state: the request is any thread's, the rest the pump queue's.
static struct SceneState gScene = {.applied = {.gen = -1}, .scan = {.lastOrientation = -1}};

// The virtual device orientation. Device orientations are UIDeviceOrientation values 1 to 4.
struct OrientationState {
    _Atomic long deviceForced;           // set by the environment or the control file; 0 follows the simulator
    _Atomic long deviceSeen;             // the last valid UIDevice orientation, refreshed on the main thread
    _Atomic long ui;   // a copy of gOrientTracker.uiOrientation (client/motion.inc), for readers off the main thread
};

// The virtual device orientation (atomic).
static struct OrientationState gOrientation = {.deviceSeen = 1, .ui = 1};

// ---- functions other fragments call

static CVPixelBufferRef SceneRenderSource(SimcamSession *sess);   // providers/pump.inc, providers/photo.inc
static void SceneSetSource(NSString *spec, const char *why);       // core/control.inc
static void SceneQRWarm(void);                                     // core/control.inc
static long OrientDevice(void);                                    // client/motion.inc, client/rotation.inc
static long OrientUIAny(void);                                     // client/motion.inc, client/rotation.inc

#endif
