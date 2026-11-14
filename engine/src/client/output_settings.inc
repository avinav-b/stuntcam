// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): recommended output settings fix.
// ============================================================================
// m7: -[AVCaptureOutput recommendedOutputSettingsForConnection:...] is a stub in the sim AVFCapture:
// video → AVVideoWidthKey/HeightKey 0 (AVAssetWriterInput throws "must specify a positive width"),
// audio → AVSampleRateKey 0. Fill them from the source settings / the audio source rate.
// ============================================================================
static IMP gOrigRecSettings, gOrigRecSettings6;
static id M7FixRecSettings(NSDictionary *r, NSDictionary *src);
static id m7_recSettings(id self, SEL c, id conn, NSDictionary *src, id codec, id fileType, BOOL iris, id url, unsigned tag) {
    return M7FixRecSettings(((id (*)(id, SEL, id, id, id, id, BOOL, id, unsigned))gOrigRecSettings)(self, c, conn, src, codec, fileType, iris, url, tag), src);
}
// m10: iOS 17.5 has the same method without spatialAudioChannelLayoutTag:
static id m7_recSettings6(id self, SEL c, id conn, NSDictionary *src, id codec, id fileType, BOOL iris, id url) {
    return M7FixRecSettings(((id (*)(id, SEL, id, id, id, id, BOOL, id))gOrigRecSettings6)(self, c, conn, src, codec, fileType, iris, url), src);
}
static id M7FixRecSettings(NSDictionary *r, NSDictionary *src) {
    if (![r isKindOfClass:[NSDictionary class]] || getenv("M7_NO_RECFIX")) return r;
    NSMutableDictionary *m = [r mutableCopy];
    if (m[AVVideoCodecKey]) {
        if ([m[AVVideoWidthKey] intValue] <= 0 || [m[AVVideoHeightKey] intValue] <= 0) {
            int w = [src[(id)kCVPixelBufferWidthKey] intValue], h = [src[(id)kCVPixelBufferHeightKey] intValue];
            if (w <= 0 || h <= 0) { w = 1920; h = 1080; }
            m[AVVideoWidthKey] = @(w); m[AVVideoHeightKey] = @(h);
            NSMutableDictionary *cp = [m[AVVideoCompressionPropertiesKey] mutableCopy] ?: [NSMutableDictionary dictionary];
            LOG(@"recommendedVideoSettings original compression props: %@", cp);
            for (NSString *key in cp.allKeys) if ([cp[key] isKindOfClass:[NSNumber class]] && [cp[key] doubleValue] == 0 && ![key isEqualToString:AVVideoAllowFrameReorderingKey]) [cp removeObjectForKey:key];
            // device-encoder-only keys the simulator's software encoder rejects at AVAssetWriterInput init (Priority, RealTime, QP bounds, ...)
            NSSet *ok = [NSSet setWithObjects:AVVideoAverageBitRateKey, AVVideoExpectedSourceFrameRateKey, AVVideoMaxKeyFrameIntervalKey, AVVideoMaxKeyFrameIntervalDurationKey, AVVideoProfileLevelKey, AVVideoAllowFrameReorderingKey, AVVideoH264EntropyModeKey, AVVideoQualityKey, nil];
            for (NSString *key in cp.allKeys) if (![ok containsObject:key]) [cp removeObjectForKey:key];
            if (!cp[AVVideoAverageBitRateKey]) cp[AVVideoAverageBitRateKey] = @((long)(w * h * 30 * 0.25 * 0.5)); // ~7.8 Mbit/s at 1080p30, near a device
            if (!cp[AVVideoExpectedSourceFrameRateKey]) cp[AVVideoExpectedSourceFrameRateKey] = @30;
            m[AVVideoCompressionPropertiesKey] = cp;
            @try { (void)[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:m]; }
            @catch (NSException *e) { LOG(@"recommendedVideoSettings still rejected (%@) -> minimal", e.reason); m[AVVideoCompressionPropertiesKey] = @{AVVideoAverageBitRateKey: cp[AVVideoAverageBitRateKey]}; }
            LOG(@"recommendedVideoSettings fixed: %dx%d (was %@x%@) codec=%@", w, h, r[AVVideoWidthKey], r[AVVideoHeightKey], m[AVVideoCodecKey]);
        }
    } else if (m[AVFormatIDKey]) {
        if ([m[AVSampleRateKey] doubleValue] <= 0) {
            m[AVSampleRateKey] = @(gMicASBD.mSampleRate > 0 ? gMicASBD.mSampleRate : 48000);
            if (!m[AVEncoderBitRateKey] && !m[AVEncoderBitRatePerChannelKey]) m[AVEncoderBitRatePerChannelKey] = @64000;
            LOG(@"recommendedAudioSettings fixed: %@", m);
        }
    }
    return m;
}
static BOOL m7recfix(void) {   // PREPARE 4: the 7-argument form (26/27), else the 6-argument 17.x form
    if (getenv("M7_NO_RECFIX")) return YES;
    Class C = objc_getClass("AVCaptureOutput");
    if (class_getInstanceMethod(C, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:outputFileURL:spatialAudioChannelLayoutTag:")))
        return ActHookResolve(HG_CLIENT, "recfix (7-arg)", C, "recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:outputFileURL:spatialAudioChannelLayoutTag:", NO, NO, (IMP)m7_recSettings, &gOrigRecSettings, YES);
    return ActHookResolve(HG_CLIENT, "recfix (6-arg 17.x)", C, "recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:outputFileURL:", NO, NO, (IMP)m7_recSettings6, &gOrigRecSettings6, YES);
}

