// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): runtime detection, session vtable layout check,
// preflight gate.
// ---- runtime detection — FigCaptureSourceCopySources (<= 26.x) vs -[FigCaptureSourceManager copySourcesWithType:]
// (27.x)
static CFArrayRef PreflightCopyStockSources(void) {
    CFArrayRef (*copySources)(int) = dlsym(RTLD_DEFAULT, "FigCaptureSourceCopySources");
    if (copySources) return copySources(1);
    Class managerClass = objc_getClass("FigCaptureSourceManager");
    SEL sharedSel = sel_registerName("sharedCaptureSourceManager");
    SEL copySel = sel_registerName("copySourcesWithType:");
    if (!managerClass || !class_getClassMethod(managerClass, sharedSel) ||
        !class_getInstanceMethod(managerClass, copySel))
        return NULL;
    id mgr = ((id (*)(id, SEL))objc_msgSend)(managerClass, sharedSel);
    // "copy" -> +1
    return mgr ? (__bridge_retained CFArrayRef)((id (*)(id, SEL, int))objc_msgSend)(mgr, copySel, 1) : NULL;
}

// ---- session vtable layout, checked by name BEFORE any session exists (the slot count grew 19 -> 24 in 27.0 without a
// version bump). The simulator session class is the local symbol _kFigCaptureSessionSimulatorVTable in CMCapture; find
// it in the loaded image's symbol table.
#import <mach-o/loader.h>
#import <mach-o/nlist.h>
static OSStatus (*pFigLocalQueueCreate)(CFAllocatorRef, long, CFTypeRef *);
static OSStatus (*pFigLocalQueueEnqueue)(CFTypeRef, void *, void *);
static OSStatus (*pFigCaptureSessionCreate)(CFAllocatorRef, void *, void *, CMBaseObjectRef *);
// each of a 64-bit image's load commands, in order, until visit returns YES. Not async-signal-safe (a block call);
// used at PREPARE time only.
static void MachOForEachCommand(const struct mach_header_64 *mh, BOOL (^visit)(const struct load_command *lc)) {
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (visit(lc)) return;
}
static const void *PreflightFindLocalSymbol(const char *imageSuffix, const char *sym) {
    size_t suffixLen = strlen(imageSuffix);
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *imageName = _dyld_get_image_name(i);
        size_t nameLen = strlen(imageName);
        if (nameLen < suffixLen || strcmp(imageName + nameLen - suffixLen, imageSuffix)) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        __block const struct segment_command_64 *linkedit = NULL;
        __block const struct symtab_command *symtab = NULL;
        MachOForEachCommand(mh, ^BOOL(const struct load_command *lc) {   // keeps the last match of each
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT"))
                linkedit = (const void *)lc;
            if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
            return NO;
        });
        if (!linkedit || !symtab) return NULL;
        uintptr_t base = slide + linkedit->vmaddr - linkedit->fileoff;
        const struct nlist_64 *nl = (const void *)(base + symtab->symoff);
        const char *strs = (const char *)(base + symtab->stroff);
        for (uint32_t k = 0; k < symtab->nsyms; k++) {
            uint32_t strIndex = nl[k].n_un.n_strx;
            if (!strIndex || strIndex >= symtab->strsize) continue;
            if (!strcmp(strs + strIndex, sym) && (nl[k].n_type & N_TYPE) == N_SECT)
                return (const void *)(nl[k].n_value + slide);
        }
        return NULL;
    }
    return NULL;
}
// Slots we replace, by index. Known layouts: 19 slots (17.5, 26.3), 24 slots (27.0: 19-22 NULL, 23
// VisionDataSinkTriggerBurst, 24 MetadataSinkCaptureFaceIDBracket).
#define SESS_NAMED_SLOTS 14   // slots 0..13: the ones kPreflightSessionSlots names (NULL: not checked)
static const char *kPreflightSessionSlots[SESS_NAMED_SLOTS] = {
    NULL, "SetSectionProperty", "CopySectionProperty", "SetConfiguration", "StartRunning", "StopRunning",
    "FileSinkStartRecording", "FileSinkStopRecording", "FileSinkPauseRecording", "FileSinkResumeRecording", NULL, NULL,
    "IrisStillImageSinkCaptureImage", "IrisStillImageSinkPrepareToCapture"};
static int gSessSlots;   // number of derived slots (excluding the version word); 0 = unknown
static NSString *PreflightCheckSessionSlots(const void *const *der, int *slotsOut) {
    if ((uintptr_t)der[0] != 1)
        return [NSString stringWithFormat:@"session derived class version %lu (expected 1)",
                                          (unsigned long)(uintptr_t)der[0]];
    int last = 0;
    for (int i = 1; i < 40; i++) {
        if (!der[i]) continue;
        Dl_info di = {0};
        if (!dladdr(der[i], &di) || !di.dli_sname || !strstr(di.dli_sname, "captureSessionSimulator_")) break;
        last = i;
        if (i < SESS_NAMED_SLOTS && kPreflightSessionSlots[i] && !strstr(di.dli_sname, kPreflightSessionSlots[i]))
            return [NSString stringWithFormat:@"session slot %d is %s, expected %s", i, di.dli_sname,
                    kPreflightSessionSlots[i]];
    }
    for (int i = 1; i < SESS_NAMED_SLOTS; i++)
        if (kPreflightSessionSlots[i] && !der[i])
            return [NSString stringWithFormat:@"session slot %d NULL, expected %s", i, kPreflightSessionSlots[i]];
    if (slotsOut) *slotsOut = last;
    // known layouts; an unknown count = Apple changed the table -> refuse
    const char *knownCounts = getenv(ENV_SLOT_COUNTS) ?: "19,24";
    BOOL known = NO;
    for (NSString *c in [@(knownCounts) componentsSeparatedByString:@","])
        if (c.intValue == last) known = YES;
    if (!known)
        return [NSString stringWithFormat:@"session vtable has %d slots, known layouts are %s", last, knownCounts];
    return nil;
}

// ---- preflight: returns YES only if every dependency of every hook looks as expected. ----
// Each step below runs its checks through `check`, which logs a failure and makes the preflight fail; the steps run in
// this order, and once the frameworks are loaded every check runs (a failed one doesn't stop the rest).
typedef void (^PreflightCheck)(BOOL ok, NSString *what);
// the CoreMedia / CMCapture symbols every hook needs; resolves the local-queue and session-create pointers
static void PreflightCheckSymbols(PreflightCheck check, void *cm, void *cmc) {
    const char *cmSyms[] = {"CMDerivedObjectCreate", "CMBaseObjectGetDerivedStorage", "CMBaseObjectGetVTable",
                            "CMNotificationCenterPostNotification", "CMNotificationCenterGetDefaultLocalCenter", NULL};
    for (int i = 0; cmSyms[i]; i++)
        check(dlsym(cm, cmSyms[i]) != NULL, [NSString stringWithFormat:@"CoreMedia %s", cmSyms[i]]);
    const char *cmcSyms[] = {"FigCaptureSourceGetClassID", "FigCaptureDeviceGetNotificationCenter",
                             "FigLocalQueueCreate", "FigLocalQueueEnqueue", NULL};
    for (int i = 0; cmcSyms[i]; i++)
        check(dlsym(cmc, cmcSyms[i]) != NULL, [NSString stringWithFormat:@"CMCapture %s", cmcSyms[i]]);
    pFigLocalQueueCreate = dlsym(cmc, "FigLocalQueueCreate");
    pFigLocalQueueEnqueue = dlsym(cmc, "FigLocalQueueEnqueue");
    pFigCaptureSessionCreate = dlsym(cmc, "FigCaptureSessionCreate");   // only the opt-in GOT route needs it
}
// the Fig key constants exist with the expected string values. Returns whether the 27.x attributes model
// (FigCaptureSourceAttributes) is there.
static BOOL PreflightCheckKeys(PreflightCheck check, void *cmc) {
    // key constants must exist and have the expected string values
    NSDictionary *keys = @{
        @"kFigCaptureSourceAttributeKey_UniqueID": @"UniqueID",
        @"kFigCaptureSourceAttributeKey_DeviceType": @"DeviceType",
        @"kFigCaptureSourceAttributeKey_Position": @"Position",
        @"kFigCaptureSourceAttributeKey_Focus": @"Focus",
        @"kFigCaptureSourceProperty_AdjustingFocus": @"AdjustingFocus",
        @"kFigCaptureSourceNotificationKey_NewPropertyValue":
            gActFakeBreak ? @"NewPropertyValueV2" : @"NewPropertyValue",
        @"kFigCaptureSessionNotification_DidStartRunning": @"DidStartRunning",
        @"kFigCaptureSessionNotification_ConfigurationCommitted": @"ConfigurationCommitted",
        @"kFigCaptureSessionSinkNotification_LocalQueueUpdated": @"LocalQueueUpdated",
        @"kFigCaptureSessionLocalQueueUpdatedNotificationPayloadKey_LocalQueue": @"LocalQueue",
        @"kFigCaptureSessionNotificationPayloadKey_SectionID": @"SectionID",
        // the notification that makes AVF fire didFinishCaptureForResolvedSettings
        @"kFigCaptureSessionIrisStillImageSinkNotification_ReadyForResponsiveRequest": @"ReadyForResponsiveRequest"};
    BOOL fsaModel = class_getInstanceMethod(objc_getClass("FigCaptureSourceAttributes"),
                                            sel_registerName("initWithAttributesDictionary:")) != NULL;
    for (NSString *k in keys) {
        CFStringRef *p = dlsym(cmc, k.UTF8String);
        // 27.x no longer exports kFigCaptureSourceAttributeKey_* (attributes are parsed by FigCaptureSourceAttributes)
        if (!p && [k hasPrefix:@"kFigCaptureSourceAttributeKey_"] && fsaModel) continue;
        check(p && [(__bridge NSString *)*p isEqualToString:keys[k]],
              [NSString stringWithFormat:@"key %@ (=%@)", k, p ? (__bridge NSString *)*p : @"nosym"]);
    }
    return fsaModel;
}
// the source factories: the video device's class method and the format class. Returns the format class.
static Class PreflightCheckFactories(PreflightCheck check) {
    Class dev = objc_getClass("AVCaptureFigVideoDevice");
    check(dev && class_getClassMethod(dev, sel_registerName("_newFigCaptureSources")),
          @"+[AVCaptureFigVideoDevice _newFigCaptureSources]");
    Class vf = objc_getClass("FigCaptureSourceVideoFormat");
    check(vf && class_getInstanceMethod(vf, sel_registerName("initWithFigCaptureStreamFormatDictionary:")),
          @"-[FigCaptureSourceVideoFormat initWithFigCaptureStreamFormatDictionary:]");
    return vf;
}
static void PreflightCheckSessionHook(PreflightCheck check) {
    // the session hook (objc route) — selector + ivar by name
    Class avs = objc_getClass("AVCaptureSession");
    check(avs && class_getInstanceMethod(avs, sel_registerName("_createFigCaptureSession")),
          @"-[AVCaptureSession _createFigCaptureSession]");
    check(avs && class_getInstanceVariable(avs, "_internal"), @"AVCaptureSession._internal ivar");
    Class avsi = objc_getClass("AVCaptureSessionInternal");
    Ivar figIvar = avsi ? class_getInstanceVariable(avsi, "figCaptureSession") : NULL;
    const char *figIvarType = figIvar ? ivar_getTypeEncoding(figIvar) : NULL;
    check(figIvar && figIvarType && (figIvarType[0] == '^' || figIvarType[0] == '@'),
          [NSString stringWithFormat:@"AVCaptureSessionInternal.figCaptureSession ivar (type %s)", figIvarType ?: "-"]);
    // session vtable slot names + count (before any session exists)
    const BaseObjVTable *svt = PreflightFindLocalSymbol("/CMCapture", "_kFigCaptureSessionSimulatorVTable");
    if (svt && svt->derivedClass) {
        NSString *why = PreflightCheckSessionSlots(svt->derivedClass, &gSessSlots);
        check(!why, why ?: @"");
        check(svt->baseClass && svt->baseClass->version == 2 && svt->baseClass->finalize,
              @"session base class v2 with finalize (the session finalize wrapper)");
        if (!why) LOG(@"preflight: session vtable %p has %d slots, names OK", svt, gSessSlots);
    } else if (getenv(ENV_PF_NOSYMOK))
        LOG(@"preflight: _kFigCaptureSessionSimulatorVTable not found; slot check deferred to the first session "
            @"(SIMCAM_PF_NOSYMOK)");
    else check(NO, @"_kFigCaptureSessionSimulatorVTable not found in CMCapture's symbol table");
}
static void PreflightCheckStockSource(PreflightCheck check, void *cm, void *cmc) {
    // ABI canary 1: stock sim source vtable shape (base v2, derived v1, slot names via dladdr)
    check(dlsym(cmc, "FigCaptureSourceCopySources") || objc_getClass("FigCaptureSourceManager"),
          @"no way to enumerate stock sources");
    const BaseObjVTable *(*getVT)(CMBaseObjectRef) = dlsym(cm, "CMBaseObjectGetVTable");
    if (getVT) {
        CFArrayRef stockSources = PreflightCopyStockSources();
        if (stockSources && CFArrayGetCount(stockSources)) {
            const BaseObjVTable *vt = getVT(CFArrayGetValueAtIndex(stockSources, 0));
            Dl_info di = {0};
            const void *const *der = vt->derivedClass;
            BOOL names = dladdr(der[1], &di) && di.dli_sname && strstr(di.dli_sname, "LockForConfiguration");
            check(vt->baseClass->version == 2 && (uintptr_t)der[0] == 1 && names,
                  [NSString stringWithFormat:@"source vtable shape base.v=%u derived.v=%lu slot1=%s",
                                             vt->baseClass->version, (unsigned long)(uintptr_t)der[0],
                                             di.dli_sname ?: "?"]);
        } else check(NO, @"stock source enumeration returned nothing");
        if (stockSources) CFRelease(stockSources);
    }
}
// The round trip's pixel format: '420v' (0x34323076), the value of kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange.
// A plain int literal on purpose: CoreVideo's constant is an unsigned OSType, which would box to a different NSNumber
// (numberWithUnsignedInt:); @PIXEL_FORMAT_420V is the int literal @875704438.
#define PIXEL_FORMAT_420V 875704438
static void PreflightCheckRoundTrips(PreflightCheck check, Class vf, BOOL fsaModel) {
    // ABI canary 2: a format dictionary round-trips through Apple's parser
    @try {
        id f = [[vf alloc] performSelector:sel_registerName("initWithFigCaptureStreamFormatDictionary:")
                                withObject:@{@"Name": @"pf", @"MediaType": @"vide", @"Width": @1280, @"Height": @720,
                                             @"PixelFormatType": @PIXEL_FORMAT_420V, @"VideoMinFrameRate": @1,
                                             @"VideoMaxFrameRate": @30}];
        float maxRate = ((float (*)(id, SEL))objc_msgSend)(f, sel_registerName("maxSupportedFrameRate"));
        check(maxRate == 30.0f, [NSString stringWithFormat:@"format round-trip maxSupportedFrameRate=%g", maxRate]);
    } @catch (NSException *e) {
        check(NO, [NSString stringWithFormat:@"format round-trip threw %@", e.reason]);
    }
    // ABI canary 3 (27.x model): an attributes dictionary round-trips through FigCaptureSourceAttributes
    if (fsaModel) @try {
        id at = ((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("FigCaptureSourceAttributes") alloc],
                                                    sel_registerName("initWithAttributesDictionary:"),
                                                    @{@"UniqueID": @"pf", @"DeviceType": @2, @"Position": @1});
        id uid = [at respondsToSelector:sel_registerName("uniqueID")]
                     ? ((id (*)(id, SEL))objc_msgSend)(at, sel_registerName("uniqueID"))
                     : nil;
        check(at && [uid isEqual:@"pf"],
              [NSString stringWithFormat:@"FigCaptureSourceAttributes round-trip uniqueID=%@", uid]);
    } @catch (NSException *e) {
        check(NO, [NSString stringWithFormat:@"attributes round-trip threw %@", e.reason]);
    }
}
static void PreflightCheckClientHooks(PreflightCheck check) {
    // client-side hooks: every selector we replace must exist with the encoding we assume
    struct { const char *cls, *sel; BOOL meta; const char *enc; } sels[] = {
        {"AVCapturePhoto", "fileDataRepresentation", NO, NULL},
        {"AVCapturePhoto", "fileDataRepresentationWithCustomizer:", NO, NULL},
        {"AVCapturePhoto", "CGImageRepresentation", NO, NULL},
        {"AVCaptureVideoPreviewLayer", "didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:", NO, NULL},
        {"AVCaptureDeviceRotationCoordinator", "initWithDevice:previewLayer:", NO, NULL},
        {"AVCaptureDeviceRotationCoordinator", "videoRotationAngleForHorizonLevelPreview", NO, NULL},
        {"AVCaptureDeviceRotationCoordinator", "videoRotationAngleForHorizonLevelCapture", NO, NULL},
    };
    for (size_t i = 0; i < sizeof sels / sizeof sels[0]; i++) {
        Class c = objc_getClass(sels[i].cls);
        check(c && class_getInstanceMethod(c, sel_registerName(sels[i].sel)),
              [NSString stringWithFormat:@"-[%s %s]", sels[i].cls, sels[i].sel]);
    }
    // microphone path (fake source + isConnected) — needed on 17.5, checked everywhere
    Class afd = objc_getClass("AVCaptureFigAudioDevice");
    check(afd && class_getClassMethod(afd, sel_registerName("_newFigCaptureSources")) &&
              class_getInstanceMethod(afd, sel_registerName("isConnected")) &&
              class_getInstanceMethod(afd, sel_registerName("figCaptureSource")),
          @"AVCaptureFigAudioDevice +_newFigCaptureSources / -isConnected / -figCaptureSource");
    // recommended-settings fix: 7-arg form (26/27) or 6-arg form (17.5)
    Class aco = objc_getClass("AVCaptureOutput");
    check(class_getInstanceMethod(aco, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:"
                                                        "videoCodecType:fileType:isIris:outputFileURL:"
                                                        "spatialAudioChannelLayoutTag:")) ||
              class_getInstanceMethod(aco, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:"
                                                            "videoCodecType:fileType:isIris:outputFileURL:")),
          @"-[AVCaptureOutput recommendedOutputSettingsForConnection:...] (6 or 7 args)");
    Method previewMethod = class_getInstanceMethod(objc_getClass("AVCaptureVideoPreviewLayer"),
                                                   sel_registerName("didUpdatePreviewImageQueueSlot:imageQueue:"
                                                                    "rotationDegrees:size:"));
    if (previewMethod) {
        char argType[8] = {0};
        method_getArgumentType(previewMethod, 4, argType, sizeof argType);
        check(argType[0] == 'd' || argType[0] == 'f',
              [NSString stringWithFormat:@"preview hook rotation arg type %s", argType]);
    }
    // photo surface: AVCapturePhoto._internal @8 and AVCapturePhotoInternal.photoSurface @0x20 (the photo fix reads
    // them by offset)
    Ivar photoInternal = class_getInstanceVariable(objc_getClass("AVCapturePhoto"), "_internal");
    Ivar photoSurface = class_getInstanceVariable(objc_getClass("AVCapturePhotoInternal"), "photoSurface");
    check(photoInternal && ivar_getOffset(photoInternal) == 8 && photoSurface && ivar_getOffset(photoSurface) == 0x20,
          [NSString stringWithFormat:@"AVCapturePhoto._internal@%td photoSurface@%td (expected 8, 32)",
                                     photoInternal ? ivar_getOffset(photoInternal) : -1,
                                     photoSurface ? ivar_getOffset(photoSurface) : -1]);
    // settings objects: rotation keys (degrees on 26/27, enum on 17.5) + the KVC keys the session layer reads
    Class iss = objc_getClass("FigCaptureIrisStillImageSettings");
    Class mfs = objc_getClass("FigCaptureMovieFileRecordingSettings");
    check(iss &&
              (class_getInstanceMethod(iss, sel_registerName("outputRotationDegrees")) ||
               class_getInstanceMethod(iss, sel_registerName("outputOrientation"))) &&
              class_getInstanceMethod(iss, sel_registerName("settingsID")),
          @"FigCaptureIrisStillImageSettings rotation/settingsID");
    check(mfs &&
              (class_getInstanceMethod(mfs, sel_registerName("videoRotationDegrees")) ||
               class_getInstanceMethod(mfs, sel_registerName("videoOrientation"))) &&
              class_getInstanceMethod(mfs, sel_registerName("outputURL")),
          @"FigCaptureMovieFileRecordingSettings rotation/outputURL");
    const char *cfgCls[] = {"FigCaptureSessionConfiguration", "FigCaptureVideoDataSinkConfiguration",
                            "FigCaptureVideoPreviewSinkConfiguration", "FigCaptureIrisSinkConfiguration",
                            "FigCaptureMovieFileSinkConfiguration", "FigCaptureMetadataObjectSinkConfiguration", NULL};
    for (int i = 0; cfgCls[i]; i++)
        check(objc_getClass(cfgCls[i]) != Nil, [NSString stringWithFormat:@"class %s", cfgCls[i]]);
}
static BOOL PreflightRun(void) {
    __block BOOL ok = YES;
    PreflightCheck check = ^(BOOL c, NSString *what) {
        if (!c) {
            ok = NO;
            LOG(@"PREFLIGHT FAIL: %@", what);
        }
    };
    void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
    void *cmc = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
    check(cm && cmc, @"dlopen CoreMedia/CMCapture");
    if (!cm || !cmc) return NO;
    PreflightCheckSymbols(check, cm, cmc);
    BOOL fsaModel = PreflightCheckKeys(check, cmc);
    Class vf = PreflightCheckFactories(check);
    PreflightCheckSessionHook(check);
    PreflightCheckStockSource(check, cm, cmc);
    PreflightCheckRoundTrips(check, vf, fsaModel);
    PreflightCheckClientHooks(check);
    LOG(@"PREFLIGHT %@", ok ? @"PASS" : @"FAIL — leaving stock AVFoundation behaviour");
    return ok;
}

