// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the webcam source's internal interface. providers/webcam.inc and
// webcam_path.inc own it; other fragments include it for what they read. It defines the source's and the frame path's
// state (gWebcam, gWebcamPath), declares the types that state is made of, and the functions other fragments call.
//
// Who owns what:
//   gWebcam (providers/webcam.inc) belongs to the pump queue, with two exceptions. The reader file's own queue reads
//   the atomics in gWebcam.readerFile. Core Graphics clears the frame buffers' busy flags on any thread.
//   gWebcamPath (providers/webcam_path.inc) belongs to the pump queue. The gpu warm-up and the gpu waits run on other
//   queues and hand their results back to the pump queue (WebcamPathWarm, WebcamRenderGpu).
#ifndef SIMCAM_WEBCAM_H
#define SIMCAM_WEBCAM_H
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>
#include "../webcam/ring_shm.h"

// ---- sizes the state below is declared with

#define kWcHomeBytes 1024              // $SIMCAM_HOME
#define kWcUdidBytes 128               // the simulator's UDID
#define kWcHomePathBytes 1100          // a path under $SIMCAM_HOME/webcam
#define kWcReaderPathBytes 1300        // the reader file: a kWcHomePathBytes directory, the UDID and the pid
#define kWcPhaseWindow 8               // the phase lock averages this many publish-to-tick offsets
#define kWcFrameBufCount 2             // the CG canvas's reusable frame buffers
#define kWcGpuMaxLate 3                // this many gpu renders over the wait limit within a window: cg for good
#define kWcTurns 4                     // 0, 90, 180 and 270 degrees
#define kWcRotationSessions 8          // one VideoToolbox rotation session per turn, with and without a mirror
#define kWcStatsMinStartMs 1e9         // the stats window's publish-to-tick minimum before its first frame
#define kWcSpecPrefixLen 7             // strlen("webcam:"), the source spec's prefix (providers/scene.inc too)

// The values of gWebcamPath.backend: the frame path's backend in use. kWcPathCg is scene.inc's CG canvas.
enum { kWcPathUnchosen = -1, kWcPathCg = 0, kWcPathVt = 1, kWcPathGpu = 2 };

// ---- the types the state is made of

// Stats for the 5 s "webcam stat" line. They start again after each line. The sums of times are over the new frames;
// the line shows averages.
typedef struct {
    uint64_t sinceNs;              // when this line's interval began
    long ticks;
    long newFrames;
    long repeatedFrames;
    long skippedFrames;
    long tornReads;
    long offlineTicks;
    double copyMs;
    double captureToTickMs;
    double captureToTickMaxMs;
    double publishToTickMs;
    double publishToTickMinMs;     // starts at kWcStatsMinStartMs, so the first frame sets it
    double publishToTickMaxMs;
    double tickMs;                 // tick: from the pump tick's start until the sinks are fed
    long timedTicks;
    long lateFound;                // late frames waited for and found
    long lateMissed;               // late frames waited for and not found
    double lateWaitMs;             // the total wait for the late frames found
    double renderMs;               // frame path renders (providers/webcam_path.inc)
    long renders;
} WebcamStats;

// A reusable frame buffer for the CG canvas. It is busy while a CGImage made from it lives; the image's data provider
// clears the flag when it releases the bytes.
typedef struct {
    uint8_t *bytes;
    size_t len;
    _Atomic int busy;
} WebcamFrameBuffer;

// One output's transform: a turn, then a mirror (VideoToolbox's order), into a width x height buffer.
typedef struct {
    int turn;                  // degrees clockwise: 0, 90, 180 or 270
    int mirror;                // 1: mirror horizontally after the turn
    int width;
    int height;
    OSType format;             // the pixel format
} WebcamOutputOp;

// An output kept for the ring frame it was made from: its pool, and the last buffer with its frame count and op key.
typedef struct {
    CVPixelBufferPoolRef pool;
    int poolWidth;
    int poolHeight;
    OSType poolFormat;
    CVPixelBufferRef lastOutput;
    uint64_t frameGen;         // gWebcam.frame.bufferGen of the frame lastOutput was made from
    uint64_t opKey;            // WebcamOutputOpKey of the op lastOutput was made with
} WebcamOutputCache;

// One tick's frame from the source (WebcamAdvance), all borrowed. At most one of buffer and image is set; with neither,
// the scene shows under the offline band.
typedef struct {
    CVPixelBufferRef buffer;   // the ring frame, for the frame path (providers/webcam_path.inc)
    CGImageRef image;          // the frame as an image, for scene.inc's CG canvas
    NSString *band;            // the offline band's text, or nil
    BOOL rescan;               // a new frame of a moving picture: barcode detection reruns
} WebcamTickFrame;

// ---- the source's state (gWebcam, providers/webcam.inc)

struct WebcamState {
    // The selected camera. Set when the source is applied.
    struct {
        BOOL on;                   // webcam is the applied source
        NSString *fromSpec;        // the uniqueID the spec names; nil means the default camera
        NSString *inUse;           // the uniqueID in use
    } camera;

    // The ring: where it is, the reader mapped onto it, and how far this engine has read it.
    struct {
        char home[kWcHomeBytes];   // $SIMCAM_HOME, spelled as the CLI and the helper spell it
        char udid[kWcUdidBytes];   // this simulator's UDID, safe for a file name
        scw_shm_reader reader;     // the ring library's reader (webcam/ring_shm.h)
        BOOL open;                 // reader names a ring (scw_shm_reader_init ran)
        uint64_t nextResolveNs;    // while offline, the ring's name is resolved again from this time on
        uint64_t epoch;            // the ring's epoch: a new one (a new helper) restarts the measurements
        uint64_t lastSeq;          // the sequence number of the newest frame read
    } ring;

    // The newest frame. It is a CGImage for scene.inc's canvas, or a pixel buffer for the frame path
    // (webcam_path.inc). bufferGen counts the pixel buffers, so outputs made from one can be reused.
    struct {
        CGImageRef image;
        CVPixelBufferRef buffer;
        uint64_t bufferGen;
    } frame;

    // The camera feed: live or offline.
    struct {
        BOOL live;
        BOOL everLive;             // the ring was live since the source was applied
        BOOL hadFrame;             // a frame was read since the source was applied
        uint64_t offlineSinceNs;
        NSString *bandText;        // the offline band's reason, kept while it doesn't change
        int offlineStatus;         // the reader's open result last seen offline: a new SCW_E_LIMIT logs its reason
    } feed;

    // This tick's timing.
    struct {
        uint64_t ptsNs;            // this tick's presentation time
        uint64_t lagNs;            // the tick time less the capture time of the newest frame
    } tick;

    // The phase lock: the pump timer it re-armed, when, at what period, and the last kWcPhaseWindow publish-to-tick
    // offsets.
    struct {
        void *timer;               // the session timer it re-armed; NULL: no timer is locked
        uint64_t rearmedAtNs;
        uint64_t periodNs;
        int64_t offsets[kWcPhaseWindow];
        int offsetCount;
        long rearms;               // re-arms so far (the stat line shows the running total)
    } phaseLock;

    // The camera's measured interval: the frames and publish times since the lock began.
    struct {
        uint64_t baseSeq;
        uint64_t basePublishNs;
        uint64_t lastPublishNs;    // the newest frame's publish time
    } rate;

    WebcamStats stats;

    WebcamFrameBuffer frameBufs[kWcFrameBufCount];

    // The reader file. Its timer runs on its own queue; the atomics carry the pump queue's state to it.
    struct {
        dispatch_queue_t queue;
        dispatch_source_t timer;
        char path[kWcReaderPathBytes];
        _Atomic int sourceOn;              // camera.on, for the reader file's queue
        _Atomic uint64_t lastTickNs;       // when the pump last ticked with this source
        _Atomic uint64_t wantedFormat;     // the session's format, packed as the kWcWant* constants describe
    } readerFile;
};

// The source's state: the pump queue's, except the reader file's atomics.
static struct WebcamState gWebcam = {.stats = {.publishToTickMinMs = kWcStatsMinStartMs}};

// ---- the frame path's state (gWebcamPath, providers/webcam_path.inc)

struct WebcamPathState {
    // The backend. The warm-up and the gpu waits run on other queues, as WebcamPathWarm and WebcamRenderGpu say.
    int backend;                   // kWcPath*; kWcPathUnchosen until the path is first asked for
    CIContext *ciContext;          // gpu: the context, once WebcamPathWarm has made it
    BOOL warming;
    dispatch_queue_t gpuWaitQueue;

    // Late gpu renders. One that timed out holds the path at cg until it ends.
    struct {
        BOOL running;                      // a gpu render timed out and is still running
        uint64_t timesNs[kWcGpuMaxLate];   // when the last timeouts were (a ring buffer)
        long count;
    } lateRender;

    // The GPU stall test hook (SIMCAM_TEST_WEBCAM_GPU_STALL), and the count of gpu renders it compares with.
    struct {
        long from;                 // -1: the hook is not set
        long ms;
        long count;                // -1: every render from `from` on
        long renders;
    } testStall;

    // The pool the ring frames are copied into.
    struct {
        CVPixelBufferPoolRef pool;
        int width;
        int height;
    } framePool;

    // This tick's canvas.
    struct {
        WebcamOutputCache cache;   // the canvas, made from frame gWebcam.frame.bufferGen
        WebcamOutputOp op;
        BOOL direct;               // this tick's canvas came from the frame path: sinks render from the frame buffer
        int angle;                 // the capture angle the canvas was turned by
    } canvas;

    // VideoToolbox, the vt backend.
    struct {
        VTPixelRotationSessionRef rotations[kWcRotationSessions];
        VTPixelTransferSessionRef transfer;
        CVPixelBufferPoolRef turnedPool[2];   // the turned frame: as is, or with width and height swapped
        int turnedPoolWidth[2];
        int turnedPoolHeight[2];
    } vt;
};

// The frame path's state (pump queue).
static struct WebcamPathState gWebcamPath = {.backend = kWcPathUnchosen, .testStall = {.from = -1, .count = -1}};

// ---- functions other fragments call

// providers/scene.inc: switching to and from the source, each tick's frame, the canvas, the offline band.
static void WebcamApply(NSString *spec);
static void WebcamLeave(void);
static void WebcamRestorePump(SimcamSession *sess);
static WebcamTickFrame WebcamAdvance(SimcamSession *sess);
static CVPixelBufferRef WebcamCanvas(SimcamSession *sess, CVPixelBufferRef frame, int angle);
static void WebcamDrawOfflineBand(CGContextRef ctx, double width, double height, NSString *text);

// providers/pump.inc, providers/audio.inc and fig/session_state.inc: the tick's clock and the sinks' outputs.
static BOOL WebcamIsOn(void);
static uint64_t WebcamNowNs(void);
static CMTime WebcamPts(void);
static void WebcamTickEnd(uint64_t tickStart);
static CVPixelBufferRef WebcamSinkOutput(SimcamSink *sink, BOOL preview);
static CVPixelBufferRef WebcamMovieOutput(SimcamRecording *recording, CVPixelBufferRef canvas);
static void WebcamOutputCacheFree(void *cache);

// Between providers/webcam.inc and providers/webcam_path.inc.
static const char *WebcamPathName(int path);
static BOOL WebcamPathOn(void);
static void WebcamPathWarm(void);
static int WebcamCopyFrame(uint64_t lastSeq, scw_slot_read *slotRead, CVPixelBufferRef *out);
static void WebcamPathReset(void);
static void WebcamPathFree(void);
static void WebcamFrameBuffersFree(void);

#endif
