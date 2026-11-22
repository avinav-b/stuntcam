// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): movie audio, synthetic audio source.
//
// Provides: MicUpdate (providers/pump.inc, fig/session_hook.inc, core/control.inc)
//           MovieAppendVideo (providers/pump.inc)
// Owns:     gAudio, the audio source's state, defined in providers/audio.h with which queue owns what
//           gAudio.source.format is also read by providers/pump.inc and client/output_settings.inc
// Uses:     core/preflight.inc, fig/session_state.inc, providers/video.inc,
//           providers/webcam_path.inc, providers/mirror.inc
//
// ---- audio. The simulator FigCaptureSession never captures audio (StartRunning, StopRunning and the FileSink slots
// are one shared stub), so the microphone is captured in-process with an AudioQueue (CoreSimulator routes it to the
// Mac's microphone) and fed to (a) the engine's movie writer's AAC input and (b) audio data sinks (through their local
// queue), all on the host clock like the video frames.
#import <AudioToolbox/AudioToolbox.h>
#include "audio.h"
#include "../fig/session.h"
#include "pump.h"
#include "webcam.h"
#include "mirror.h"
#include "../core/preflight.h"

// Both sources deliver 20 ms buffers (50 a second) of 16-bit samples (2 bytes per mono frame).
enum { kAudioBuffersPerSecond = 50, kAudioBytesPerFrame = 2 };
// gAudio.preroll holds this many buffers: 12 x 20 ms, about 250 ms
enum { kMicRingBuffers = 12 };
// the microphone's AudioQueue cycles this many buffers
enum { kMicQueueBuffers = 6 };
// the microphone's rate when the audio session reports none (or one below 8 kHz)
enum { kMicMinSampleRate = 8000, kMicDefaultSampleRate = 48000 };
// the microphone logs its level once every this many buffers (5 s)
enum { kMicLogEveryBuffers = 250 };
#define kInt16FullScale 32768.0   // the magnitude of the most negative 16-bit sample
static void MicDeliver(CMSampleBufferRef sampleBuffer);   // below

// Sets the audio source's format, for the tone and the microphone alike: 16-bit signed mono PCM at `rate`. It sets
// gAudio.source.format and makes gAudio.source.formatDescription anew. The tone sets it on its queue, the microphone
// on the pump queue.
static void AudioSourceSetFormat(double rate) {
    gAudio.source.format = (AudioStreamBasicDescription){
        .mSampleRate = rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = kAudioBytesPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = kAudioBytesPerFrame,
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 16
    };
    if (gAudio.source.formatDescription) {
        CFRelease(gAudio.source.formatDescription);
    }
    gAudio.source.formatDescription = NULL;
    // on failure formatDescription stays NULL, and the tone and the microphone deliver nothing (they check it)
    (void)CMAudioFormatDescriptionCreate(NULL, &gAudio.source.format, 0, NULL, 0, NULL, NULL,
                                         &gAudio.source.formatDescription);
}

// The AudioQueue's input callback (the microphone's thread): the buffer as a sample buffer on the host clock, to
// MicDeliver; the buffer goes back to the queue unless the queue was stopped.
static void MicInputCallback(void *userData, AudioQueueRef queue, AudioQueueBufferRef buffer,
                             const AudioTimeStamp *timeStamp, UInt32 packetCount,
                             const AudioStreamPacketDescription *packetDescs) {
    UInt32 frames = buffer->mAudioDataByteSize / gAudio.source.format.mBytesPerFrame;

    if (frames && gAudio.source.formatDescription) {
        CMTime pts;
        if (timeStamp && (timeStamp->mFlags & kAudioTimeStampHostTimeValid)) {
            pts = CMClockMakeHostTimeFromSystemUnits(timeStamp->mHostTime);
        } else {
            pts = CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()),
                                 CMTimeMake(frames, (int32_t)gAudio.source.format.mSampleRate));
        }
        if (gAudio.mic.bufferCount == 0) {
            LOG(@"mic first buffer frames=%u hostTimeValid=%d sampleTimeValid=%d lag=%.1f ms", (unsigned)frames,
                timeStamp ? !!(timeStamp->mFlags & kAudioTimeStampHostTimeValid) : -1,
                timeStamp ? !!(timeStamp->mFlags & kAudioTimeStampSampleTimeValid) : -1,
                CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), pts)) * 1000);
        }
        int16_t *samples = buffer->mAudioData;
        for (UInt32 i = 0; i < frames; i++) {
            double level = fabs(samples[i] / kInt16FullScale);
            if (level > gAudio.mic.peak) {
                gAudio.mic.peak = level;
            }
        }
        CMBlockBufferRef blockBuffer = NULL;
        if (!CMBlockBufferCreateWithMemoryBlock(NULL, NULL, buffer->mAudioDataByteSize, NULL, NULL, 0,
                                                buffer->mAudioDataByteSize, kCMBlockBufferAssureMemoryNowFlag,
                                                &blockBuffer)) {
            CMBlockBufferReplaceDataBytes(buffer->mAudioData, blockBuffer, 0, buffer->mAudioDataByteSize);
            CMSampleBufferRef sampleBuffer = NULL;
            CMAudioFormatDescriptionRef format = gAudio.source.formatDescription;
            if (!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, blockBuffer, format, frames, pts, NULL,
                                                                      &sampleBuffer)) {
                MicDeliver(sampleBuffer);
                CFRelease(sampleBuffer);
            }
            CFRelease(blockBuffer);
        }
        if (++gAudio.mic.bufferCount % kMicLogEveryBuffers == 0) {
            // the peak level as dBFS, floored at 1e-9 (-180 dBFS) so silence logs a number
            LOG(@"mic %ld buffers, peak since last %.1f dBFS", gAudio.mic.bufferCount,
                20 * log10(MAX(gAudio.mic.peak, 1e-9)));
            gAudio.mic.peak = 0;
        }
    }

    if (gAudio.mic.queue == queue) {
        (void)AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);   // fails only while the queue stops, with its buffers
    }
}

static void MicAudioSessionPrepare(void) {
    AVAudioSession *audioSession = [AVAudioSession sharedInstance];
    NSError *error = nil;
    if (![audioSession.category isEqualToString:AVAudioSessionCategoryPlayAndRecord] &&
        ![audioSession.category isEqualToString:AVAudioSessionCategoryRecord]) {
        // what AVCaptureSession does on a device when automaticallyConfiguresApplicationAudioSession is YES
        [audioSession setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeVideoRecording
                          options:AVAudioSessionCategoryOptionDefaultToSpeaker |
                                  AVAudioSessionCategoryOptionAllowBluetoothHFP
                            error:&error];
        LOG(@"audio session category -> PlayAndRecord/VideoRecording err=%@", error);
    }
    error = nil;
    [audioSession setActive:YES error:&error];
    LOG(@"audio session active err=%@ rate=%.0f inputs=%@", error, audioSession.sampleRate,
        [audioSession.currentRoute.inputs valueForKey:@"portName"]);
}

// ---- the synthetic audio source (the default). SIMCAM_AUDIO_SOURCE = tone (default) | silence | mic
// the synthetic source: 20 ms buffers of 48 kHz mono
enum { kToneRate = 48000, kToneFrames = 960 };
#define kToneHz 440.0   // the tone's pitch
#define kToneAmplitude 0.1   // of full scale: -20 dBFS
// The tone's state is touched only on its queue (providers/audio.h says why).

static dispatch_queue_t ToneQueue(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        gAudio.tone.queue = dispatch_queue_create("simcam.tone", DISPATCH_QUEUE_SERIAL);
    });
    return gAudio.tone.queue;
}

static int AudioSourceWanted(void) {
    const char *value = getenv(ENV_AUDIO_SOURCE);
    if (!value || !strcmp(value, "tone")) {
        return kAudioSourceTone;
    }
    if (!strcmp(value, "silence")) {
        return kAudioSourceSilence;
    }
    if (!strcmp(value, "mic")) {
        return kAudioSourceMic;
    }
    return kAudioSourceTone;
}

// Starts the synthetic source (mode: kAudioSourceTone or kAudioSourceSilence). It is a timer on the tone's queue that
// delivers every 20 ms buffer that is due.
static void ToneStart(int mode) {
    dispatch_sync(ToneQueue(), ^{
        gAudio.source.kind = mode;
        AudioSourceSetFormat(kToneRate);
        dispatch_queue_t toneQueue = gAudio.tone.queue;
        gAudio.tone.bufferCount = 0;
        gAudio.tone.phase = 0;
        __block CMTime next = CMClockGetTime(CMClockGetHostTimeClock());
        gAudio.tone.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, toneQueue);
        dispatch_source_set_timer(gAudio.tone.timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / kAudioBuffersPerSecond,
                                  NSEC_PER_MSEC);
        dispatch_source_set_event_handler(gAudio.tone.timer, ^{
            // emit every whole 20 ms buffer that is due (this keeps the PTS contiguous and on the host clock)
            CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
            while (CMTimeCompare(CMTimeAdd(next, CMTimeMake(kToneFrames, kToneRate)), now) <= 0) {
                const UInt32 frames = kToneFrames;
                int16_t samples[kToneFrames];
                for (UInt32 i = 0; i < frames; i++) {
                    samples[i] = gAudio.source.kind == kAudioSourceSilence
                                     ? 0
                                     : (int16_t)(kToneAmplitude * INT16_MAX * sin(gAudio.tone.phase));
                    gAudio.tone.phase += 2 * M_PI * kToneHz / kToneRate;
                    if (gAudio.tone.phase > 2 * M_PI) {
                        gAudio.tone.phase -= 2 * M_PI;
                    }
                }
                CMBlockBufferRef blockBuffer = NULL;
                CMSampleBufferRef sampleBuffer = NULL;
                if (!CMBlockBufferCreateWithMemoryBlock(NULL, NULL, sizeof samples, NULL, NULL, 0, sizeof samples,
                                                        kCMBlockBufferAssureMemoryNowFlag, &blockBuffer)) {
                    CMBlockBufferReplaceDataBytes(samples, blockBuffer, 0, sizeof samples);
                    CMTime pts = CMTimeConvertScale(next, kToneRate, kCMTimeRoundingMethod_Default);
                    if (!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, blockBuffer,
                                                                              gAudio.source.formatDescription, frames,
                                                                              pts, NULL, &sampleBuffer)) {
                        MicDeliver(sampleBuffer);
                        CFRelease(sampleBuffer);
                    }
                    CFRelease(blockBuffer);
                }
                next = CMTimeAdd(next, CMTimeMake(kToneFrames, kToneRate));
                if (gAudio.tone.bufferCount++ == 0) {
                    LOG(@"synthetic audio (%s) first buffer",
                        gAudio.source.kind == kAudioSourceSilence ? "silence" : "tone 440 Hz -20 dBFS");
                }
            }
        });
        dispatch_resume(gAudio.tone.timer);
        LOG(@"audio source: synthetic %s 48 kHz mono (no AVAudioSession / mic access)",
            mode == kAudioSourceSilence ? "silence" : "tone");
    });
}

static BOOL MicPermitted(void) {
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    LOG(@"mic authorizationStatus=%ld", (long)status);
    return status == AVAuthorizationStatusAuthorized;
}

// On the pump queue: one more user of the audio source; the first starts it (the tone, silence or the microphone).
static void MicStart(void) {
    if (gAudio.source.users++ > 0 || !AudioOn()) {
        return;   // SIMCAM_OFF_AUDIO: no audio at all
    }
    double startTime = CFAbsoluteTimeGetCurrent();
    if (!gAudio.preroll) {
        gAudio.preroll = [NSMutableArray array];
    }
    int source = AudioSourceWanted();

    // SIMCAM_MIC_IGNORE_PERMISSION: use the microphone even if the app is not authorized for it
    if (source == kAudioSourceMic && !getenv(ENV_MIC_IGNORE_PERMISSION) && !MicPermitted()) {
        LOG(@"SIMCAM_AUDIO_SOURCE=mic but microphone not authorized -> synthetic tone");
        source = kAudioSourceTone;
    }

    if (source != kAudioSourceMic) {
        ToneStart(source);
        return;
    }

    gAudio.source.kind = kAudioSourceMic;
    MicAudioSessionPrepare();
    double rate = [AVAudioSession sharedInstance].sampleRate;
    if (rate < kMicMinSampleRate) {
        rate = kMicDefaultSampleRate;
    }
    AudioSourceSetFormat(rate);
    AudioQueueRef queue = NULL;
    OSStatus status =
        AudioQueueNewInput(&gAudio.source.format, MicInputCallback, NULL, NULL, kCFRunLoopCommonModes, 0, &queue);
    if (status) {
        LOG(@"mic AudioQueueNewInput -> %d (no audio)", (int)status);
        return;
    }

    UInt32 bufferBytes = (UInt32)(rate / kAudioBuffersPerSecond) * kAudioBytesPerFrame;   // 20 ms buffers
    for (int i = 0; i < kMicQueueBuffers; i++) {
        AudioQueueBufferRef buffer;
        if (!AudioQueueAllocateBuffer(queue, bufferBytes, &buffer)) {
            (void)AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);   // a buffer not queued only leaves one fewer
        }
    }

    gAudio.mic.queue = queue;
    gAudio.mic.bufferCount = 0;
    status = AudioQueueStart(queue, NULL);
    LOG(@"mic AudioQueueStart %.0f Hz -> %d (%.1f ms incl. audio session)", rate, (int)status,
        (CFAbsoluteTimeGetCurrent() - startTime) * 1000);
}

// On the pump queue: one user fewer; the last stops the audio source.
static void MicStop(void) {
    if (gAudio.source.users <= 0 || --gAudio.source.users > 0) {
        return;
    }
    dispatch_sync(ToneQueue(), ^{
        if (gAudio.tone.timer) {
            dispatch_source_cancel(gAudio.tone.timer);
            gAudio.tone.timer = nil;
            LOG(@"synthetic audio stopped after %ld buffers", gAudio.tone.bufferCount);
        }
    });
    AudioQueueRef queue = gAudio.mic.queue;
    gAudio.mic.queue = NULL;
    if (queue) {
        // teardown: a queue that won't stop or go away has nothing left to deliver to
        (void)AudioQueueStop(queue, true);
        (void)AudioQueueDispose(queue, true);
        LOG(@"mic stopped after %ld buffers", gAudio.mic.bufferCount);
    }
    @synchronized (gAudio.preroll) {
        [gAudio.preroll removeAllObjects];
    }
}

// On the pump queue: sampleBuffer appended to the recording's audio input, unless it ends before the movie starts.
static void MovieAppendAudio(SimcamRecording *recording, CMSampleBufferRef sampleBuffer) {
    if (!recording.audioInput || !recording.started || recording.audioDone) {
        return;
    }
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    CMTime end = CMTimeAdd(pts, CMSampleBufferGetDuration(sampleBuffer));
    if (CMTimeCompare(end, recording.startPts) <= 0) {
        return;   // entirely before the movie start: dropped
    }
    if (!recording.audioInput.readyForMoreMediaData) {
        return;
    }
    if ([recording.audioInput appendSampleBuffer:sampleBuffer]) {
        if (recording.audioBufferCount++ == 0) {
            recording.audioFirstPts = pts;
            LOG(@"movie first audio buffer at %+.1f ms vs first video frame",
                CMTimeGetSeconds(CMTimeSubtract(pts, recording.startPts)) * 1000);
        }
    } else {
        LOG(@"movie audio append failed %@", recording.writer.error);
    }
}

// On the audio source's thread: sampleBuffer is kept in the pre-roll ring. Then, on the pump queue, it is appended to
// every recording and enqueued on every audio data sink of the sessions that want audio.
static void MicDeliver(CMSampleBufferRef sampleBuffer) {
    CFRetain(sampleBuffer);

    @synchronized (gAudio.preroll) {
        [gAudio.preroll addObject:(__bridge id)sampleBuffer];
        while (gAudio.preroll.count > kMicRingBuffers) {
            [gAudio.preroll removeObjectAtIndex:0];
        }
    }

    dispatch_async(gSessionLayer.pumpQueue, ^{
        NSArray *sessions;
        @synchronized (gAudio.sessions) {
            sessions = gAudio.sessions.allObjects;
        }
        for (SimcamSession *sess in sessions) {
            NSArray *recs;
            @synchronized (sess.recs) {
                recs = sess.recs.allValues;
            }
            for (SimcamRecording *recording in recs) {
                @synchronized (recording) {
                    MovieAppendAudio(recording, sampleBuffer);
                }
            }
            for (SimcamSink *sink in sess.sinks) {
                if ([sink.kind isEqualToString:@"ado"] && sink.queue) {
                    LocalQueueMsg msg = { kLocalQueueMsgSampleBuffer, sampleBuffer, {0, 0} };
                    OSStatus status = pFigLocalQueueEnqueue(sink.queue, &msg, NULL);
                    if (sink.frames++ < 2 || status) {
                        LOG(@"ado %@ audio buffer %d -> %d", sink.sinkID, sink.frames, (int)status);
                    }
                }
            }
        }
        CFRelease(sampleBuffer);
    });
}

// On the pump queue: the audio source runs while a running session has an audio connection.
static void MicUpdate(SimcamSession *sess) {
    BOOL wantsAudio = sess.running && sess.wantsMic;
    if (wantsAudio == sess.micOn) {
        return;
    }
    sess.micOn = wantsAudio;
    if (!gAudio.sessions) {
        gAudio.sessions = [NSHashTable weakObjectsHashTable];
    }
    if (wantsAudio) {
        @synchronized (gAudio.sessions) {
            [gAudio.sessions addObject:sess];
        }
        MicStart();
    } else {
        @synchronized (gAudio.sessions) {
            [gAudio.sessions removeObject:sess];
        }
        MicStop();
    }
}

// whether frame is other than width x height
static BOOL FrameSizeDiffers(CVPixelBufferRef frame, int width, int height) {
    return (int)CVPixelBufferGetWidth(frame) != width || (int)CVPixelBufferGetHeight(frame) != height;
}

// On the pump queue: one video frame is appended to the recording. The first frame starts the writer and appends the
// audio pre-roll.
static void MovieAppendVideo(SimcamRecording *recording, CVPixelBufferRef frame, CMTime pts) {
    if (recording.stopping) {
        return;
    }

    if (!recording.started) {
        if (![recording.writer startWriting]) {
            LOG(@"movie startWriting failed %@", recording.writer.error);
            recording.stopping = YES;
            return;
        }
        [recording.writer startSessionAtSourceTime:pts];
        recording.startPts = pts;
        recording.started = YES;
        if (recording.audioInput) {
            NSArray *preroll;
            @synchronized (gAudio.preroll) {
                preroll = [gAudio.preroll copy];
            }
            for (id buffered in preroll) {
                MovieAppendAudio(recording, (__bridge CMSampleBufferRef)buffered);
            }
        }
    }

    if (recording.adaptor.assetWriterInput.readyForMoreMediaData) {
        // webcam: straight from the ring frame on the GPU when it can (NULL otherwise)
        CVPixelBufferRef movieFrame = WebcamMovieOutput(recording, frame);
        // a webcam canvas at the ring's size
        if (!movieFrame && !recording.converter && FrameSizeDiffers(frame, recording.width, recording.height)) {
            SimcamSink *converter = [SimcamSink new];
            converter.width = recording.width;
            converter.height = recording.height;
            converter.pixelFormat = '420v';
            recording.converter = converter;
        }
        if (!movieFrame) {
            movieFrame = recording.converter ? SinkConvert(recording.converter, frame) : CVPixelBufferRetain(frame);
        }
        if (recording.mirror) {
            VTPixelRotationSessionRef flipSess = recording.flipSess;
            int flipKind = recording.flipKind;
            CVPixelBufferRef flipped = MirrorFlip(movieFrame, recording.rotationDeg, &flipSess, &flipKind);
            recording.flipSess = flipSess;
            recording.flipKind = flipKind;
            CVPixelBufferRelease(movieFrame);
            movieFrame = flipped;
        }
        [recording.adaptor appendPixelBuffer:movieFrame withPresentationTime:pts];
        recording.lastPts = pts;
        CVPixelBufferRelease(movieFrame);
    }
}
