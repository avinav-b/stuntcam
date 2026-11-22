// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): Fig capture sources: CMBaseObject ABI, source model,
// attributes, formats, source factories, fake mic.
//
// Provides: BaseObjClass, BaseObjVTable, CMBaseObjectRef, the CMBaseObject types (core/preflight.inc, fig/, providers/)
//           hook_videoNewFigCaptureSources, hook_audioNewFigCaptureSources, hook_audioIsConnected (core/init.inc)
// Owns:     the fake sources and their properties (each source under its own lock)
//           the dlsym'd CoreMedia functions p* and gKeysSeen, gOverride (filled by core/init.inc)
//           gOrigAudioSources, gOrigIsConnected, the audio hooks' original IMPs (stored by core/init.inc)
//           gQuietPost, which silences the post log line (set by providers/video.inc)
// Uses:     nothing.
// ---- CMBaseObject ABI (CoreMedia/CMBaseObject.h, version 2) ----------------
typedef const void *CMBaseObjectRef;
typedef uint32_t CMBaseClassID;
typedef struct {
    uint32_t version;
    size_t derivedStorageSize;
    Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
    OSStatus (*invalidate)(CMBaseObjectRef);
    void (*finalize)(CMBaseObjectRef);
    CFStringRef (*copyDebugDescription)(CMBaseObjectRef);
    OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef, CFTypeRef);
    OSStatus (*notificationBarrier)(CMBaseObjectRef);
    const void *protocolTable;
} BaseObjClass;
typedef struct {
    const void *reserved;
    const BaseObjClass *baseClass;
    const void *derivedClass;
} BaseObjVTable;

static OSStatus (*pCMDerivedObjectCreate)(CFAllocatorRef, const BaseObjVTable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*pCMBaseObjectGetDerivedStorage)(CMBaseObjectRef);
static CMBaseClassID (*pFigCaptureSourceGetClassID)(void);

// ---- our source model --------------------------------------------------------
typedef struct {
    int index;          // 0 = back wide, 1 = front wide
    int lockCount;
} SrcStorage;

static NSMutableSet *gKeysSeen;
static NSDictionary *gOverride;
static NSDictionary *SrcAttributes(int index);
static NSArray *SrcFormats(int index);
#define SRC_MAXSOURCES 16
enum { kFigSourceErrValueNotAvailable = -12784 };   // kFigCaptureSourceError / ValueNotAvailable
static NSMutableDictionary *gSourceProps[SRC_MAXSOURCES];

static void logKey(const char *op, int idx, CFStringRef key, NSString *answer) {
    @synchronized (gKeysSeen) {
        NSString *seenKey = [NSString stringWithFormat:@"%s %@", op, key];
        if (![gKeysSeen containsObject:seenKey] || getenv(ENV_DIAG_EVERY_KEY)) {
            [gKeysSeen addObject:seenKey];
            LOG(@"src%d %s %@ -> %@", idx, op, key, answer);
        }
    }
}

static OSStatus src_Invalidate(CMBaseObjectRef o) {
    LOG(@"Invalidate");
    return 0;
}

static void src_Finalize(CMBaseObjectRef o) {
    LOG(@"Finalize");
}

static CFStringRef src_CopyDebugDescription(CMBaseObjectRef o) {
    SrcStorage *s = pCMBaseObjectGetDerivedStorage(o);
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("<SimcamFigCaptureSource %d>"), s->index);
}

static OSStatus src_CopyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef alloc, void *out) {
    SrcStorage *s = pCMBaseObjectGetDerivedStorage(o);
    id val = gSourceProps[s->index][(__bridge NSString *)key];
    if (CFEqual(key, CFSTR("LockedForConfiguration"))) {
        val = @(s->lockCount > 0);
    }
    if (val == [NSNull null]) {
        val = nil;
    }
    if (!val) {
        logKey("CopyProperty", s->index, key, @"MISSING (-12784)");
        if (out) {
            *(CFTypeRef *)out = NULL;
        }
        return kFigSourceErrValueNotAvailable;
    }
    NSString *desc = [val isKindOfClass:[NSArray class]]
                         ? [NSString stringWithFormat:@"array[%lu]", (unsigned long)[val count]]
                     : [val isKindOfClass:[NSDictionary class]]
                         ? [NSString stringWithFormat:@"dict[%lu]", (unsigned long)[val count]]
                         : [val description];
    logKey("CopyProperty", s->index, key, desc);
    if (out) {
        *(CFTypeRef *)out = CFRetain((__bridge CFTypeRef)val);
    }
    return 0;
}

static void *(*pFigCaptureDeviceGetNotificationCenter)(void);
static _Atomic BOOL gQuietPost;   // written on the pump queue, read in AVF's setFocusMode: path (TSan); log-only
static OSStatus (*pCMNotificationCenterPostNotification)(void *, CFTypeRef, CFTypeRef, CFTypeRef, uint32_t);

static void SrcPost(CMBaseObjectRef src, NSString *name, id value) {
    NSDictionary *payload = value ? @{@"NewPropertyValue": value} : nil;
    OSStatus e = pCMNotificationCenterPostNotification(pFigCaptureDeviceGetNotificationCenter(),
                                                       (__bridge CFStringRef)name, src,
                                                       (__bridge CFDictionaryRef)payload, 0);
    if (!gQuietPost) {
        LOG(@"posted %@=%@ -> %d", name, value, (int)e);
    }
}

static void SrcPostAdjusting(CMBaseObjectRef o, NSString *prop, int ms) {
    CFRetain(o);
    SrcPost(o, prop, @YES);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{
        SrcPost(o, prop, @NO);
        CFRelease(o);
    });
}

static OSStatus src_SetProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef value) {
    SrcStorage *s = pCMBaseObjectGetDerivedStorage(o);
    NSString *valueDesc = value ? [(__bridge id)value description] : @"(null)";
    if (valueDesc.length > 300) {
        valueDesc = [[valueDesc substringToIndex:300] stringByAppendingString:@"…"];
    }
    if (getenv(ENV_DIAG_SET_PROPERTY)) {
        LOG(@"src%d SetProperty %@ = %@", s->index, key, valueDesc);
    } else {
        logKey("SetProperty", s->index, key, valueDesc);
    }
    if (value) {
        gSourceProps[s->index][(__bridge NSString *)key] = (__bridge id)value;
    }
    NSString *k = (__bridge NSString *)key;
    if (!SetPropertyNotesOn()) {
        return 0;
    }

    // manual-control completions (setExposureTargetBias:/setFocusModeLocked:/custom exposure/WB gains completion
    // handlers)
    if ([k hasSuffix:@"Operation"] && [(__bridge id)value isKindOfClass:[NSDictionary class]] &&
        ((__bridge NSDictionary *)value)[@"RequestID"]) {
        NSDictionary *op = (__bridge NSDictionary *)value;
        NSString *done = nil;
        if ([k isEqualToString:@"ExposureTargetBiasOperation"]) {
            done = @"BiasedExposureComplete";
            if (op[@"Bias"]) {
                gSourceProps[s->index][@"ExposureTargetBias"] = op[@"Bias"];
                if (BiasNoteOn()) {
                    SrcPost(o, @"ExposureTargetBias", op[@"Bias"]);
                }
            }
        }
        else if ([k isEqualToString:@"ExposureOperation"] && (op[@"ExposureDuration"] || op[@"ISO"])) {
            done = @"ManualExposureComplete";
        } else if ([k isEqualToString:@"FocusOperation"] && op[@"FocusPosition"]) {
            done = @"ManualFocusComplete";
        } else if ([k isEqualToString:@"WhiteBalanceOperation"] && op[@"DeviceWhiteBalanceGains"]) {
            done = @"ManualWhiteBalanceComplete";
        }
        if (done && ManualCompletionOn()) {
            id requestID = op[@"RequestID"];
            CFRetain(o);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{
                NSDictionary *payload = @{
                    @"ManualControlRequestID": requestID,
                    @"RequestID": requestID,
                    @"Timestamp": (__bridge_transfer id)CMTimeCopyAsDictionary(
                        CMClockGetTime(CMClockGetHostTimeClock()), NULL),
                };
                OSStatus e = pCMNotificationCenterPostNotification(pFigCaptureDeviceGetNotificationCenter(),
                                                                   (__bridge CFStringRef)done, o,
                                                                   (__bridge CFDictionaryRef)payload, 0);
                LOG(@"posted %@ rid=%@ -> %d", done, requestID, (int)e);
                CFRelease(o);
            });
        }
    }

    if ([k isEqualToString:@"FocusOperation"]) {
        SrcPostAdjusting(o, @"AdjustingFocus", 300);
    } else if ([k isEqualToString:@"ExposureOperation"]) {
        SrcPostAdjusting(o, @"AdjustingExposure", 250);
    } else if ([k isEqualToString:@"WhiteBalanceOperation"]) {
        SrcPostAdjusting(o, @"AdjustingWhiteBalance", 200);
    }

    // AutoTorchEnabled=0 must not post TorchActive=NO (SIMCAM_OFF_TORCH_LEVEL_ONLY=1 does)
    else if ([k containsString:@"Torch"] && ![k isEqualToString:@"TorchActive"] &&
             (!TorchLevelOnlyOn() || [k isEqualToString:@"TorchLevel"])) {
        // TorchLevel (float) / TorchMode-like keys: report TorchActive = level > 0
        BOOL on = [(__bridge id)value respondsToSelector:@selector(floatValue)]
                      ? [(__bridge id)value floatValue] > 0
                      : NO;
        CFRetain(o);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{
            SrcPost(o, @"TorchActive", @(on));
            CFRelease(o);
        });
    }

    return 0;
}

static OSStatus src_Lock(CMBaseObjectRef o) {
    SrcStorage *s = pCMBaseObjectGetDerivedStorage(o);
    s->lockCount++;
    LOG(@"src%d LockForConfiguration", s->index);
    return 0;
}

static OSStatus src_Unlock(CMBaseObjectRef o) {
    SrcStorage *s = pCMBaseObjectGetDerivedStorage(o);
    s->lockCount--;
    LOG(@"src%d UnlockForConfiguration", s->index);
    return 0;
}

static OSStatus src_CheckTCC(CMBaseObjectRef o) {
    SrcStorage *s = pCMBaseObjectGetDerivedStorage(o);
    LOG(@"src%d CheckTCCAccess -> 0", s->index);
    return 0;
}

// Unknown extra derived-class slots: log which slot is called.
#define SLOT(n)                                                                     \
    static OSStatus src_slot##n(CMBaseObjectRef o, void *a, void *b, void *c) {     \
        LOG(@"derived slot %d called (%p %p %p)", n, a, b, c);                      \
        return 0;                                                                   \
    }

SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)

static const BaseObjClass kSrcBaseClass = {
    .version = 2,
    .derivedStorageSize = sizeof(SrcStorage),
    .equal = NULL,
    .invalidate = src_Invalidate,
    .finalize = src_Finalize,
    .copyDebugDescription = src_CopyDebugDescription,
    .copyProperty = src_CopyProperty,
    .setProperty = src_SetProperty,
};

static const void *kSrcDerivedClass[16] = {
    (void *)1, (void *)src_Lock, (void *)src_Unlock, (void *)src_CheckTCC,
    (void *)src_slot4, (void *)src_slot5, (void *)src_slot6, (void *)src_slot7,
    (void *)src_slot8, (void *)src_slot9, (void *)src_slot10, (void *)src_slot11,
    (void *)src_slot12, (void *)src_slot13, (void *)src_slot14, (void *)src_slot15,
};

static const BaseObjVTable kSrcVTable = { NULL, &kSrcBaseClass, kSrcDerivedClass };


// ---- logging dictionary: records every key AVF/CMCapture reads ---------------
@interface SimcamLogDict : NSDictionary
@property (strong) NSDictionary *backing;
@property (copy) NSString *tag;
@end
@implementation SimcamLogDict

+ (instancetype)with:(NSDictionary *)d tag:(NSString *)tag {
    SimcamLogDict *x = [[self alloc] init];
    x.backing = d;
    x.tag = tag;
    return x;
}

- (NSUInteger)count { return self.backing.count; }

- (NSEnumerator *)keyEnumerator { return self.backing.keyEnumerator; }

- (id)objectForKey:(id)k {
    id v = self.backing[k];
    @synchronized (gKeysSeen) {
        NSString *sig = [NSString stringWithFormat:@"%@ %@", [self.tag substringToIndex:3], k];
        if (![gKeysSeen containsObject:sig]) {
            [gKeysSeen addObject:sig];
            LOG(@"%@ read %@ -> %@", self.tag, k,
                v ? ([v isKindOfClass:[NSArray class]] || [v isKindOfClass:[NSDictionary class]] ? @"<coll>" : v)
                  : @"MISSING");
        }
    }
    return v;
}

- (id)copyWithZone:(NSZone *)z { return self; }
@end

static id SrcWrapLogged(NSDictionary *d, NSString *tag) {
    const char *offKeyLog = EnvConfig(ENV_OFF_KEY_LOG, ENV_LEGACY_OFF_KEY_LOG);
    return (offKeyLog && strcmp(offKeyLog, "0")) ? d : [SimcamLogDict with:d tag:tag];
}

// 27.x reads property "Attributes" = FigCaptureSourceAttributes (typed object), built by Apple's own initializer from
// the same dictionary
static void SrcAddAttributesObject(int i, NSDictionary *attrs) {
    Class FSA = objc_getClass("FigCaptureSourceAttributes");
    if (!FSA || !class_getInstanceMethod(FSA, sel_registerName("initWithAttributesDictionary:"))) {
        return;
    }
    id at = ((id (*)(id, SEL, id))objc_msgSend)([FSA alloc], sel_registerName("initWithAttributesDictionary:"), attrs);
    if (at) {
        gSourceProps[i][@"Attributes"] = at;
    } else {
        LOG(@"MISMATCH: FigCaptureSourceAttributes initWithAttributesDictionary: returned nil (src%d)", i);
    }
}

static NSDictionary *gFormatOverride;

static NSArray *SrcFormats(int index) {
    Class VF = objc_getClass("FigCaptureSourceVideoFormat");
    NSMutableArray *out = [NSMutableArray array];
    NSArray *specs = (gOverride[@"devices"] ? gOverride[@"devices"][index][@"formats"] : nil)
                     ?: gOverride[@"formats"]
                     ?: @[
                            @{@"w": @1920, @"h": @1080, @"fps": @30},
                            @{@"w": @1920, @"h": @1080, @"fps": @60},
                            @{@"w": @3840, @"h": @2160, @"fps": @30},
                            @{@"w": @3840, @"h": @2160, @"fps": @60},
                        ];
    int i = 0;

    for (NSDictionary *spec in specs) {
        NSMutableDictionary *fd = [NSMutableDictionary dictionary];
        NSDictionary *tmpl = gOverride[@"formatTemplate"];
        if (tmpl) {
            [fd addEntriesFromDictionary:tmpl];
        }
        // values substituted from spec: any template string value "$w"/"$h"/"$fps"
        for (NSString *k in fd.allKeys) {
            id v = fd[k];
            if ([v isKindOfClass:[NSString class]] && [v hasPrefix:@"$"]) {
                fd[k] = spec[[v substringFromIndex:1]] ?: v;
            }
        }
        if (spec[@"extra"]) {
            [fd addEntriesFromDictionary:spec[@"extra"]];
        }
        id f = [[VF alloc] performSelector:sel_registerName("initWithFigCaptureStreamFormatDictionary:")
                                withObject:SrcWrapLogged(fd, [NSString stringWithFormat:@"fmt%d.%d", index, i])];
        if (f) {
            [out addObject:f];
        }
        i++;
    }

    return out;
}

static CFArrayRef gSources;

static CFArrayRef SrcCopyAll(void) {
    if (!gSources) {
        CMBaseObjectRef objs[SRC_MAXSOURCES] = {0};
        int n = gOverride[@"devices"] ? (int)MIN([gOverride[@"devices"] count], SRC_MAXSOURCES)
                                      : (getenv(ENV_ONE_SOURCE) ? 1 : 2);
        for (int i = 0; i < n; i++) {
            gSourceProps[i] = [NSMutableDictionary dictionary];
            gSourceProps[i][@"AttributesDictionary"] =
                SrcWrapLogged(SrcAttributes(i), [NSString stringWithFormat:@"attr%d", i]);
            SrcAddAttributesObject(i, gSourceProps[i][@"AttributesDictionary"]);
            // 17.5 reads barcode availability from the *source property* AvailableMetadataKeyGroups (26.3 from the
            // attribute)
            id keyGroups = SrcAttributes(i)[@"AvailableMetadataKeyGroups"];
            if (keyGroups) {
                gSourceProps[i][@"AvailableMetadataKeyGroups"] = keyGroups;
            }
            gSourceProps[i][@"Formats"] = SrcFormats(i);
            [gSourceProps[i] addEntriesFromDictionary:gOverride[@"props"]];
            OSStatus err = pCMDerivedObjectCreate(kCFAllocatorDefault, &kSrcVTable, pFigCaptureSourceGetClassID(),
                                                  &objs[i]);
            if (err) {
                LOG(@"CMDerivedObjectCreate failed %d", (int)err);
                for (int k = 0; k < i; k++) {
                    CFRelease(objs[k]);   // the sources already made (+1 each, as below)
                }
                return NULL;
            }
            SrcStorage *s = pCMBaseObjectGetDerivedStorage(objs[i]);
            s->index = i;
        }
        gSources = CFArrayCreate(NULL, (const void **)objs, n, &kCFTypeArrayCallBacks);
        for (int i = 0; i < n; i++) {
            CFRelease(objs[i]);
        }
        LOG(@"created %d fake FigCaptureSources", n);
    }

    return CFRetain(gSources);
}

// iOS 17.5's AVF drops the stock simulator audio source (it has no attributes), so there is no microphone device.
// Provide one fake audio FigCaptureSource with microphone attributes (index SRC_MAXSOURCES-1). Audio samples come from
// MicStart (providers/audio.inc).
static CFTypeRef gFakeMicSrc;

// "new" family: caller expects +1
__attribute__((ns_returns_retained)) static id MicNewFakeSources(id self, SEL _cmd) {
    if (!gFakeMicSrc) {
        int i = SRC_MAXSOURCES - 1;
        gSourceProps[i] = [NSMutableDictionary dictionary];
        NSDictionary *attrs = gOverride[@"audio"] ?: @{
            @"UniqueID": @"com.apple.avfoundation.avcapturedevice.built-in_audio:0",
            @"LocalizedName": @"iPhone Microphone",
            @"NonLocalizedName": @"iPhone Microphone",
            @"ModelID": @"iPhone Microphone",
            @"Manufacturer": @"Apple Inc.",
            @"Position": @0,
            @"DeviceType": @0,
        };
        gSourceProps[i][@"AttributesDictionary"] = [SimcamLogDict with:attrs tag:@"audioattr"];
        SrcAddAttributesObject(i, attrs);
        gSourceProps[i][@"Formats"] = @[];
        CMBaseObjectRef o = NULL;
        if (pCMDerivedObjectCreate(kCFAllocatorDefault, &kSrcVTable, pFigCaptureSourceGetClassID(), &o) == 0) {
            ((SrcStorage *)pCMBaseObjectGetDerivedStorage(o))->index = i;
            gFakeMicSrc = o;
        }
    }

    LOG(@"+[AVCaptureFigAudioDevice _newFigCaptureSources] -> fake microphone source %p", gFakeMicSrc);
    return gFakeMicSrc ? @[(__bridge id)gFakeMicSrc] : @[];
}

// The audio factory is hooked at SWAP whenever SIMCAM_FIG_SOURCES is set; its first armed call decides fake mic vs
// stock once, in the caller's own context (not a probe at activation, which would call AVF from the activating thread).
// The stock sources from that first call are the answer when stock wins, so the original runs exactly once for the
// decision.
static IMP gOrigAudioSources;
static _Atomic int gMicMode;   // 0 undecided, 1 fake microphone, 2 stock

// "new" family: caller expects +1
__attribute__((ns_returns_retained)) static id hook_audioNewFigCaptureSources(id self, SEL _cmd) {
    int mode = atomic_load(&gMicMode);

    if (!mode) {
        const char *fakeMicEnv = getenv(ENV_FAKE_MIC);
        NSArray *stock = nil;
        if (fakeMicEnv) {
            mode = fakeMicEnv[0] == '1' ? 1 : 2;
        } else {
            BOOL stockHasAttrs = NO;
            @try {
                // "new": +1
                stock = (__bridge_transfer NSArray *)((CFTypeRef (*)(id, SEL))gOrigAudioSources)(self, _cmd);
                const BaseObjVTable *(*getVT)(CMBaseObjectRef) = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
                for (id src in stock) {
                    const BaseObjVTable *vt = getVT((__bridge CMBaseObjectRef)src);
                    CFTypeRef d = NULL;
                    if (vt && vt->baseClass && vt->baseClass->copyProperty &&
                        vt->baseClass->copyProperty((__bridge CMBaseObjectRef)src, CFSTR("AttributesDictionary"), NULL,
                                                    &d) == 0 &&
                        d) {
                        if (CFGetTypeID(d) == CFDictionaryGetTypeID() && CFDictionaryGetCount(d) > 0) {
                            stockHasAttrs = YES;
                        }
                        CFRelease(d);
                    }
                }
                LOG(@"stock audio sources: %lu, with attributes: %d", (unsigned long)stock.count, stockHasAttrs);
            } @catch (NSException *e) {
                LOG(@"stock audio source probe threw %@", e);
            }
            mode = stockHasAttrs ? 2 : 1;
        }
        int undecided = 0;
        if (atomic_compare_exchange_strong(&gMicMode, &undecided, mode)) {
            LOG(@"microphone: %s (decided at the first audio source query%s)",
                mode == 1 ? "fake microphone source" : "stock", fakeMicEnv ? ", SIMCAM_FAKE_MIC" : "");
        } else {
            mode = undecided;
        }
        if (mode == 2 && stock) {
            return stock;
        }
    }

    if (mode == 1) {
        return MicNewFakeSources(self, _cmd);
    }
    return (__bridge_transfer id)((CFTypeRef (*)(id, SEL))gOrigAudioSources)(self, _cmd);
}

static IMP gOrigIsConnected;

static BOOL hook_audioIsConnected(id self, SEL c) {
    id src = [self respondsToSelector:sel_registerName("figCaptureSource")]
                 ? (__bridge id)((CFTypeRef (*)(id, SEL))objc_msgSend)(self, sel_registerName("figCaptureSource"))
                 : nil;
    if (gFakeMicSrc && (__bridge CFTypeRef)src == gFakeMicSrc) {
        return YES;
    }
    return ((BOOL (*)(id, SEL))gOrigIsConnected)(self, c);
}

// +1: a +0 (autoreleased) return under ARC over-released gSources
__attribute__((ns_returns_retained)) static id hook_videoNewFigCaptureSources(id self, SEL _cmd) {
    LOG(@"+[%@ _newFigCaptureSources] -> fake", self);
    return (__bridge_transfer id)SrcCopyAll(); // "new" -> +1
}

static NSDictionary *SrcAttributes(int index) {
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    NSDictionary *common = gOverride[@"common"];
    if (common) {
        [d addEntriesFromDictionary:common];
    }
    NSDictionary *perDevice = gOverride[@"devices"] ? gOverride[@"devices"][index]
                                                    : gOverride[index == 0 ? @"back" : @"front"];
    if (perDevice) {
        [d addEntriesFromDictionary:perDevice];
    }
    [d removeObjectForKey:@"formats"];
    return d;
}


