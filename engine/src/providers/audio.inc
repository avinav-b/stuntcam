// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): movie audio, synthetic audio source.

// ---- movie --------------------------------------------------------------------
// ---- m6: audio. The simulator FigCaptureSession never captures audio (StartRunning/StopRunning/FileSink* are one shared stub),
// so the mic is captured in-process with an AudioQueue (CoreSimulator routes it to the Mac microphone) and fed to (a) our movie
// writer's AAC input and (b) audio data sinks (LocalQueue), all on the host clock like the video frames.
#import <AudioToolbox/AudioToolbox.h>
static AudioQueueRef gAQ; static int gMicUsers; static AudioStreamBasicDescription gMicASBD; static CMAudioFormatDescriptionRef gMicFmt;
static NSMutableArray *gMicRing;            // last ~250 ms of CMSampleBuffers (pre-roll for a recording that starts mid-buffer)
static NSHashTable *gMicSessions;           // running M5Sess that want audio
static long gMicBufs; static double gMicPeak;
static void M5AudioDeliver(CMSampleBufferRef sb);
static void M5MicCB(void *ud, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts, UInt32 np, const AudioStreamPacketDescription *pd) {
    UInt32 frames = b->mAudioDataByteSize / gMicASBD.mBytesPerFrame;
    if (frames && gMicFmt) {
        CMTime pts;
        if (ts && (ts->mFlags & kAudioTimeStampHostTimeValid)) pts = CMClockMakeHostTimeFromSystemUnits(ts->mHostTime);
        else pts = CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), CMTimeMake(frames, (int32_t)gMicASBD.mSampleRate));
        if (gMicBufs == 0) LOG(@"mic first buffer frames=%u hostTimeValid=%d sampleTimeValid=%d lag=%.1f ms", (unsigned)frames, ts ? !!(ts->mFlags & kAudioTimeStampHostTimeValid) : -1, ts ? !!(ts->mFlags & kAudioTimeStampSampleTimeValid) : -1,
                              CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), pts)) * 1000);
        int16_t *smp = b->mAudioData; for (UInt32 i = 0; i < frames; i++) { double v = fabs(smp[i] / 32768.0); if (v > gMicPeak) gMicPeak = v; }
        CMBlockBufferRef bb = NULL;
        if (!CMBlockBufferCreateWithMemoryBlock(NULL, NULL, b->mAudioDataByteSize, NULL, NULL, 0, b->mAudioDataByteSize, kCMBlockBufferAssureMemoryNowFlag, &bb)) {
            CMBlockBufferReplaceDataBytes(b->mAudioData, bb, 0, b->mAudioDataByteSize);
            CMSampleBufferRef sb = NULL;
            if (!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, gMicFmt, frames, pts, NULL, &sb)) { M5AudioDeliver(sb); CFRelease(sb); }
            CFRelease(bb);
        }
        if (++gMicBufs % 250 == 0) { LOG(@"mic %ld buffers, peak since last %.1f dBFS", gMicBufs, 20 * log10(MAX(gMicPeak, 1e-9))); gMicPeak = 0; }
    }
    if (gAQ == q) AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static void M5AudioSessionPrepare(void) {
    AVAudioSession *as = [AVAudioSession sharedInstance]; NSError *e = nil;
    if (![as.category isEqualToString:AVAudioSessionCategoryPlayAndRecord] && ![as.category isEqualToString:AVAudioSessionCategoryRecord]) {
        // what AVCaptureSession does on a device when automaticallyConfiguresApplicationAudioSession is YES
        [as setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeVideoRecording options:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothHFP error:&e];
        LOG(@"audio session category -> PlayAndRecord/VideoRecording err=%@", e);
    }
    e = nil; [as setActive:YES error:&e];
    LOG(@"audio session active err=%@ rate=%.0f inputs=%@", e, as.sampleRate, [as.currentRoute.inputs valueForKey:@"portName"]);
}
// ---- m7: synthetic audio source (default). M7_AUDIO_SRC = tone (default) | silence | mic
static dispatch_source_t gToneTimer; static double gTonePhase; static long gToneBufs; static int gAudioSrc; // 0 tone 1 silence 2 mic
// w1: the tone state (timer, counters, gMicFmt) is only touched on gToneQ: start and stop run there synchronously, so a stop/start
// can no longer free gMicFmt under a still-running timer handler, and the counter read is not a race (TSan, w1 e5).
static dispatch_queue_t gToneQ;
static dispatch_queue_t M7ToneQ(void) { static dispatch_once_t o; dispatch_once(&o, ^{ gToneQ = dispatch_queue_create("m7.tone", DISPATCH_QUEUE_SERIAL); }); return gToneQ; }
static int M7AudioSrcWanted(void) {
    const char *e = getenv("M7_AUDIO_SRC");
    if (!e || !strcmp(e, "tone")) return 0;
    if (!strcmp(e, "silence")) return 1;
    if (!strcmp(e, "mic")) return 2;
    return 0;
}
static void M7ToneStart(int mode) { dispatch_sync(M7ToneQ(), ^{
    gAudioSrc = mode;
    gMicASBD = (AudioStreamBasicDescription){ .mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 2, .mFramesPerPacket = 1, .mBytesPerFrame = 2, .mChannelsPerFrame = 1, .mBitsPerChannel = 16 };
    if (gMicFmt) CFRelease(gMicFmt); gMicFmt = NULL;
    CMAudioFormatDescriptionCreate(NULL, &gMicASBD, 0, NULL, 0, NULL, NULL, &gMicFmt);
    dispatch_queue_t tq = gToneQ;
    gToneBufs = 0; gTonePhase = 0;
    __block CMTime next = CMClockGetTime(CMClockGetHostTimeClock());
    gToneTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, tq);
    dispatch_source_set_timer(gToneTimer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 50, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(gToneTimer, ^{
        // emit every whole 20 ms buffer that is due (keeps PTS contiguous and on the host clock)
        CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
        while (CMTimeCompare(CMTimeAdd(next, CMTimeMake(960, 48000)), now) <= 0) {
            const UInt32 frames = 960; int16_t buf[960];
            for (UInt32 i = 0; i < frames; i++) { buf[i] = gAudioSrc == 1 ? 0 : (int16_t)(0.1 * 32767 * sin(gTonePhase)); gTonePhase += 2 * M_PI * 440.0 / 48000.0; if (gTonePhase > 2 * M_PI) gTonePhase -= 2 * M_PI; }
            CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
            if (!CMBlockBufferCreateWithMemoryBlock(NULL, NULL, sizeof buf, NULL, NULL, 0, sizeof buf, kCMBlockBufferAssureMemoryNowFlag, &bb)) {
                CMBlockBufferReplaceDataBytes(buf, bb, 0, sizeof buf);
                CMTime pts = CMTimeConvertScale(next, 48000, kCMTimeRoundingMethod_Default);
                if (!CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, gMicFmt, frames, pts, NULL, &sb)) { M5AudioDeliver(sb); CFRelease(sb); }
                CFRelease(bb);
            }
            next = CMTimeAdd(next, CMTimeMake(960, 48000));
            if (gToneBufs++ == 0) LOG(@"synthetic audio (%s) first buffer", gAudioSrc == 1 ? "silence" : "tone 440 Hz -20 dBFS");
        }
    });
    dispatch_resume(gToneTimer);
    LOG(@"audio source: synthetic %s 48 kHz mono (no AVAudioSession / mic access)", mode == 1 ? "silence" : "tone");
}); }
static BOOL M7MicPermitted(void) {
    AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    LOG(@"mic authorizationStatus=%ld", (long)st);
    return st == AVAuthorizationStatusAuthorized;
}
static void M5MicStart(void) {   // on gPumpQ
    if (gMicUsers++ > 0 || getenv("M6_NO_MIC")) return;
    double t0 = CFAbsoluteTimeGetCurrent();
    if (!gMicRing) gMicRing = [NSMutableArray array];
    int want = M7AudioSrcWanted();
    if (want == 2 && !getenv("M7_MIC_FORCE") && !M7MicPermitted()) { LOG(@"M7_AUDIO_SRC=mic but microphone not authorized -> synthetic tone"); want = 0; }
    if (want != 2) { M7ToneStart(want); return; }
    gAudioSrc = 2;
    M5AudioSessionPrepare();
    double rate = [AVAudioSession sharedInstance].sampleRate; if (rate < 8000) rate = 48000;
    gMicASBD = (AudioStreamBasicDescription){ .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 2, .mFramesPerPacket = 1, .mBytesPerFrame = 2, .mChannelsPerFrame = 1, .mBitsPerChannel = 16 };
    if (gMicFmt) CFRelease(gMicFmt); gMicFmt = NULL;
    CMAudioFormatDescriptionCreate(NULL, &gMicASBD, 0, NULL, 0, NULL, NULL, &gMicFmt);
    AudioQueueRef q = NULL;
    OSStatus e = AudioQueueNewInput(&gMicASBD, M5MicCB, NULL, NULL, kCFRunLoopCommonModes, 0, &q);
    if (e) { LOG(@"mic AudioQueueNewInput -> %d (no audio)", (int)e); return; }
    UInt32 bytes = (UInt32)(rate / 50) * 2; // 20 ms buffers
    for (int i = 0; i < 6; i++) { AudioQueueBufferRef b; if (!AudioQueueAllocateBuffer(q, bytes, &b)) AudioQueueEnqueueBuffer(q, b, 0, NULL); }
    gAQ = q; gMicBufs = 0;
    e = AudioQueueStart(q, NULL);
    LOG(@"mic AudioQueueStart %.0f Hz -> %d (%.1f ms incl. audio session)", rate, (int)e, (CFAbsoluteTimeGetCurrent() - t0) * 1000);
}
static void M5MicStop(void) {
    if (gMicUsers <= 0 || --gMicUsers > 0) return;
    dispatch_sync(M7ToneQ(), ^{ if (gToneTimer) { dispatch_source_cancel(gToneTimer); gToneTimer = nil; LOG(@"synthetic audio stopped after %ld buffers", gToneBufs); } });
    AudioQueueRef q = gAQ; gAQ = NULL;
    if (q) { AudioQueueStop(q, true); AudioQueueDispose(q, true); LOG(@"mic stopped after %ld buffers", gMicBufs); }
    @synchronized (gMicRing) { [gMicRing removeAllObjects]; }
}
static void M5RecAppendAudio(M5Rec *r, CMSampleBufferRef sb) {   // on gPumpQ
    if (!r.audioIn || !r.started || r.audioDone) return;
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb), end = CMTimeAdd(pts, CMSampleBufferGetDuration(sb));
    if (CMTimeCompare(end, r.start) <= 0) return;   // entirely before the movie start: drop
    if (!r.audioIn.readyForMoreMediaData) return;
    if ([r.audioIn appendSampleBuffer:sb]) { if (r.audioBufs++ == 0) { r.audioFirst = pts; LOG(@"movie first audio buffer at %+.1f ms vs first video frame", CMTimeGetSeconds(CMTimeSubtract(pts, r.start)) * 1000); } }
    else LOG(@"movie audio append failed %@", r.writer.error);
}
static void M5AudioDeliver(CMSampleBufferRef sb) {   // mic thread
    CFRetain(sb);
    @synchronized (gMicRing) { [gMicRing addObject:(__bridge id)sb]; while (gMicRing.count > 12) [gMicRing removeObjectAtIndex:0]; }
    dispatch_async(gPumpQ, ^{
        NSArray *ss; @synchronized (gMicSessions) { ss = gMicSessions.allObjects; }
        for (M5Sess *S in ss) {
            NSArray *recs; @synchronized (S.recs) { recs = S.recs.allValues; }
            for (M5Rec *r in recs) @synchronized (r) { M5RecAppendAudio(r, sb); }
            for (M5Sink *k in S.sinks) if ([k.kind isEqualToString:@"ado"] && k.queue) {
                M2LQMsg m = { 3, sb, {0, 0} }; OSStatus e = pFigLocalQueueEnqueue(k.queue, &m, NULL);
                if (k.frames++ < 2 || e) LOG(@"ado %@ audio buffer %d -> %d", k.sinkID, k.frames, (int)e);
            }
        }
        CFRelease(sb);
    });
}
static void M5UpdateMic(M5Sess *S) {   // on gPumpQ: mic runs while a running session has an audio connection
    BOOL want = S.running && S.wantsMic;
    if (want == S.micOn) return;
    S.micOn = want;
    if (!gMicSessions) gMicSessions = [NSHashTable weakObjectsHashTable];
    if (want) { @synchronized (gMicSessions) { [gMicSessions addObject:S]; } M5MicStart(); }
    else { @synchronized (gMicSessions) { [gMicSessions removeObject:S]; } M5MicStop(); }
}

static void M5RecAppend(M5Rec *r, CVPixelBufferRef pb, CMTime pts) {
    if (r.stopping) return;
    if (!r.started) {
        if (![r.writer startWriting]) { LOG(@"movie startWriting failed %@", r.writer.error); r.stopping = YES; return; }
        [r.writer startSessionAtSourceTime:pts]; r.start = pts; r.started = YES;
        if (r.audioIn) { NSArray *pre; @synchronized (gMicRing) { pre = [gMicRing copy]; } for (id sb in pre) M5RecAppendAudio(r, (__bridge CMSampleBufferRef)sb); }
    }
    if (r.adaptor.assetWriterInput.readyForMoreMediaData) {
        CVPixelBufferRef px = r.conv ? M5Convert(r.conv, pb) : CVPixelBufferRetain(pb);
        if (r.mirror) { VTPixelRotationSessionRef fs = r.flipSess; int fk = r.flipKind; CVPixelBufferRef f = M12Flip(px, r.rot, &fs, &fk); r.flipSess = fs; r.flipKind = fk; CVPixelBufferRelease(px); px = f; }
        [r.adaptor appendPixelBuffer:px withPresentationTime:pts]; r.last = pts; CVPixelBufferRelease(px);
    }
}

