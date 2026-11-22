// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the frame pump's internal interface. providers/video.inc, the pump's
// frame source, includes it first. It defines the pump's shared state (gPump) and the source canvas's size, and
// declares the functions other fragments call, the pump's session slots among them.
//
// Who owns what:
//   gPump (defined in providers/video.inc) belongs to the pump queue. Each tick renders a frame into the canvas pool
//   (providers/scene.inc) and keeps it as gPump.latest (providers/pump.inc). providers/photo.inc takes that frame for
//   a photo. PumpStart sets gPump.fps.
#ifndef SIMCAM_PUMP_H
#define SIMCAM_PUMP_H

// The source canvas: a 1080p landscape buffer, as a sensor gives it.
static const int kSrcW = 1920;
static const int kSrcH = 1080;

// The pump's frame rate before it is known (the first start), and the default when the device reports none.
#define kPumpDefaultFps 30

struct PumpState {
    CGImageRef fixtureImage;           // the plain scene's picture, loaded with the canvas pool by SceneRenderFixture
    CVPixelBufferPoolRef canvasPool;   // the source canvas's buffers, kSrcW x kSrcH BGRA
    CVPixelBufferRef latest;           // the latest source frame (retained): the frame a photo is made from
    long frameNo;                      // frames rendered so far; it moves the scene's bar and paces the metadata
    double fps;                        // the pump's current frame rate; 0 before the first start
};

// The pump's shared state (pump queue).
static struct PumpState gPump;

// The CAImageQueue functions the opt-in preview path calls (providers/video.inc), resolved by fig/session_hook.inc.
static CFTypeRef (*pCAImageQueueCreate)(uint32_t, uint32_t, uint32_t);
static uint64_t (*pCAImageQueueRegisterIOSurfaceBuffer)(CFTypeRef, IOSurfaceRef, uint32_t);
static bool (*pCAImageQueueInsertImage)(CFTypeRef, double, uint32_t, uint64_t, uint32_t, void *, void *);

// ---- functions other fragments call

// The frame source and the sinks' conversions (providers/video.inc).
static double PumpFps(void);
static CVPixelBufferRef SceneRenderFixture(void);
static CVPixelBufferRef SinkConvert(SimcamSink *sink, CVPixelBufferRef input);
static CMSampleBufferRef SinkSampleBuffer(CVPixelBufferRef frame, CMTime pts);
static void PreviewPush(CMBaseObjectRef session, SimcamSink *sink, CVPixelBufferRef frame);

// The pump and the movie sinks (providers/pump.inc), for core/control.inc.
static void PumpStart(SimcamSession *sess);
static OSStatus MovieStop(CMBaseObjectRef session, CFStringRef sinkID, int stopStatus);

// The session slots defined in providers/pump.inc, which fig/session_hook.inc installs.
static OSStatus sess_SetSectionProperty(CMBaseObjectRef session, CFStringRef section, CFStringRef key, CFTypeRef value);
static OSStatus sess_CopySectionProperty(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                         CFAllocatorRef allocator, CFTypeRef *out);
static OSStatus sess_SetConfiguration(CMBaseObjectRef session, id config);
static OSStatus sess_StartRunning(CMBaseObjectRef session);
static OSStatus sess_StopRunning(CMBaseObjectRef session);
static OSStatus sess_FileStart(CMBaseObjectRef session, CFStringRef sinkID, id settings);
static OSStatus sess_FileStop(CMBaseObjectRef session, CFStringRef sinkID);
static OSStatus sess_FilePause(CMBaseObjectRef session, CFStringRef sinkID);
static OSStatus sess_FileResume(CMBaseObjectRef session, CFStringRef sinkID);

#endif
