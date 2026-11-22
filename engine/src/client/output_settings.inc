// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): recommended output settings fix.
//
// Provides: RecSettingsResolveFix (core/lazy.inc)
// Owns:     nothing shared.
// Uses:     core/act.inc, providers/audio.inc
#include "../core/activation.h"
#include "../providers/audio.h"
#include "output_settings.h"

// ============================================================================
// -[AVCaptureOutput recommendedOutputSettingsForConnection:...] is a stub in the simulator's AVFCapture. For video it
// gives AVVideoWidthKey and AVVideoHeightKey 0 (AVAssetWriterInput then throws "must specify a positive width"); for
// audio, AVSampleRateKey 0. The hook fills them in from the source settings and the audio source's rate.
// ============================================================================
static IMP gOrigRecommendedSettings7Arg;
static IMP gOrigRecommendedSettings6Arg;
static id RecSettingsFix(NSDictionary *recommended, NSDictionary *sourceSettings);

static id hook_recommendedOutputSettings7Arg(id self, SEL _cmd, id connection, NSDictionary *sourceSettings, id codec,
                                             id fileType, BOOL iris, id url, unsigned tag) {
    return RecSettingsFix(((id (*)(id, SEL, id, id, id, id, BOOL, id, unsigned))gOrigRecommendedSettings7Arg)(
                              self, _cmd, connection, sourceSettings, codec, fileType, iris, url, tag),
                          sourceSettings);
}

// iOS 17.5 has the same method without spatialAudioChannelLayoutTag:
static id hook_recommendedOutputSettings6Arg(id self, SEL _cmd, id connection, NSDictionary *sourceSettings, id codec,
                                             id fileType, BOOL iris, id url) {
    return RecSettingsFix(((id (*)(id, SEL, id, id, id, id, BOOL, id))gOrigRecommendedSettings6Arg)(
                              self, _cmd, connection, sourceSettings, codec, fileType, iris, url),
                          sourceSettings);
}

// The video size when the source settings give none: 1080p.
#define kRecSettingsFallbackWidth 1920
#define kRecSettingsFallbackHeight 1080
// The frame rate the default bit rate assumes, and the one filled in when the settings have none.
#define kRecSettingsFrameRate 30
// The audio sample rate when the microphone source has none, and the bit rate per channel filled in when the settings
// have no bit rate.
#define kRecSettingsFallbackSampleRateHz 48000
#define kRecSettingsAudioBitRatePerChannel 64000

// The compression properties the simulator's software encoder accepts: numbers that are 0 are dropped (except
// AllowFrameReordering, where 0 means NO), and so are the keys only a device encoder knows; a bit rate and a frame rate
// are filled in when missing.
static NSMutableDictionary *RecSettingsCompressionProps(NSDictionary *original, int width, int height) {
    NSMutableDictionary *props = [original mutableCopy] ?: [NSMutableDictionary dictionary];
    LOG(@"recommendedVideoSettings original compression props: %@", props);

    for (NSString *key in props.allKeys) {
        BOOL isZero = [props[key] isKindOfClass:[NSNumber class]] && [props[key] doubleValue] == 0;
        if (isZero && ![key isEqualToString:AVVideoAllowFrameReorderingKey]) {
            [props removeObjectForKey:key];
        }
    }

    // The simulator's software encoder rejects device-encoder-only keys (Priority, RealTime, QP bounds, ...) when the
    // AVAssetWriterInput is created; keep only these.
    NSSet *allowedKeys = [NSSet setWithObjects:AVVideoAverageBitRateKey, AVVideoExpectedSourceFrameRateKey,
                                               AVVideoMaxKeyFrameIntervalKey, AVVideoMaxKeyFrameIntervalDurationKey,
                                               AVVideoProfileLevelKey, AVVideoAllowFrameReorderingKey,
                                               AVVideoH264EntropyModeKey, AVVideoQualityKey, nil];
    for (NSString *key in props.allKeys) {
        if (![allowedKeys containsObject:key]) {
            [props removeObjectForKey:key];
        }
    }

    // 0.125 bit per pixel per frame: about 7.8 Mbit/s at 1080p30, near what a device records
    if (!props[AVVideoAverageBitRateKey]) {
        props[AVVideoAverageBitRateKey] = @((long)(width * height * kRecSettingsFrameRate * 0.25 * 0.5));
    }
    if (!props[AVVideoExpectedSourceFrameRateKey]) {
        props[AVVideoExpectedSourceFrameRateKey] = @(kRecSettingsFrameRate);
    }
    return props;
}

// Video: a width and height from the source settings (else 1080p) and compression properties the encoder accepts.
// If AVAssetWriterInput still rejects the result, only the average bit rate is kept.
static void RecSettingsFixVideo(NSMutableDictionary *fixed, NSDictionary *recommended, NSDictionary *sourceSettings) {
    if ([fixed[AVVideoWidthKey] intValue] > 0 && [fixed[AVVideoHeightKey] intValue] > 0) {
        return;
    }
    int width = [sourceSettings[(id)kCVPixelBufferWidthKey] intValue];
    int height = [sourceSettings[(id)kCVPixelBufferHeightKey] intValue];

    if (width <= 0 || height <= 0) {
        width = kRecSettingsFallbackWidth;
        height = kRecSettingsFallbackHeight;
    }

    fixed[AVVideoWidthKey] = @(width);
    fixed[AVVideoHeightKey] = @(height);
    NSMutableDictionary *props = RecSettingsCompressionProps(fixed[AVVideoCompressionPropertiesKey], width, height);
    fixed[AVVideoCompressionPropertiesKey] = props;

    @try {
        (void)[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:fixed];
    } @catch (NSException *exception) {
        LOG(@"recommendedVideoSettings still rejected (%@) -> minimal", exception.reason);
        fixed[AVVideoCompressionPropertiesKey] = @{AVVideoAverageBitRateKey: props[AVVideoAverageBitRateKey]};
    }

    LOG(@"recommendedVideoSettings fixed: %dx%d (was %@x%@) codec=%@", width, height, recommended[AVVideoWidthKey],
        recommended[AVVideoHeightKey], fixed[AVVideoCodecKey]);
}

// Audio: the microphone source's sample rate (else 48 kHz) and, if the settings have no bit rate, one per channel.
static void RecSettingsFixAudio(NSMutableDictionary *fixed) {
    if ([fixed[AVSampleRateKey] doubleValue] <= 0) {
        fixed[AVSampleRateKey] = @(gAudio.source.format.mSampleRate > 0 ? gAudio.source.format.mSampleRate
                                                                         : kRecSettingsFallbackSampleRateHz);
        if (!fixed[AVEncoderBitRateKey] && !fixed[AVEncoderBitRatePerChannelKey]) {
            fixed[AVEncoderBitRatePerChannelKey] = @(kRecSettingsAudioBitRatePerChannel);
        }
        LOG(@"recommendedAudioSettings fixed: %@", fixed);
    }
}

static id RecSettingsFix(NSDictionary *recommended, NSDictionary *sourceSettings) {
    // SIMCAM_OFF_RECOMMENDED_SETTINGS: AVF's stub settings
    if (![recommended isKindOfClass:[NSDictionary class]] || !RecommendedSettingsFixOn()) {
        return recommended;
    }
    NSMutableDictionary *fixed = [recommended mutableCopy];
    if (fixed[AVVideoCodecKey]) {
        RecSettingsFixVideo(fixed, recommended, sourceSettings);
    } else if (fixed[AVFormatIDKey]) {
        RecSettingsFixAudio(fixed);
    }
    return fixed;
}

// resolve step: the 7-argument form (iOS 26 and 27), else the 6-argument form (iOS 17)
static BOOL RecSettingsResolveFix(void) {
    if (!RecommendedSettingsFixOn()) {
        return YES;
    }
    Class outputClass = objc_getClass("AVCaptureOutput");
    if (class_getInstanceMethod(outputClass, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:"
                                                              "videoCodecType:fileType:isIris:outputFileURL:"
                                                              "spatialAudioChannelLayoutTag:"))) {
        return ActivationHookResolve(
            kHookGroupClient, "recfix (7-arg)", outputClass,
            "recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:"
            "outputFileURL:spatialAudioChannelLayoutTag:",
            NO, NO, (IMP)hook_recommendedOutputSettings7Arg, &gOrigRecommendedSettings7Arg, YES);
    }
    return ActivationHookResolve(kHookGroupClient, "recfix (6-arg 17.x)", outputClass,
                                 "recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:"
                                 "outputFileURL:",
                                 NO, NO, (IMP)hook_recommendedOutputSettings6Arg, &gOrigRecommendedSettings6Arg, YES);
}
