// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): movie audio, synthetic audio source.

// ---- audio. The simulator FigCaptureSession never captures audio (StartRunning/StopRunning/FileSink* are one shared
// stub), so the mic is captured in-process with an AudioQueue (CoreSimulator routes it to the Mac microphone) and fed
// to (a) our movie writer's AAC input and (b) audio data sinks (LocalQueue), all on the host clock like the video
// frames.
#import <AudioToolbox/AudioToolbox.h>
static AudioQueueRef gAQ;
static int gMicUsers;
static AudioStreamBasicDescription gMicASBD;
static CMAudioFormatDescriptionRef gMicFmt;
// last ~250 ms of CMSampleBuffers (pre-roll for a recording that starts mid-buffer)
static NSMutableArray *gMicRing;
static NSHashTable *gMicSessions;           // running SimcamSession that want audio
static long gMicBufs;
static double gMicPeak;
static void MicDeliver(CMSampleBufferRef sb);
// The audio source's format, for the tone and the mic alike: 16-bit signed mono PCM at `rate` (gMicASBD, and gMicFmt
// made anew). The tone sets it on gToneQ, the mic on the pump queue.
static void AudioSourceSetFormat(double rate) {
    gMicASBD = (AudioStreamBasicDescription){ .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, .mBytesPerPacket = 2,
        .mFramesPerPacket = 1, .mBytesPerFrame = 2, .mChannelsPerFrame = 1, .mBitsPerChannel = 16 };
    if (gMicFmt) CFRelease(gMicFmt);
    gMicFmt = NULL;
    CMAudioFormatDescriptionCreate(NULL, &gMicASBD, 0, NULL, 0, NULL, NULL, &gMicFmt);
}
static void MicInputCallback(void *ud, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts, UInt32 np,
                    const AudioStreamPacketDescription *pd) {
    UInt32 frames = b->mAudioDataByteSize / gMicASBD.mBytesPerFrame;
    if (frames && gMicFmt) {
        CMTime pts;
        if (ts && (ts->mFlags & kAudioTimeStampHostTimeValid)) pts = CMClockMakeHostTimeFromSystemUnits(ts->mHostTime);
        else pts = CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()),
                                  CMTimeMake(frames, (int32_t)gMicASBD.mSampleRate));
        if (gMicBufs == 0)
            LOG(@"mic first buffer frames=%u hostTimeValid=%d sampleTimeValid=%d lag=%.1f ms", (unsigned)frames,
                ts ? !!(ts->mFlags & kAudioTimeStampHostTimeValid) : -1,
                ts ? !!(ts->mFlags & kAudioTimeStampSampleTimeValid) : -1,
                CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), pts)) * 1000);
        int16_t *samples = b->mAudioData;
        for (UInt32 i = 0; i < frames; i++) {
            double v = fabs(samples[i] / 32768.0);
            if (v > gMicPeak) gMicPeak = v;
        }
        CMBlockBufferRef bb = NULL;
        if (!CMBlockBufferCreateWithMemoryBlock(NULL, NULL, b->mAudioDataByteSize, NULL, NULL, 0, b->mAudioDataByteSize,
                                                kCMBlockBufferAssureMemoryNowFlag, &bb)) {
            CMBlockBufferReplaceDataBytes(b->mAudioData, bb, 0, b->mAudioDataByteSize);
            CMSampleBufferRef sb = NULL;
            if (!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, gMicFmt, frames, pts, NULL, &sb)) {
                MicDeliver(sb);
                CFRelease(sb);
            }
            CFRelease(bb);
        }
        if (++gMicBufs % 250 == 0) {
            LOG(@"mic %ld buffers, peak since last %.1f dBFS", gMicBufs, 20 * log10(MAX(gMicPeak, 1e-9)));
            gMicPeak = 0;
        }
    }
    if (gAQ == q) AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static void MicAudioSessionPrepare(void) {
    AVAudioSession *as = [AVAudioSession sharedInstance];
    NSError *e = nil;
    if (![as.category isEqualToString:AVAudioSessionCategoryPlayAndRecord] &&
        ![as.category isEqualToString:AVAudioSessionCategoryRecord]) {
        // what AVCaptureSession does on a device when automaticallyConfiguresApplicationAudioSession is YES
        [as setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeVideoRecording
                options:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothHFP
                  error:&e];
        LOG(@"audio session category -> PlayAndRecord/VideoRecording err=%@", e);
    }
    e = nil;
    [as setActive:YES error:&e];
    LOG(@"audio session active err=%@ rate=%.0f inputs=%@", e, as.sampleRate,
        [as.currentRoute.inputs valueForKey:@"portName"]);
}
// ---- synthetic audio source (default). SIMCAM_AUDIO_SOURCE = tone (default) | silence | mic
enum { kAudioSrcTone = 0, kAudioSrcSilence = 1, kAudioSrcMic = 2 };   // the values of gAudioSrc
// the synthetic source: 20 ms buffers of 48 kHz mono
enum { kToneRate = 48000, kToneFrames = 960 };
static dispatch_source_t gToneTimer;
static double gTonePhase;
static long gToneBufs;
static int gAudioSrc;   // kAudioSrcTone, kAudioSrcSilence or kAudioSrcMic
// The tone state (timer, counters, gMicFmt) is only touched on gToneQ: start and stop run there synchronously, so a
// stop/start can't free gMicFmt under a still-running timer handler, and the counter read is not a race (TSan).
static dispatch_queue_t gToneQ;
static dispatch_queue_t ToneQueue(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{ gToneQ = dispatch_queue_create("simcam.tone", DISPATCH_QUEUE_SERIAL); });
    return gToneQ;
}
static int AudioSourceWanted(void) {
    const char *e = getenv(ENV_AUDIO_SOURCE);
    if (!e || !strcmp(e, "tone")) return kAudioSrcTone;
    if (!strcmp(e, "silence")) return kAudioSrcSilence;
    if (!strcmp(e, "mic")) return kAudioSrcMic;
    return kAudioSrcTone;
}
static void ToneStart(int mode) {
    dispatch_sync(ToneQueue(), ^{
        gAudioSrc = mode;
        AudioSourceSetFormat(kToneRate);
        dispatch_queue_t tq = gToneQ;
        gToneBufs = 0;
        gTonePhase = 0;
        __block CMTime next = CMClockGetTime(CMClockGetHostTimeClock());
        gToneTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, tq);
        dispatch_source_set_timer(gToneTimer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 50, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(gToneTimer, ^{
            // emit every whole 20 ms buffer that is due (keeps PTS contiguous and on the host clock)
            CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
            while (CMTimeCompare(CMTimeAdd(next, CMTimeMake(kToneFrames, kToneRate)), now) <= 0) {
                const UInt32 frames = kToneFrames;
                int16_t buf[kToneFrames];
                for (UInt32 i = 0; i < frames; i++) {
                    buf[i] = gAudioSrc == kAudioSrcSilence ? 0 : (int16_t)(0.1 * 32767 * sin(gTonePhase));
                    gTonePhase += 2 * M_PI * 440.0 / 48000.0;
                    if (gTonePhase > 2 * M_PI) gTonePhase -= 2 * M_PI;
                }
                CMBlockBufferRef bb = NULL;
                CMSampleBufferRef sb = NULL;
                if (!CMBlockBufferCreateWithMemoryBlock(NULL, NULL, sizeof buf, NULL, NULL, 0, sizeof buf,
                                                        kCMBlockBufferAssureMemoryNowFlag, &bb)) {
                    CMBlockBufferReplaceDataBytes(buf, bb, 0, sizeof buf);
                    CMTime pts = CMTimeConvertScale(next, kToneRate, kCMTimeRoundingMethod_Default);
                    if (!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, gMicFmt, frames, pts, NULL,
                                                                             &sb)) {
                        MicDeliver(sb);
                        CFRelease(sb);
                    }
                    CFRelease(bb);
                }
                next = CMTimeAdd(next, CMTimeMake(kToneFrames, kToneRate));
                if (gToneBufs++ == 0)
                    LOG(@"synthetic audio (%s) first buffer",
                        gAudioSrc == kAudioSrcSilence ? "silence" : "tone 440 Hz -20 dBFS");
            }
        });
        dispatch_resume(gToneTimer);
        LOG(@"audio source: synthetic %s 48 kHz mono (no AVAudioSession / mic access)",
            mode == kAudioSrcSilence ? "silence" : "tone");
    });
}
static BOOL MicPermitted(void) {
    AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    LOG(@"mic authorizationStatus=%ld", (long)st);
    return st == AVAuthorizationStatusAuthorized;
}
static void MicStart(void) {   // on gPumpQ
    if (gMicUsers++ > 0 || getenv(ENV_OFF_AUDIO)) return;   // SIMCAM_OFF_AUDIO: no audio at all
    double t0 = CFAbsoluteTimeGetCurrent();
    if (!gMicRing) gMicRing = [NSMutableArray array];
    int want = AudioSourceWanted();
    // SIMCAM_MIC_IGNORE_PERMISSION: use the mic even if the app is not authorized for it
    if (want == kAudioSrcMic && !getenv(ENV_MIC_IGNORE_PERMISSION) && !MicPermitted()) {
        LOG(@"SIMCAM_AUDIO_SOURCE=mic but microphone not authorized -> synthetic tone");
        want = kAudioSrcTone;
    }
    if (want != kAudioSrcMic) {
        ToneStart(want);
        return;
    }
    gAudioSrc = kAudioSrcMic;
    MicAudioSessionPrepare();
    double rate = [AVAudioSession sharedInstance].sampleRate;
    if (rate < 8000) rate = 48000;
    AudioSourceSetFormat(rate);
    AudioQueueRef q = NULL;
    OSStatus e = AudioQueueNewInput(&gMicASBD, MicInputCallback, NULL, NULL, kCFRunLoopCommonModes, 0, &q);
    if (e) {
        LOG(@"mic AudioQueueNewInput -> %d (no audio)", (int)e);
        return;
    }
    UInt32 bytes = (UInt32)(rate / 50) * 2;   // 20 ms buffers
    for (int i = 0; i < 6; i++) {
        AudioQueueBufferRef b;
        if (!AudioQueueAllocateBuffer(q, bytes, &b)) AudioQueueEnqueueBuffer(q, b, 0, NULL);
    }
    gAQ = q;
    gMicBufs = 0;
    e = AudioQueueStart(q, NULL);
    LOG(@"mic AudioQueueStart %.0f Hz -> %d (%.1f ms incl. audio session)", rate, (int)e,
        (CFAbsoluteTimeGetCurrent() - t0) * 1000);
}
static void MicStop(void) {
    if (gMicUsers <= 0 || --gMicUsers > 0) return;
    dispatch_sync(ToneQueue(), ^{
        if (gToneTimer) {
            dispatch_source_cancel(gToneTimer);
            gToneTimer = nil;
            LOG(@"synthetic audio stopped after %ld buffers", gToneBufs);
        }
    });
    AudioQueueRef q = gAQ;
    gAQ = NULL;
    if (q) {
        AudioQueueStop(q, true);
        AudioQueueDispose(q, true);
        LOG(@"mic stopped after %ld buffers", gMicBufs);
    }
    @synchronized (gMicRing) { [gMicRing removeAllObjects]; }
}
static void MovieAppendAudio(SimcamRecording *r, CMSampleBufferRef sb) {   // on gPumpQ
    if (!r.audioIn || !r.started || r.audioDone) return;
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb), end = CMTimeAdd(pts, CMSampleBufferGetDuration(sb));
    if (CMTimeCompare(end, r.start) <= 0) return;   // entirely before the movie start: drop
    if (!r.audioIn.readyForMoreMediaData) return;
    if ([r.audioIn appendSampleBuffer:sb]) {
        if (r.audioBufs++ == 0) {
            r.audioFirst = pts;
            LOG(@"movie first audio buffer at %+.1f ms vs first video frame",
                CMTimeGetSeconds(CMTimeSubtract(pts, r.start)) * 1000);
        }
    } else {
        LOG(@"movie audio append failed %@", r.writer.error);
    }
}
static void MicDeliver(CMSampleBufferRef sb) {   // mic thread
    CFRetain(sb);
    @synchronized (gMicRing) {
        [gMicRing addObject:(__bridge id)sb];
        while (gMicRing.count > 12) [gMicRing removeObjectAtIndex:0];
    }
    dispatch_async(gPumpQ, ^{
        NSArray *sessions;
        @synchronized (gMicSessions) { sessions = gMicSessions.allObjects; }
        for (SimcamSession *S in sessions) {
            NSArray *recs;
            @synchronized (S.recs) { recs = S.recs.allValues; }
            for (SimcamRecording *r in recs) @synchronized (r) { MovieAppendAudio(r, sb); }
            for (SimcamSink *k in S.sinks)
                if ([k.kind isEqualToString:@"ado"] && k.queue) {
                    LocalQueueMsg m = { kLocalQueueMsgSampleBuffer, sb, {0, 0} };
                    OSStatus e = pFigLocalQueueEnqueue(k.queue, &m, NULL);
                    if (k.frames++ < 2 || e) LOG(@"ado %@ audio buffer %d -> %d", k.sinkID, k.frames, (int)e);
                }
        }
        CFRelease(sb);
    });
}
static void MicUpdate(SimcamSession *S) {   // on gPumpQ: mic runs while a running session has an audio connection
    BOOL want = S.running && S.wantsMic;
    if (want == S.micOn) return;
    S.micOn = want;
    if (!gMicSessions) gMicSessions = [NSHashTable weakObjectsHashTable];
    if (want) {
        @synchronized (gMicSessions) { [gMicSessions addObject:S]; }
        MicStart();
    } else {
        @synchronized (gMicSessions) { [gMicSessions removeObject:S]; }
        MicStop();
    }
}

static void MovieAppendVideo(SimcamRecording *r, CVPixelBufferRef pb, CMTime pts) {
    if (r.stopping) return;
    if (!r.started) {
        if (![r.writer startWriting]) {
            LOG(@"movie startWriting failed %@", r.writer.error);
            r.stopping = YES;
            return;
        }
        [r.writer startSessionAtSourceTime:pts];
        r.start = pts;
        r.started = YES;
        if (r.audioIn) {
            NSArray *preroll;
            @synchronized (gMicRing) { preroll = [gMicRing copy]; }
            for (id sb in preroll) MovieAppendAudio(r, (__bridge CMSampleBufferRef)sb);
        }
    }
    if (r.adaptor.assetWriterInput.readyForMoreMediaData) {
        // webcam: straight from the ring frame on the GPU when it can (NULL otherwise)
        CVPixelBufferRef px = WcMovieOut(r, pb);
        // a webcam canvas at the ring's size
        if (!px && !r.conv && ((int)CVPixelBufferGetWidth(pb) != r.w || (int)CVPixelBufferGetHeight(pb) != r.h)) {
            SimcamSink *conv = [SimcamSink new];
            conv.w = r.w;
            conv.h = r.h;
            conv.fmt = '420v';
            r.conv = conv;
        }
        if (!px) px = r.conv ? SinkConvert(r.conv, pb) : CVPixelBufferRetain(pb);
        if (r.mirror) {
            VTPixelRotationSessionRef flipSess = r.flipSess;
            int flipKind = r.flipKind;
            CVPixelBufferRef flipped = MirrorFlip(px, r.rot, &flipSess, &flipKind);
            r.flipSess = flipSess;
            r.flipKind = flipKind;
            CVPixelBufferRelease(px);
            px = flipped;
        }
        [r.adaptor appendPixelBuffer:px withPresentationTime:pts];
        r.last = pts;
        CVPixelBufferRelease(px);
    }
}
