// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): photo encode and still-image slots.
//
// Provides: sess_IrisCapture, sess_IrisPrepare, the photo slots (fig/session_hook.inc)
// Owns:     gPhotoErrNext, the next photo's error status (armed by core/control.inc; defined in providers/photo.h)
// Uses:     fig/sources.inc, core/safety.inc, core/trace.inc, fig/session_state.inc, providers/video.inc,
//           providers/scene.inc, providers/mirror.inc
#include "../core/trace.h"
#include "../fig/session.h"
#include "pump.h"
#include "scene.h"
#include "mirror.h"
#include "photo.h"
#include "../core/sentinel.h"

// ---- photo ----------------------------------------------------------------------
// the photo width when neither the settings nor the format give a size and SIMCAM_PHOTO_WIDTH is unset (12 MP at 4:3)
#define kPhotoDefaultWidth 4032
// the watchdog's limit for one photo's encode, seconds
#define kPhotoOpTimeoutS 10.0
// a CMTime timescale of nanoseconds, for the trace's timestamps
#define kNanosecondTimescale 1000000000

// The lens values written into each photo's EXIF, so the metadata looks like a device's. Nothing measures them.
// The focal length and aperture are what an iPhone 15 Pro's main (wide) camera writes: 6.765 mm, about 24 mm in
// full-frame terms, at f/1.78. The exposure is a typical indoor one: 1/60 s at ISO 100.
#define kPhotoExifFocalLengthMm 6.765
#define kPhotoExifFNumber 1.78
#define kPhotoExifExposureTimeS (1.0 / 60)
#define kPhotoExifISO 100

// Encodes one photo with its EXIF and TIFF metadata.
// The capture times are `when`: the wall clock at the frame grab, the instant the photo's timestamp names. They carry
// milliseconds (SubsecTime*) and the UTC offset (OffsetTime*), as a device writes them. The encode's own time would
// be 100-480 ms later.
// SIMCAM_OFF_GRAB_EXIF_TIME=1 stamps them at the encode instead, still with the subseconds.
static NSData *PhotoEncode(CVPixelBufferRef src, int width, int height, NSString *uti, int exifOrientation,
                           NSDictionary **metaOut, NSDate *when) {
    CIImage *image = [CIImage imageWithCVPixelBuffer:src];

    // the source is the 1920x1080 canvas, or the webcam canvas at the ring's size
    if (width > 0 && height > 0) {
        image = [image imageByApplyingTransform:CGAffineTransformMakeScale(
                                                    (double)width / CVPixelBufferGetWidth(src),
                                                    (double)height / CVPixelBufferGetHeight(src))];
    }
    static CIContext *ctx;
    if (!ctx) {
        ctx = [CIContext contextWithOptions:nil];
    }
    CGImageRef cgImage = [ctx createCGImage:image fromRect:image.extent];
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data,
                                                                         (__bridge CFStringRef)uti, 1, NULL);
    if (!destination && ![uti isEqualToString:@"public.jpeg"]) {
        LOG(@"photo: no encoder for %@, using JPEG", uti);
        destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.jpeg"), 1, NULL);
    }

    if (!when || !GrabExifTimeOn()) {
        when = [NSDate date];
    }
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.dateFormat = @"yyyy:MM:dd HH:mm:ss";
    NSString *dateTime = [formatter stringFromDate:when];
    formatter.dateFormat = @"SSS";
    NSString *subsecond = [formatter stringFromDate:when];
    formatter.dateFormat = @"xxx";
    NSString *utcOffset = [formatter stringFromDate:when];
    NSDictionary *exif = @{
        (id)kCGImagePropertyExifDateTimeOriginal: dateTime,
        (id)kCGImagePropertyExifDateTimeDigitized: dateTime,
        (id)kCGImagePropertyExifSubsecTimeOriginal: subsecond,
        (id)kCGImagePropertyExifSubsecTimeDigitized: subsecond,
        (id)kCGImagePropertyExifOffsetTimeOriginal: utcOffset,
        (id)kCGImagePropertyExifOffsetTimeDigitized: utcOffset,
        (id)kCGImagePropertyExifSubsecTime: subsecond,
        (id)kCGImagePropertyExifOffsetTime: utcOffset,
        (id)kCGImagePropertyExifPixelXDimension: @(CGImageGetWidth(cgImage)),
        (id)kCGImagePropertyExifPixelYDimension: @(CGImageGetHeight(cgImage)),
        (id)kCGImagePropertyExifLensMake: @"Apple",
        (id)kCGImagePropertyExifLensModel: @"simcam virtual back camera",
        (id)kCGImagePropertyExifFocalLength: @(kPhotoExifFocalLengthMm),
        (id)kCGImagePropertyExifFNumber: @(kPhotoExifFNumber),
        (id)kCGImagePropertyExifExposureTime: @(kPhotoExifExposureTimeS),
        (id)kCGImagePropertyExifISOSpeedRatings: @[@(kPhotoExifISO)],
    };
    NSDictionary *tiff = @{
        (id)kCGImagePropertyTIFFMake: @"Apple",
        (id)kCGImagePropertyTIFFModel: @"iPhone Simulator (simcam)",
        (id)kCGImagePropertyTIFFOrientation: @(exifOrientation),
        (id)kCGImagePropertyTIFFDateTime: dateTime,
    };
    NSDictionary *meta = @{
        (id)kCGImagePropertyOrientation: @(exifOrientation),
        (id)kCGImagePropertyExifDictionary: exif,
        (id)kCGImagePropertyTIFFDictionary: tiff,
    };
    CGImageDestinationAddImage(destination, cgImage, (__bridge CFDictionaryRef)meta);
    CGImageDestinationFinalize(destination);
    CFRelease(destination);
    CGImageRelease(cgImage);
    if (metaOut) {
        *metaOut = meta;
    }
    return data;
}

// the encoded photo's bytes in a one-row IOSurface (+1), as AVF expects a photo's data
static IOSurfaceRef PhotoSurfaceFromBytes(NSData *bytes) {
    NSDictionary *props = @{(id)kIOSurfaceWidth: @(bytes.length), (id)kIOSurfaceHeight: @1,
                            (id)kIOSurfaceBytesPerElement: @1, (id)kIOSurfacePixelFormat: @(0),
                            (id)kIOSurfaceAllocSize: @(bytes.length)};
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    (void)IOSurfaceLock(surface, 0, NULL);   // a new surface no other thread holds: nothing contends for it
    memcpy(IOSurfaceGetBaseAddress(surface), bytes.bytes, bytes.length);
    IOSurfaceUnlock(surface, 0, NULL);
    return surface;
}

// Slot kSessSlotIrisPrepareToCapture: IrisStillImageSinkPrepareToCapture(session, sinkID, settings), answered at once.
static OSStatus sess_IrisPrepare(CMBaseObjectRef session, CFStringRef sinkID, id settings) {
    TraceRecord(kTraceOpPrepare, [SafeKVC(settings, @"settingsID") longLongValue], 0);
    LOG(@"IrisPrepareToCapture %@ %@", sinkID, DescribeShort(settings, 600));
    SessPost(session, @"IrisPreparationComplete", @{@"SectionID": (__bridge NSString *)sinkID,
                                                    @"SettingsID": @([SafeKVC(settings, @"settingsID") longLongValue]),
                                                    @"ErrorStatus": @0});
    return 0;
}

// The photo's pixel size: the settings' output size, else their outputDimensions, else the format's maximum photo
// dimensions (4032x3024 for 4:3), else kPhotoDefaultWidth (or SIMCAM_PHOTO_WIDTH) wide at the 16:9 canvas's aspect.
// SIMCAM_OFF_PHOTO_DIMS: outputDimensions and the format's maximum are ignored (the output size, else the old 16:9
// size).
static void PhotoOutputSize(id settings, int *widthOut, int *heightOut) {
    int width = [SafeKVC(settings, @"outputWidth") intValue];
    int height = [SafeKVC(settings, @"outputHeight") intValue];
    CMVideoDimensions outDims = DimsFromValue(SafeKVC(settings, @"outputDimensions"));
    if ((width <= 0 || height <= 0) && outDims.width > 0 && PhotoDimsOn()) {
        width = outDims.width;
        height = outDims.height;
    }
    if ((width <= 0 || height <= 0) && gSessionLayer.irisMaxDims.width > 0 && PhotoDimsOn()) {
        width = gSessionLayer.irisMaxDims.width;
        height = gSessionLayer.irisMaxDims.height;
    }
    if (width <= 0 || height <= 0) {
        width = getenv(ENV_PHOTO_WIDTH) ? atoi(getenv(ENV_PHOTO_WIDTH)) : kPhotoDefaultWidth;
        height = width * kSrcH / kSrcW;
    }
    *widthOut = width;
    *heightOut = height;
}

// SIMCAM_TEST_FIRST_PHOTO_STALL_MS=<ms> (a test fault): the process's first photo stalls the frame pump for that long,
// as a lazy encoder setup on the pump queue would; later photos don't. The canary's in-process retry of the frame-gap
// check passes such a regression, so the CLI must report it as a retried pass, never a plain cached one. Runs on the
// pump queue.
static void TestFaultFirstPhotoStall(void) {
    static _Atomic int done;
    int stallMs = TestFaultInt(ENV_TEST_FIRST_PHOTO_STALL_MS);
    if (stallMs > 0 && atomic_exchange(&done, 1) == 0) {
        LOG(@"SIMCAM_TEST_FIRST_PHOTO_STALL_MS: first-photo stall %d ms", stallMs);
        usleep((useconds_t)stallMs * 1000);
    }
}

// IrisWillBeginCapture's payload: the resolved sizes (a preview size of 0: no preview image), no thumbnail, flash, HDR
// or stabilization, and the settings' photo manifest when there is one
static NSDictionary *PhotoWillBeginPayload(NSDictionary *base, id settings, int width, int height, int previewWidth,
                                           int previewHeight) {
    NSMutableDictionary *willBegin = [base mutableCopy];
    [willBegin addEntriesFromDictionary:@{
        @"StillWidth": @(width), @"StillHeight": @(height), @"PreviewWidth": @(previewWidth),
        @"PreviewHeight": @(previewHeight), @"ThumbnailWidth": @0, @"ThumbnailHeight": @0, @"FlashActive": @NO,
        @"HDRActive": @NO, @"SISActive": @NO, @"ErrorStatus": @0}];
    id manifest = SafeKVC(settings, @"photoManifest");
    if (manifest) {
        willBegin[@"PhotoManifest"] = manifest;
    }
    return willBegin;
}

// A photo that completes with errStatus and no image (a photo latch or the photo-error test hook): ends the photo op,
// posts the completion and releases the session. Runs on the photo queue (the pump queue with
// SIMCAM_OFF_PHOTO_QUEUE).
static void PhotoCompleteWithError(CMBaseObjectRef session, NSDictionary *base, long long settingsID, int errStatus,
                                   BOOL latched, int photoOp) {
    NSMutableDictionary *fail = [base mutableCopy];
    fail[@"ErrorStatus"] = @(errStatus);
    LOG(@"photo: completing with ErrorStatus %d (%s)", errStatus, latched ? "photo latch" : "test hook");
    SentinelOpEnd(photoOp);
    SessPost(session, @"IrisStillImageComplete", fail);
    TraceRecord(kTraceOpStillComplete, settingsID, errStatus);
    // SIMCAM_OFF_RESPONSIVE_NOTE: no ReadyForResponsiveRequest (AVF then never runs didFinishCapture)
    if (ResponsiveNoteOn()) {
        SessPost(session, @"ReadyForResponsiveRequest", base);
    }
    // never wait on the pump queue from the photo queue (why: the end of PhotoEncodeAndComplete)
    dispatch_async(gSessionLayer.pumpQueue, ^{
        CFRelease(session);
    });
}

// img mirrored for a photo taken at rotDeg (it consumes img and returns +1), with a flip session of its own
static CVPixelBufferRef PhotoMirrored(CVPixelBufferRef img, int rotDeg) {
    VTPixelRotationSessionRef flipSess = NULL;
    int flipKind = 0;
    CVPixelBufferRef flipped = MirrorFlip(img, rotDeg, &flipSess, &flipKind);
    if (flipSess) {
        VTPixelRotationSessionInvalidate(flipSess);
        CFRelease(flipSess);
    }
    CVPixelBufferRelease(img);
    LOG(@"photo mirrored (rot %d)", rotDeg);
    return flipped;
}

// The preview image (onPreviewImageAvailable / previewCGImageRepresentation): img at width x height in format (+1)
static CVPixelBufferRef PhotoPreviewImage(CVPixelBufferRef img, int width, int height, OSType format) {
    // the converter frees its pool and transfer session (SimcamSink's dealloc)
    SimcamSink *converter = [SimcamSink new];
    converter.width = width;
    converter.height = height;
    converter.pixelFormat = format;
    return SinkConvert(converter, img);
}

// IrisStillImageComplete's payload for an encoded photo: the bytes' surface, metadata and timestamp, the codec and
// file type when given, and the preview image's surface when there is one
static NSDictionary *PhotoCompletePayload(NSDictionary *base, IOSurfaceRef surface, NSUInteger size, NSDictionary *meta,
                                          CMTime pts, unsigned codec, unsigned fileType,
                                          CVPixelBufferRef previewFrame) {
    NSMutableDictionary *complete = [base mutableCopy];
    [complete addEntriesFromDictionary:@{
        @"ErrorStatus": @0, @"Surface": (__bridge id)surface, @"SurfaceSize": @(size), @"Metadata": meta,
        @"PresentationTimestamp": (__bridge_transfer id)CMTimeCopyAsDictionary(pts, NULL)}];
    if (codec) {
        complete[@"PhotoCodec"] = @(codec);
    }
    if (fileType) {
        complete[@"PhotoFileType"] = @(fileType);
    }
    if (previewFrame && CVPixelBufferGetIOSurface(previewFrame)) {
        IOSurfaceRef previewSurface = CVPixelBufferGetIOSurface(previewFrame);
        complete[@"PreviewSurface"] = (__bridge id)previewSurface;
        complete[@"PreviewSurfaceSize"] = @(IOSurfaceGetAllocSize(previewSurface));
    }
    return complete;
}

// One photo request as the capture slot (kSessSlotIrisCaptureImage) resolved it from the settings, carried from the
// capture call to the pump queue (which grabs the frame and fills in the grab's times) and then to the photo queue
// (encode and completion). Each queue writes before it hands the request on (dispatch_async), so no lock is needed.
@interface SimcamPhotoRequest : NSObject
@property (nonatomic) NSString *sinkID;
@property (nonatomic) long long settingsID;   // the settings' uniqueID (Fig SettingsID)
@property (nonatomic) id settings;
// the photo's pixel size (PhotoOutputSize)
@property (nonatomic) int width;
@property (nonatomic) int height;
@property (nonatomic) BOOL previewOn;   // the settings ask for a preview image (SIMCAM_OFF_PHOTO_PREVIEW unset)
@property (nonatomic) int previewWidth;
@property (nonatomic) int previewHeight;
@property (nonatomic) OSType previewFormat;
@property (nonatomic) int rotDeg;   // the output rotation, clockwise degrees
@property (nonatomic) int exifOrientation;
// the settings' outputFormat and outputFileType (0: not given)
@property (nonatomic) unsigned codec;
@property (nonatomic) unsigned fileType;
@property (nonatomic) NSString *uti;   // public.heic for HEVC, else public.jpeg
@property (nonatomic) NSMutableDictionary *base;   // SectionID + SettingsID, the payload every notification starts from
@property (nonatomic) BOOL stampAfterEncode;   // SIMCAM_OFF_GRAB_TIMESTAMP: the timestamp is taken after the encode
// the frame grab: its mach time, and the same instant's wall clock in nanoseconds
@property (nonatomic) uint64_t grabMachTime;
@property (nonatomic) uint64_t grabWallNs;
@end

@implementation SimcamPhotoRequest
@end

// The EXIF orientation for a photo taken at rotDeg (clockwise): it tells a viewer how to turn the stored
// (sensor-landscape) pixels upright.
static CGImagePropertyOrientation PhotoExifOrientation(int rotDeg) {
    switch (rotDeg) {
        case 90: return kCGImagePropertyOrientationRight;   // 6: turn 90° clockwise
        case 180: return kCGImagePropertyOrientationDown;   // 3
        case 270: return kCGImagePropertyOrientationLeft;   // 8: turn 90° counter-clockwise
        default: return kCGImagePropertyOrientationUp;      // 1
    }
}

// what the capture slot's settings ask for, read on the calling thread
static SimcamPhotoRequest *PhotoRequestResolve(CMBaseObjectRef session, NSString *sinkID, id settings,
                                               long long settingsID) {
    SimcamPhotoRequest *req = [SimcamPhotoRequest new];
    req.sinkID = sinkID;
    req.settingsID = settingsID;
    req.settings = settings;
    int width;
    int height;
    PhotoOutputSize(settings, &width, &height);
    req.width = width;
    req.height = height;

    // SIMCAM_OFF_PHOTO_PREVIEW: no preview image with the photo
    req.previewOn = [SafeKVC(settings, @"previewEnabled") boolValue] && PhotoPreviewOn();
    req.previewWidth = [SafeKVC(settings, @"previewWidth") intValue];
    req.previewHeight = [SafeKVC(settings, @"previewHeight") intValue];
    req.previewFormat = [SafeKVC(settings, @"previewFormat") unsignedIntValue] ?: '420f';
    int rotDeg = SettingsRotationDeg(settings, @"outputRotationDegrees", @"outputOrientation",
                                     SessLookup(session).position);
    req.rotDeg = rotDeg;
    req.exifOrientation = PhotoExifOrientation(rotDeg);
    req.codec = [SafeKVC(settings, @"outputFormat") unsignedIntValue];
    req.fileType = [SafeKVC(settings, @"outputFileType") unsignedIntValue];
    BOOL hevc = req.codec == 'hvc1';
    req.uti = hevc ? @"public.heic" : @"public.jpeg";
    return req;
}

// On the pump queue, in order with the frames: the will-begin notifications, the source frame grab (returned +1) and
// its timestamp, then did-capture.
static CVPixelBufferRef PhotoGrabFrame(CMBaseObjectRef session, SimcamPhotoRequest *req) {
    long long settingsID = req.settingsID;
    NSMutableDictionary *base = [@{@"SectionID": req.sinkID, @"SettingsID": @(settingsID)} mutableCopy];
    req.base = base;
    NSDictionary *willBegin = PhotoWillBeginPayload(base, req.settings, req.width, req.height,
                                                    req.previewOn ? req.previewWidth : 0,
                                                    req.previewOn ? req.previewHeight : 0);
    SessPost(session, @"IrisWillBeginCaptureBeforeResolvingSettings", base);
    SessPost(session, @"IrisWillBeginCapture", willBegin);
    SessPost(session, @"IrisWillCaptureStillImage", base);
    TraceRecord(kTraceOpWillCapture, settingsID, 0);

    // the pump's latest frame; before the first tick, one rendered now
    CVPixelBufferRef src;
    if (gPump.latest) {
        src = CVPixelBufferRetain(gPump.latest);
    } else if (SceneGeneratedOn()) {
        src = SceneRenderSource(SessLookup(session));
    } else {
        src = SceneRenderFixture();
    }

    // The photo's timestamp (PresentationTimestamp, which becomes AVCapturePhoto.timestamp) is this instant, when its
    // source frame is taken, as a device's is the sensor frame's: within a few ms of the capture slot's trace mark.
    // After the encode it would be 120-160 ms later for a 12 MP JPEG, too late for an app that matches the photo
    // against sensor data around the shutter. SIMCAM_OFF_GRAB_TIMESTAMP=1 stamps it after the encode.
    req.stampAfterEncode = !GrabTimestampOn();

    // The wall clock too, for the EXIF capture time: one value, computed back from grabMachTime, for both the
    // timestamp op and the EXIF, so a preemption between two clock reads can't put them apart.
    uint64_t grabMachTime = mach_absolute_time();
    uint64_t grabWallNs = TraceWallAt(grabMachTime);
    req.grabMachTime = grabMachTime;
    req.grabWallNs = grabWallNs;
    if (!req.stampAfterEncode) {
        TraceRecordAtWall(kTraceOpTimestamp, settingsID,
                          CMTimeConvertScale(CMClockMakeHostTimeFromSystemUnits(grabMachTime), kNanosecondTimescale,
                                             kCMTimeRoundingMethod_Default)
                              .value,
                          grabMachTime, grabWallNs);
    }
    TestFaultFirstPhotoStall();
    SessPost(session, @"IrisDidCaptureStillImage", base);
    TraceRecord(kTraceOpDidCapture, settingsID, 0);
    return src;
}

// On the photo queue: encodes the grabbed frame img (consumed), with its preview image, and posts the completion; or
// error-completes when photo is latched or the photo-error test hook is armed. Releases the session.
static void PhotoEncodeAndComplete(CMBaseObjectRef session, SimcamPhotoRequest *req, CVPixelBufferRef img) {
    long long settingsID = req.settingsID;
    NSMutableDictionary *base = req.base;

    // The op starts when this photo's encode starts, so in a burst the k-th photo's 10 s doesn't include the k-1
    // encodes before it; it ends before the still-complete is posted (the engine's work only, not the app's
    // delegate).
    int photoOp = SentinelOpBegin("photo", kPhotoOpTimeoutS);
    TraceRecord(kTraceOpEncodeStart, settingsID, 0);

    // Decided per photo, when its encode starts: once photo is latched, every photo still to encode error-completes
    // (including ones requested before the latch); otherwise the test hook's one-shot status, if set.
    BOOL latched = SentinelLatched("photo");
    int errStatus = latched ? kSentinelLatchStatus : atomic_exchange(&gPhotoErrNext, 0);
    if (latched) {
        LOG(@"photo latched: this capture error-completes (%d)", kSentinelLatchStatus);
    }

    if (errStatus) {
        CVPixelBufferRelease(img);
        PhotoCompleteWithError(session, base, settingsID, errStatus, latched, photoOp);
        return;
    }

    NSDictionary *meta = nil;

    // device-like: landscape sensor pixels and EXIF orientation 6 (turn 90° clockwise for a portrait display)
    if (OrientVirtualOn() && [SafeKVC(req.settings, @"outputMirroring") boolValue]) {
        img = PhotoMirrored(img, req.rotDeg);
    }

    // SIMCAM_PHOTO_EXIF_ORIENTATION: a fixed EXIF orientation
    int exifOrientation = getenv(ENV_PHOTO_EXIF_ORIENTATION) ? atoi(getenv(ENV_PHOTO_EXIF_ORIENTATION))
                                                             : req.exifOrientation;
    NSDate *grabTime = [NSDate dateWithTimeIntervalSince1970:req.grabWallNs / 1e9];
    NSData *encoded = PhotoEncode(img, req.width, req.height, req.uti, exifOrientation, &meta, grabTime);

    // the preview image (onPreviewImageAvailable / previewCGImageRepresentation)
    CVPixelBufferRef previewFrame = NULL;
    if (req.previewOn && req.previewWidth > 0 && req.previewHeight > 0) {
        previewFrame = PhotoPreviewImage(img, req.previewWidth, req.previewHeight, req.previewFormat);
    }
    CVPixelBufferRelease(img);
    IOSurfaceRef surface = PhotoSurfaceFromBytes(encoded);
    TraceRecord(kTraceOpEncoded, settingsID, (int64_t)encoded.length);

    // the frame grab's time (PhotoGrabFrame); with SIMCAM_OFF_GRAB_TIMESTAMP, now, after the encode
    uint64_t ptsMach = req.stampAfterEncode ? mach_absolute_time() : req.grabMachTime;
    CMTime pts = CMClockMakeHostTimeFromSystemUnits(ptsMach);
    NSDictionary *complete = PhotoCompletePayload(base, surface, encoded.length, meta, pts, req.codec, req.fileType,
                                                  previewFrame);
    LOG(@"photo encoded %lu bytes as %@ (codec=%.4s ftype=%.4s) %dx%d", (unsigned long)encoded.length, req.uti,
        (char *)&(unsigned){CFSwapInt32HostToBig(req.codec)}, (char *)&(unsigned){CFSwapInt32HostToBig(req.fileType)},
        req.width, req.height);
    SentinelOpEnd(photoOp);
    if (req.stampAfterEncode) {
        TraceRecordAt(kTraceOpTimestamp, settingsID,
                      CMTimeConvertScale(pts, kNanosecondTimescale, kCMTimeRoundingMethod_Default).value, ptsMach);
    }
    SessPost(session, @"IrisStillImageComplete", complete);
    TraceRecord(kTraceOpStillComplete, settingsID, 0);

    // AVF fires captureOutput:didFinishCaptureForResolvedSettings: only after ReadyForResponsiveRequest (it sets the
    // 0x8000 callback flag, then calls -_handlePotentiallyFinalPhotoRequestCallbackWithPayload:). It also drives
    // readyForResponsiveRequestAfterResolvedSettings: and readiness.
    if (ResponsiveNoteOn()) {
        SessPost(session, @"ReadyForResponsiveRequest", base);
    }
    CFRelease(surface);
    if (previewFrame) {
        CVPixelBufferRelease(previewFrame);
    }

    // Never wait on the pump queue from this queue: an interruption waits on this queue from inside a pump-queue block
    // (core/control.inc). A last release here would run the session's finalize, which syncs to the pump queue; so
    // the session is released on the pump queue instead, where finalize runs inline (SessForget).
    dispatch_async(gSessionLayer.pumpQueue, ^{
        CFRelease(session);
    });
}

// Slot kSessSlotIrisCaptureImage: IrisStillImageSinkCaptureImage(session, sinkID, settings). The frame is grabbed on
// the pump queue, then encoded and completed on the photo queue.
static OSStatus sess_IrisCapture(CMBaseObjectRef session, CFStringRef sinkIDRef, id settings) {
    NSString *sinkID = (__bridge NSString *)sinkIDRef;
    long long settingsID = [SafeKVC(settings, @"settingsID") longLongValue];
    TraceRecord(kTraceOpSlot12, settingsID, 0);   // the capture slot's trace mark, before the (long) log line
    LOG(@"IrisCaptureImage %@ settings=%@", sinkID, DescribeShort(settings, 2500));
    DiagDumpProps(settings);
    SimcamPhotoRequest *req = PhotoRequestResolve(session, sinkID, settings, settingsID);

    // pending until AVF has run the delegate's didFinishCapture (a stop waits for that)
    StopWaitPhotoBegin(session, settingsID);
    CFRetain(session);
    dispatch_async(gSessionLayer.pumpQueue, ^{
        CVPixelBufferRef src = PhotoGrabFrame(session, req);
        // The encode, mirroring, the preview image and the completion run on their own serial queue, so a slow encode
        // (7.6 s once on a starved hosted runner) doesn't stall the preview and video data frames on the pump queue.
        // Photos still complete in request order; the early notifications stay on the pump queue, in order with the
        // frames. Each pending photo holds one retained source frame (~8 MB BGRA at 1080p) until its encode; a burst
        // of n holds n of them. SIMCAM_OFF_PHOTO_QUEUE encodes on the pump queue instead, a negative control for the
        // canary's frame-gap check.
        dispatch_async(PhotoQueueOn() ? PhotoQueue() : gSessionLayer.pumpQueue, ^{
            PhotoEncodeAndComplete(session, req, src);
        });
    });

    return 0;
}
