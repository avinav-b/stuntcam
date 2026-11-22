// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the session layer's internal interface. fig/session_state.inc includes
// it first. It declares the per-session classes, defines the layer's shared state (gSessionLayer, gStopWait) and
// declares the functions other fragments call.
//
// Who owns what:
//   gSessionLayer: fig/session_hook.inc fills the vtables and makes the two queues once, when the hook is installed.
//   The session table is guarded by @synchronized on the table itself.
//   gStopWait: its condition guards the collections and lastStopId. The atomics are set by client/photo.inc's hooks.
#ifndef SIMCAM_SESSION_H
#define SIMCAM_SESSION_H

// ---- the per-session classes

// One output of a session (a sink): a video data output, a preview, a metadata output, a photo or movie output, or
// an audio data output.
@interface SimcamSink : NSObject
@property (copy) NSString *kind;     // vdo, preview, metadata, photo, movie, ado or other (SinkFromConnection)
@property (copy) NSString *sinkID;   // the Fig section ID
@property (assign) CFTypeRef queue;   // the Fig local queue its buffers go to (vdo, metadata, ado)
// the size and pixel format its frames are converted to
@property int width;
@property int height;
@property OSType pixelFormat;
// the conversion's pool and transfer session (SinkConvertScale)
@property (assign) CVPixelBufferPoolRef pool;
@property (assign) VTPixelTransferSessionRef transferSess;
@property float rotation;   // a preview's rotation, degrees
// the opt-in CAImageQueue preview path (SIMCAM_PREVIEW_IMAGE_QUEUE): the queue, and IOSurface ID -> buffer ID
@property (assign) CFTypeRef imageQueue;
@property (strong) NSMutableDictionary *bufferIDsBySurface;
@property int frames;       // frames delivered so far
@property BOOL mirrored;    // a video data output's connection mirrors
// a video data output's connection rotation (degrees clockwise), and the rotation session and pool (SinkRotate)
@property int dataOutputRotationDeg;
@property (assign) VTPixelRotationSessionRef rotationSess;
@property (assign) CVPixelBufferPoolRef rotationPool;
// the mirroring flip (MirrorFlip)
@property (assign) VTPixelRotationSessionRef flipSess;
@property int flipKind;
// the webcam frame path: this sink's pool and last output (providers/webcam_path.inc, WebcamOutputCache)
@property (assign) void *wcOut;
@end

// One movie recording of a movie file output (providers/pump.inc), written with AVAssetWriter.
@interface SimcamRecording : NSObject
@property (copy) NSString *sinkID;
@property long long settingsID;
@property (strong) AVAssetWriter *writer;
@property (strong) AVAssetWriterInputPixelBufferAdaptor *adaptor;
// the first and the latest video frame's presentation time
@property CMTime startPts;
@property CMTime lastPts;
@property BOOL started;         // the writer has started (at the first frame)
@property BOOL stopping;        // no more appends (also set when startWriting fails)
@property BOOL stopRequested;   // MovieStop ran
@property (strong) NSURL *url;
// the movie's size
@property int width;
@property int height;
@property (strong) AVAssetWriterInput *audioInput;
@property int audioBufferCount;   // audio buffers appended so far
@property CMTime audioFirstPts;
@property BOOL audioDone;         // the audio input is marked finished
@property (strong) SimcamSink *converter;   // converts frames to the movie's size (the active format's)
@property BOOL mirror;
@property int rotationDeg;   // the track's rotation, degrees clockwise
// the mirroring flip (MirrorFlip)
@property (assign) VTPixelRotationSessionRef flipSess;
@property int flipKind;
// the webcam frame path: the movie's own pool and last frame (providers/webcam_path.inc, WebcamMovieOutput)
@property (assign) void *wcOut;
@end

// The engine's state for one adopted FigCaptureSession.
@interface SimcamSession : NSObject
@property (assign) CMBaseObjectRef obj;   // the FigCaptureSession (NULL once it is finalized)
@property (strong) id config;             // the configuration last committed (SetConfiguration)
@property BOOL running;
@property (strong) NSMutableArray<SimcamSink *> *sinks;
@property (strong) dispatch_source_t timer;   // the frame pump's timer while running
@property (strong) NSMutableDictionary<NSString *, SimcamRecording *> *recs;   // sink ID -> its recording
@property (strong) NSMutableSet<NSString *> *audioMovieSinks;   // the movie sinks with an audio connection
@property BOOL wantsMic;   // the configuration has an audio connection
@property BOOL micOn;      // the session is one of the audio source's users (MicUpdate)
// from the genuine device's activeFormat and activeVideoMinFrameDuration
@property int formatWidth;
@property int formatHeight;
@property double fps;
// the device position, and interruptions
@property long position;
@property BOOL interrupted;
@property int interruptReason;
// this session's serial stop queue (StopWaitQueue), made at its first deferred stop
@property (strong) dispatch_queue_t stopQueue;
@end

// The signatures of the simulator's derived-class slots that the engine calls through gSessionLayer.originalDerived.
typedef OSStatus (*SessSetSectionPropertyFn)(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                             CFTypeRef value);
typedef OSStatus (*SessCopySectionPropertyFn)(CMBaseObjectRef session, CFStringRef section, CFStringRef key,
                                              CFAllocatorRef allocator, CFTypeRef *out);

// ---- the layer's shared state

// Room for every slot the slot check accepts (fewer than kSessSlotScanLimit, core/preflight.inc) and the version word.
enum { kSessDerivedTableSize = 64 };

struct SessionLayerState {
    // The session vtables, filled once by fig/session_hook.inc.
    void *derived[kSessDerivedTableSize];           // the engine's derived vtable: the simulator's, some slots replaced
    void *originalDerived[kSessDerivedTableSize];   // the simulator's own derived vtable, for calling the originals
    BaseObjVTable vtable;

    // The queues, made by fig/session_hook.inc.
    dispatch_queue_t notifyQueue;   // posts the sessions' notifications (serial, so in order)
    dispatch_queue_t pumpQueue;     // the frame pump and the sinks

    // FigCaptureSession pointer -> its state. Guarded by @synchronized(sessions).
    NSMutableDictionary<NSValue *, SimcamSession *> *sessions;

    // The active format's largest photo size: written by providers/pump.inc, read by providers/photo.inc.
    CMVideoDimensions irisMaxDims;
};

static struct SessionLayerState gSessionLayer;

// CoreMedia's default local notification center, resolved by fig/session_hook.inc.
static void *(*pCMNotificationCenterGetDefaultLocalCenter)(void);

// The photo stop wait (fig/session_state.inc explains it).
struct StopWaitState {
    // Guarded by `cond`. The collections are made once, by StopWaitInit; until then cond is nil.
    NSCondition *cond;
    NSMutableDictionary<NSNumber *, NSValue *> *pending;   // settings ID -> session: photos whose didFinishCapture
                                                           // hasn't run yet
    NSMutableDictionary<NSNumber *, NSValue *> *stops;     // stop id -> session: the deferred stops outstanding
    NSMutableSet<NSNumber *> *flushed;                     // of those, the stop ids a start flushed
    long long lastStopId;                                  // the last stop id handed out

    // Set by client/photo.inc's photo hooks.
    _Atomic int hooked;               // the client hook resolved, so something removes photos from `pending`
    _Atomic int delegateQueuesSeen;   // which photo delegate queues were seen (kStopWaitSaw* bits)
    char delegateQueueKey;            // its address marks every photo delegate queue the hooks have seen

    _Atomic long stopsPosted;         // deferred stops whose DidStopRunning is queued (SIMCAM_TEST_FLUSH_RACE reads it)
};

// gStopWait.delegateQueuesSeen's bits: a photo delegate queue was main, or another queue.
enum { kStopWaitSawMainDelegateQ = 1, kStopWaitSawOtherDelegateQ = 2 };

static struct StopWaitState gStopWait;
static const double kStopWaitBoundS = 0.5;   // the longest a stop waits for its photos (fig/session_state.inc: why)

// A message on a Fig local queue (FigLocalQueueEnqueue), packed as CMCapture lays it out.
#pragma pack(push, 1)
typedef struct {
    int32_t type;   // what obj is (kLocalQueueMsgSampleBuffer)
    CFTypeRef obj;
    int32_t pad[2];
} LocalQueueMsg;
#pragma pack(pop)
// The type field of a message that carries a sample buffer.
enum { kLocalQueueMsgSampleBuffer = 3 };


// ---- functions other fragments call

// Sessions and their notifications (the providers, fig/session_hook.inc, core/control.inc).
static SimcamSession *SessLookup(CMBaseObjectRef session);
static void SessPost(CMBaseObjectRef session, NSString *name, NSDictionary *payload);
static dispatch_queue_t PhotoQueue(void);

// Reading AVF's objects safely (the providers, client/photo.inc, core/control.inc).
static id SafeKVC(id object, NSString *key);
static NSString *DescribeShort(id object, NSUInteger maxLength);
static void DiagDumpProps(id object);
static CMVideoDimensions DimsFromValue(id value);
static int SettingsRotationDeg(id settings, NSString *degKey, NSString *orientKey, long position);

// The photo stop wait (providers/pump.inc, providers/photo.inc, client/photo.inc, fig/session_hook.inc).
static void StopWaitInit(void);
static void StopWaitPhotoBegin(CMBaseObjectRef session, long long settingsID);
static void StopWaitPhotoDone(long long settingsID);
static BOOL StopWaitHasPending(CMBaseObjectRef session);
static BOOL StopWaitOnDelegateQueue(void);
static NSArray<NSNumber *> *StopWaitBeginStop(CMBaseObjectRef session, long long *stopId);
static NSUInteger StopWaitForPhotos(long long stopId, NSArray<NSNumber *> *settingsIDs, double budgetSeconds,
                                    BOOL *flushed);
static void StopWaitEndStop(long long stopId);
static void StopWaitForget(CMBaseObjectRef session);
static dispatch_queue_t StopWaitQueue(CMBaseObjectRef session);
static BOOL StopWaitFlush(CMBaseObjectRef session);

#endif
