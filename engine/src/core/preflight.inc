// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): runtime detection, session vtable layout check, preflight gate.
// ---- runtime detection — FigCaptureSourceCopySources (<= 26.x) vs -[FigCaptureSourceManager copySourcesWithType:] (27.x)
static CFArrayRef M9CopyStockSources(void) {
    CFArrayRef (*f)(int) = dlsym(RTLD_DEFAULT, "FigCaptureSourceCopySources");
    if (f) return f(1);
    Class m = objc_getClass("FigCaptureSourceManager");
    SEL sh = sel_registerName("sharedCaptureSourceManager"), cp = sel_registerName("copySourcesWithType:");
    if (!m || !class_getClassMethod(m, sh) || !class_getInstanceMethod(m, cp)) return NULL;
    id mgr = ((id (*)(id, SEL))objc_msgSend)(m, sh);
    return mgr ? (__bridge_retained CFArrayRef)((id (*)(id, SEL, int))objc_msgSend)(mgr, cp, 1) : NULL; // "copy" -> +1
}

// ---- session vtable layout, checked by name BEFORE any session exists (the slot count grew 19 -> 24 in 27.0 without a version bump).
// The simulator session class is the local symbol _kFigCaptureSessionSimulatorVTable in CMCapture; find it in the loaded image's symbol table.
#import <mach-o/loader.h>
#import <mach-o/nlist.h>
static OSStatus (*pFigLocalQueueCreate)(CFAllocatorRef, long, CFTypeRef *);
static OSStatus (*pFigLocalQueueEnqueue)(CFTypeRef, void *, void *);
static OSStatus (*pFigCaptureSessionCreate)(CFAllocatorRef, void *, void *, CMBaseObjectRef *);
static const void *M10FindLocalSymbol(const char *imageSuffix, const char *sym) {
    size_t sl = strlen(imageSuffix);
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i); size_t l = strlen(nm);
        if (l < sl || strcmp(nm + l - sl, imageSuffix)) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i); intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const struct load_command *lc = (const void *)(mh + 1);
        const struct segment_command_64 *le = NULL; const struct symtab_command *st = NULL;
        for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT")) le = (const void *)lc;
            if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
        }
        if (!le || !st) return NULL;
        uintptr_t base = slide + le->vmaddr - le->fileoff;
        const struct nlist_64 *nl = (const void *)(base + st->symoff); const char *strs = (const char *)(base + st->stroff);
        for (uint32_t k = 0; k < st->nsyms; k++) {
            uint32_t sx = nl[k].n_un.n_strx; if (!sx || sx >= st->strsize) continue;
            if (!strcmp(strs + sx, sym) && (nl[k].n_type & N_TYPE) == N_SECT) return (const void *)(nl[k].n_value + slide);
        }
        return NULL;
    }
    return NULL;
}
// Slots we replace, by index. Known layouts: 19 slots (17.5, 26.3), 24 slots (27.0: 19-22 NULL, 23 VisionDataSinkTriggerBurst, 24 MetadataSinkCaptureFaceIDBracket).
static const char *kM10SlotWant[14] = {NULL, "SetSectionProperty", "CopySectionProperty", "SetConfiguration", "StartRunning", "StopRunning", "FileSinkStartRecording",
    "FileSinkStopRecording", "FileSinkPauseRecording", "FileSinkResumeRecording", NULL, NULL, "IrisStillImageSinkCaptureImage", "IrisStillImageSinkPrepareToCapture"};
static int gSessSlots;   // number of derived slots (excluding the version word); 0 = unknown
static NSString *M10CheckSessionDerived(const void *const *der, int *slotsOut) {
    if ((uintptr_t)der[0] != 1) return [NSString stringWithFormat:@"session derived class version %lu (expected 1)", (unsigned long)(uintptr_t)der[0]];
    int last = 0;
    for (int i = 1; i < 40; i++) {
        if (!der[i]) continue;
        Dl_info di = {0};
        if (!dladdr(der[i], &di) || !di.dli_sname || !strstr(di.dli_sname, "captureSessionSimulator_")) break;
        last = i;
        if (i < 14 && kM10SlotWant[i] && !strstr(di.dli_sname, kM10SlotWant[i])) return [NSString stringWithFormat:@"session slot %d is %s, expected %s", i, di.dli_sname, kM10SlotWant[i]];
    }
    for (int i = 1; i < 14; i++) if (kM10SlotWant[i] && !der[i]) return [NSString stringWithFormat:@"session slot %d NULL, expected %s", i, kM10SlotWant[i]];
    if (slotsOut) *slotsOut = last;
    const char *ok = getenv("SIMCAM_SLOT_COUNTS") ?: "19,24";   // known layouts; an unknown count = Apple changed the table -> refuse
    BOOL known = NO; for (NSString *c in [@(ok) componentsSeparatedByString:@","]) if (c.intValue == last) known = YES;
    if (!known) return [NSString stringWithFormat:@"session vtable has %d slots, known layouts are %s", last, ok];
    return nil;
}

// ---- preflight: returns YES only if every dependency of every hook looks as expected. ----
static BOOL M2Preflight(void) {
    __block BOOL ok = YES;
    void (^check)(BOOL, NSString *) = ^(BOOL c, NSString *what) { if (!c) { ok = NO; LOG(@"PREFLIGHT FAIL: %@", what); } };
    void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
    void *cmc = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
    check(cm && cmc, @"dlopen CoreMedia/CMCapture");
    if (!cm || !cmc) return NO;
    const char *cmSyms[] = {"CMDerivedObjectCreate", "CMBaseObjectGetDerivedStorage", "CMBaseObjectGetVTable", "CMNotificationCenterPostNotification", "CMNotificationCenterGetDefaultLocalCenter", NULL};
    for (int i = 0; cmSyms[i]; i++) check(dlsym(cm, cmSyms[i]) != NULL, [NSString stringWithFormat:@"CoreMedia %s", cmSyms[i]]);
    const char *cmcSyms[] = {"FigCaptureSourceGetClassID", "FigCaptureDeviceGetNotificationCenter", "FigLocalQueueCreate", "FigLocalQueueEnqueue", NULL};
    for (int i = 0; cmcSyms[i]; i++) check(dlsym(cmc, cmcSyms[i]) != NULL, [NSString stringWithFormat:@"CMCapture %s", cmcSyms[i]]);
    pFigLocalQueueCreate = dlsym(cmc, "FigLocalQueueCreate"); pFigLocalQueueEnqueue = dlsym(cmc, "FigLocalQueueEnqueue");
    pFigCaptureSessionCreate = dlsym(cmc, "FigCaptureSessionCreate");   // only the opt-in GOT route needs it
    // key constants must exist and have the expected string values
    NSDictionary *keys = @{@"kFigCaptureSourceAttributeKey_UniqueID": @"UniqueID", @"kFigCaptureSourceAttributeKey_DeviceType": @"DeviceType",
                           @"kFigCaptureSourceAttributeKey_Position": @"Position", @"kFigCaptureSourceAttributeKey_Focus": @"Focus",
                           @"kFigCaptureSourceProperty_AdjustingFocus": @"AdjustingFocus", @"kFigCaptureSourceNotificationKey_NewPropertyValue": gActFakeBreak ? @"NewPropertyValueV2" : @"NewPropertyValue",
                           @"kFigCaptureSessionNotification_DidStartRunning": @"DidStartRunning", @"kFigCaptureSessionNotification_ConfigurationCommitted": @"ConfigurationCommitted",
                           @"kFigCaptureSessionSinkNotification_LocalQueueUpdated": @"LocalQueueUpdated", @"kFigCaptureSessionLocalQueueUpdatedNotificationPayloadKey_LocalQueue": @"LocalQueue",
                           @"kFigCaptureSessionNotificationPayloadKey_SectionID": @"SectionID",
                           // the notification that makes AVF fire didFinishCaptureForResolvedSettings
                           @"kFigCaptureSessionIrisStillImageSinkNotification_ReadyForResponsiveRequest": @"ReadyForResponsiveRequest"};
    BOOL fsaModel = class_getInstanceMethod(objc_getClass("FigCaptureSourceAttributes"), sel_registerName("initWithAttributesDictionary:")) != NULL;
    for (NSString *k in keys) {
        CFStringRef *p = dlsym(cmc, k.UTF8String);
        // 27.x no longer exports kFigCaptureSourceAttributeKey_* (attributes are parsed by FigCaptureSourceAttributes)
        if (!p && [k hasPrefix:@"kFigCaptureSourceAttributeKey_"] && fsaModel) continue;
        check(p && [(__bridge NSString *)*p isEqualToString:keys[k]], [NSString stringWithFormat:@"key %@ (=%@)", k, p ? (__bridge NSString *)*p : @"nosym"]);
    }
    Class dev = objc_getClass("AVCaptureFigVideoDevice");
    check(dev && class_getClassMethod(dev, sel_registerName("_newFigCaptureSources")), @"+[AVCaptureFigVideoDevice _newFigCaptureSources]");
    Class vf = objc_getClass("FigCaptureSourceVideoFormat");
    check(vf && class_getInstanceMethod(vf, sel_registerName("initWithFigCaptureStreamFormatDictionary:")), @"-[FigCaptureSourceVideoFormat initWithFigCaptureStreamFormatDictionary:]");
    // the session hook (objc route) — selector + ivar by name
    Class avs = objc_getClass("AVCaptureSession");
    check(avs && class_getInstanceMethod(avs, sel_registerName("_createFigCaptureSession")), @"-[AVCaptureSession _createFigCaptureSession]");
    check(avs && class_getInstanceVariable(avs, "_internal"), @"AVCaptureSession._internal ivar");
    Class avsi = objc_getClass("AVCaptureSessionInternal"); Ivar fiv = avsi ? class_getInstanceVariable(avsi, "figCaptureSession") : NULL;
    const char *fty = fiv ? ivar_getTypeEncoding(fiv) : NULL;
    check(fiv && fty && (fty[0] == '^' || fty[0] == '@'), [NSString stringWithFormat:@"AVCaptureSessionInternal.figCaptureSession ivar (type %s)", fty ?: "-"]);
    // session vtable slot names + count (before any session exists)
    const M2VTable *svt = M10FindLocalSymbol("/CMCapture", "_kFigCaptureSessionSimulatorVTable");
    if (svt && svt->derivedClass) {
        NSString *why = M10CheckSessionDerived(svt->derivedClass, &gSessSlots);
        check(!why, why ?: @"");
        check(svt->baseClass && svt->baseClass->version == 2 && svt->baseClass->finalize, @"session base class v2 with finalize (m10 finalize wrapper)");
        if (!why) LOG(@"preflight: session vtable %p has %d slots, names OK", svt, gSessSlots);
    } else if (getenv("SIMCAM_PF_NOSYMOK")) LOG(@"preflight: _kFigCaptureSessionSimulatorVTable not found; slot check deferred to the first session (SIMCAM_PF_NOSYMOK)");
    else check(NO, @"_kFigCaptureSessionSimulatorVTable not found in CMCapture's symbol table");
    // ABI canary 1: stock sim source vtable shape (base v2, derived v1, slot names via dladdr)
    check(dlsym(cmc, "FigCaptureSourceCopySources") || objc_getClass("FigCaptureSourceManager"), @"no way to enumerate stock sources");
    const M2VTable *(*getVT)(CMBaseObjectRef) = dlsym(cm, "CMBaseObjectGetVTable");
    if (getVT) {
        CFArrayRef a = M9CopyStockSources();
        if (a && CFArrayGetCount(a)) {
            const M2VTable *vt = getVT(CFArrayGetValueAtIndex(a, 0));
            Dl_info di = {0};
            const void *const *der = vt->derivedClass;
            BOOL names = dladdr(der[1], &di) && di.dli_sname && strstr(di.dli_sname, "LockForConfiguration");
            check(vt->baseClass->version == 2 && (uintptr_t)der[0] == 1 && names, [NSString stringWithFormat:@"source vtable shape base.v=%u derived.v=%lu slot1=%s", vt->baseClass->version, (unsigned long)(uintptr_t)der[0], di.dli_sname ?: "?"]);
        } else check(NO, @"stock source enumeration returned nothing");
        if (a) CFRelease(a);
    }
    // ABI canary 2: a format dictionary round-trips through Apple's parser
    @try {
        id f = [[vf alloc] performSelector:sel_registerName("initWithFigCaptureStreamFormatDictionary:") withObject:@{@"Name": @"pf", @"MediaType": @"vide", @"Width": @1280, @"Height": @720, @"PixelFormatType": @875704438, @"VideoMinFrameRate": @1, @"VideoMaxFrameRate": @30}];
        float mx = ((float (*)(id, SEL))objc_msgSend)(f, sel_registerName("maxSupportedFrameRate"));
        check(mx == 30.0f, [NSString stringWithFormat:@"format round-trip maxSupportedFrameRate=%g", mx]);
    } @catch (NSException *e) { check(NO, [NSString stringWithFormat:@"format round-trip threw %@", e.reason]); }
    // ABI canary 3 (27.x model): an attributes dictionary round-trips through FigCaptureSourceAttributes
    if (fsaModel) @try {
        id at = ((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("FigCaptureSourceAttributes") alloc], sel_registerName("initWithAttributesDictionary:"), @{@"UniqueID": @"pf", @"DeviceType": @2, @"Position": @1});
        id uid = [at respondsToSelector:sel_registerName("uniqueID")] ? ((id (*)(id, SEL))objc_msgSend)(at, sel_registerName("uniqueID")) : nil;
        check(at && [uid isEqual:@"pf"], [NSString stringWithFormat:@"FigCaptureSourceAttributes round-trip uniqueID=%@", uid]);
    } @catch (NSException *e) { check(NO, [NSString stringWithFormat:@"attributes round-trip threw %@", e.reason]); }
    // client-side hooks: every selector we replace must exist with the encoding we assume
    struct { const char *cls, *sel; BOOL meta; const char *enc; } sels[] = {
        {"AVCapturePhoto", "fileDataRepresentation", NO, NULL}, {"AVCapturePhoto", "fileDataRepresentationWithCustomizer:", NO, NULL}, {"AVCapturePhoto", "CGImageRepresentation", NO, NULL},
        {"AVCaptureVideoPreviewLayer", "didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:", NO, NULL},
        {"AVCaptureDeviceRotationCoordinator", "initWithDevice:previewLayer:", NO, NULL}, {"AVCaptureDeviceRotationCoordinator", "videoRotationAngleForHorizonLevelPreview", NO, NULL},
        {"AVCaptureDeviceRotationCoordinator", "videoRotationAngleForHorizonLevelCapture", NO, NULL},
    };
    for (size_t i = 0; i < sizeof sels / sizeof sels[0]; i++) {
        Class c = objc_getClass(sels[i].cls);
        check(c && class_getInstanceMethod(c, sel_registerName(sels[i].sel)), [NSString stringWithFormat:@"-[%s %s]", sels[i].cls, sels[i].sel]);
    }
    Class afd = objc_getClass("AVCaptureFigAudioDevice");   // microphone path (fake source + isConnected) — needed on 17.5, checked everywhere
    check(afd && class_getClassMethod(afd, sel_registerName("_newFigCaptureSources")) && class_getInstanceMethod(afd, sel_registerName("isConnected")) && class_getInstanceMethod(afd, sel_registerName("figCaptureSource")),
          @"AVCaptureFigAudioDevice +_newFigCaptureSources / -isConnected / -figCaptureSource");
    Class aco = objc_getClass("AVCaptureOutput");   // recommended-settings fix: 7-arg form (26/27) or 6-arg form (17.5)
    check(class_getInstanceMethod(aco, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:outputFileURL:spatialAudioChannelLayoutTag:")) ||
          class_getInstanceMethod(aco, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:videoCodecType:fileType:isIris:outputFileURL:")), @"-[AVCaptureOutput recommendedOutputSettingsForConnection:...] (6 or 7 args)");
    Method pm = class_getInstanceMethod(objc_getClass("AVCaptureVideoPreviewLayer"), sel_registerName("didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:"));
    if (pm) { char t[8] = {0}; method_getArgumentType(pm, 4, t, sizeof t); check(t[0] == 'd' || t[0] == 'f', [NSString stringWithFormat:@"preview hook rotation arg type %s", t]); }
    // photo surface: AVCapturePhoto._internal @8 and AVCapturePhotoInternal.photoSurface @0x20 (the photo fix reads them by offset)
    Ivar pi = class_getInstanceVariable(objc_getClass("AVCapturePhoto"), "_internal"); Ivar ps = class_getInstanceVariable(objc_getClass("AVCapturePhotoInternal"), "photoSurface");
    check(pi && ivar_getOffset(pi) == 8 && ps && ivar_getOffset(ps) == 0x20, [NSString stringWithFormat:@"AVCapturePhoto._internal@%td photoSurface@%td (expected 8, 32)", pi ? ivar_getOffset(pi) : -1, ps ? ivar_getOffset(ps) : -1]);
    // settings objects: rotation keys (degrees on 26/27, enum on 17.5) + the KVC keys the session layer reads
    Class iss = objc_getClass("FigCaptureIrisStillImageSettings"), mfs = objc_getClass("FigCaptureMovieFileRecordingSettings");
    check(iss && (class_getInstanceMethod(iss, sel_registerName("outputRotationDegrees")) || class_getInstanceMethod(iss, sel_registerName("outputOrientation"))) && class_getInstanceMethod(iss, sel_registerName("settingsID")), @"FigCaptureIrisStillImageSettings rotation/settingsID");
    check(mfs && (class_getInstanceMethod(mfs, sel_registerName("videoRotationDegrees")) || class_getInstanceMethod(mfs, sel_registerName("videoOrientation"))) && class_getInstanceMethod(mfs, sel_registerName("outputURL")), @"FigCaptureMovieFileRecordingSettings rotation/outputURL");
    const char *cfgCls[] = {"FigCaptureSessionConfiguration", "FigCaptureVideoDataSinkConfiguration", "FigCaptureVideoPreviewSinkConfiguration", "FigCaptureIrisSinkConfiguration", "FigCaptureMovieFileSinkConfiguration", "FigCaptureMetadataObjectSinkConfiguration", NULL};
    for (int i = 0; cfgCls[i]; i++) check(objc_getClass(cfgCls[i]) != Nil, [NSString stringWithFormat:@"class %s", cfgCls[i]]);
    LOG(@"PREFLIGHT %@", ok ? @"PASS" : @"FAIL — leaving stock AVFoundation behaviour");
    return ok;
}

