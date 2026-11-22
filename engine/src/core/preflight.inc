// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): runtime detection, session vtable layout check,
// preflight gate.
//
// Provides: PreflightRun (core/lazy.inc)
//           kSessSlot*, PreflightCheckSessionSlots, kFigBaseClassVersion (fig/session_hook.inc, providers/pump.inc)
//           MachOForEachCommand (core/crash_handler.inc, fig/session_hook.inc)
// Owns:     pFigLocalQueueCreate, pFigLocalQueueEnqueue (called by providers/pump.inc, providers/audio.inc)
//           pFigCaptureSessionCreate, gSessSlots, for the session hook (fig/session_hook.inc), all defined in
//           core/preflight.h with the session slots
// Uses:     fig/sources.inc, core/act.inc
#import <mach-o/loader.h>
#import <mach-o/nlist.h>
#include "activation.h"
#include "preflight.h"
#include "../fig/sources.h"

// ---- runtime detection: FigCaptureSourceCopySources (up to 26.x) or -[FigCaptureSourceManager
// copySourcesWithType:] (27.x)
static CFArrayRef PreflightCopyStockSources(void) {
    CFArrayRef (*copySources)(int) = dlsym(RTLD_DEFAULT, "FigCaptureSourceCopySources");
    if (copySources) {
        return copySources(1);
    }
    Class managerClass = objc_getClass("FigCaptureSourceManager");
    SEL sharedSel = sel_registerName("sharedCaptureSourceManager");
    SEL copySel = sel_registerName("copySourcesWithType:");
    if (!managerClass || !class_getClassMethod(managerClass, sharedSel) ||
        !class_getInstanceMethod(managerClass, copySel)) {
        return NULL;
    }
    id manager = ((id (*)(id, SEL))objc_msgSend)(managerClass, sharedSel);
    // a "copy" method returns the array retained (+1)
    return manager ? (__bridge_retained CFArrayRef)((id (*)(id, SEL, int))objc_msgSend)(manager, copySel, 1) : NULL;
}

// ---- session vtable layout, checked by name BEFORE any session exists (the slot count grew from 19 to 24 in 27.0
// without a version bump). The simulator session class is the local symbol _kFigCaptureSessionSimulatorVTable in
// CMCapture; it is found in the loaded image's symbol table.

// Calls visit with each of a 64-bit image's load commands, in order, until it returns YES. Not async-signal-safe (it
// calls a block), so it is used at PREPARE time only.
static void MachOForEachCommand(const struct mach_header_64 *header,
                                BOOL (^visit)(const struct load_command *command)) {
    const struct load_command *command = (const void *)(header + 1);
    for (uint32_t k = 0; k < header->ncmds; k++, command = (const void *)((const char *)command + command->cmdsize)) {
        if (visit(command)) {
            return;
        }
    }
}

// The address of the local (non-exported) symbol symbolName in the loaded image whose path ends in imageSuffix, or
// NULL.
static const void *PreflightFindLocalSymbol(const char *imageSuffix, const char *symbolName) {
    size_t suffixLen = strlen(imageSuffix);

    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *imageName = _dyld_get_image_name(i);
        size_t nameLen = strlen(imageName);
        if (nameLen < suffixLen || strcmp(imageName + nameLen - suffixLen, imageSuffix)) {
            continue;
        }
        const struct mach_header_64 *header = (const void *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        __block const struct segment_command_64 *linkedit = NULL;
        __block const struct symtab_command *symtab = NULL;
        MachOForEachCommand(header, ^BOOL(const struct load_command *command) {   // keeps the last match of each
            if (command->cmd == LC_SEGMENT_64 &&
                !strcmp(((const struct segment_command_64 *)command)->segname, "__LINKEDIT")) {
                linkedit = (const void *)command;
            }
            if (command->cmd == LC_SYMTAB) {
                symtab = (const void *)command;
            }
            return NO;
        });
        if (!linkedit || !symtab) {
            return NULL;
        }
        uintptr_t base = slide + linkedit->vmaddr - linkedit->fileoff;
        const struct nlist_64 *symbols = (const void *)(base + symtab->symoff);
        const char *strings = (const char *)(base + symtab->stroff);
        for (uint32_t k = 0; k < symtab->nsyms; k++) {
            uint32_t strIndex = symbols[k].n_un.n_strx;
            if (!strIndex || strIndex >= symtab->strsize) {
                continue;
            }
            if (!strcmp(strings + strIndex, symbolName) && (symbols[k].n_type & N_TYPE) == N_SECT) {
                return (const void *)(symbols[k].n_value + slide);
            }
        }
        return NULL;
    }

    return NULL;
}

// The capture source class's slot 1, LockForConfiguration (src_Lock in fig/sources.inc's own source class).
enum { kSourceSlotLockForConfiguration = 1 };
// The session slots' names by index (NULL: not checked); a name is matched as a substring of the slot's symbol.
static const char *kPreflightSessionSlots[kSessNamedSlotCount] = {
    [kSessSlotSetSectionProperty] = "SetSectionProperty",
    [kSessSlotCopySectionProperty] = "CopySectionProperty",
    [kSessSlotSetConfiguration] = "SetConfiguration",
    [kSessSlotStartRunning] = "StartRunning",
    [kSessSlotStopRunning] = "StopRunning",
    [kSessSlotFileSinkStartRecording] = "FileSinkStartRecording",
    [kSessSlotFileSinkStopRecording] = "FileSinkStopRecording",
    [kSessSlotFileSinkPauseRecording] = "FileSinkPauseRecording",
    [kSessSlotFileSinkResumeRecording] = "FileSinkResumeRecording",
    [kSessSlotIrisCaptureImage] = "IrisStillImageSinkCaptureImage",
    [kSessSlotIrisPrepareToCapture] = "IrisStillImageSinkPrepareToCapture"
};

// nil if the session class's derived vtable derived has the expected version, slot names and a known slot count
// (stored in *slotsOut); else why not.
static NSString *PreflightCheckSessionSlots(const void *const *derived, int *slotsOut) {
    if ((uintptr_t)derived[kSessSlotVersion] != kFigDerivedClassVersion) {
        return [NSString stringWithFormat:@"session derived class version %lu (expected 1)",
                                          (unsigned long)(uintptr_t)derived[kSessSlotVersion]];
    }
    int lastSlot = 0;

    // slot 0 is the version word; the slots end at the first word that isn't one of the class's functions
    for (int i = 1; i < kSessSlotScanLimit; i++) {
        if (!derived[i]) {
            continue;
        }
        Dl_info symbolInfo = {0};
        if (!dladdr(derived[i], &symbolInfo) || !symbolInfo.dli_sname ||
            !strstr(symbolInfo.dli_sname, "captureSessionSimulator_")) {
            break;
        }
        lastSlot = i;
        if (i < kSessNamedSlotCount && kPreflightSessionSlots[i] &&
            !strstr(symbolInfo.dli_sname, kPreflightSessionSlots[i])) {
            return [NSString stringWithFormat:@"session slot %d is %s, expected %s", i, symbolInfo.dli_sname,
                    kPreflightSessionSlots[i]];
        }
    }

    for (int i = 1; i < kSessNamedSlotCount; i++) {
        if (kPreflightSessionSlots[i] && !derived[i]) {
            return [NSString stringWithFormat:@"session slot %d NULL, expected %s", i, kPreflightSessionSlots[i]];
        }
    }

    if (slotsOut) {
        *slotsOut = lastSlot;
    }

    // Only a known layout passes: an unknown count means Apple changed the table, so the session layer is refused.
    const char *knownCounts = getenv(ENV_SLOT_COUNTS) ?: "19,24";
    BOOL known = NO;
    for (NSString *count in [@(knownCounts) componentsSeparatedByString:@","]) {
        if (count.intValue == lastSlot) {
            known = YES;
        }
    }

    if (!known) {
        return [NSString stringWithFormat:@"session vtable has %d slots, known layouts are %s", lastSlot, knownCounts];
    }
    return nil;
}

// ---- preflight: returns YES only if every dependency of every hook looks as expected. ----
// Each step below runs its checks through `check`, which logs a failure and makes the preflight fail. The steps run in
// this order, and once the frameworks are loaded every check runs (a failed one doesn't stop the rest).
typedef void (^PreflightCheck)(BOOL ok, NSString *what);

// The CoreMedia and CMCapture symbols every hook needs; resolves the local-queue and session-create pointers.
static void PreflightCheckSymbols(PreflightCheck check, void *coreMedia, void *cmCapture) {
    const char *coreMediaSymbols[] = {"CMDerivedObjectCreate", "CMBaseObjectGetDerivedStorage", "CMBaseObjectGetVTable",
                                      "CMNotificationCenterPostNotification",
                                      "CMNotificationCenterGetDefaultLocalCenter", NULL};
    for (int i = 0; coreMediaSymbols[i]; i++) {
        check(dlsym(coreMedia, coreMediaSymbols[i]) != NULL,
              [NSString stringWithFormat:@"CoreMedia %s", coreMediaSymbols[i]]);
    }
    const char *cmCaptureSymbols[] = {"FigCaptureSourceGetClassID", "FigCaptureDeviceGetNotificationCenter",
                                      "FigLocalQueueCreate", "FigLocalQueueEnqueue", NULL};
    for (int i = 0; cmCaptureSymbols[i]; i++) {
        check(dlsym(cmCapture, cmCaptureSymbols[i]) != NULL,
              [NSString stringWithFormat:@"CMCapture %s", cmCaptureSymbols[i]]);
    }
    pFigLocalQueueCreate = dlsym(cmCapture, "FigLocalQueueCreate");
    pFigLocalQueueEnqueue = dlsym(cmCapture, "FigLocalQueueEnqueue");
    pFigCaptureSessionCreate = dlsym(cmCapture, "FigCaptureSessionCreate");   // only the opt-in GOT route needs it
}

// The Fig key constants exist with the expected string values. Returns whether the 27.x attributes model
// (FigCaptureSourceAttributes) is there.
static BOOL PreflightCheckKeys(PreflightCheck check, void *cmCapture) {
    NSDictionary *keys = @{
        @"kFigCaptureSourceAttributeKey_UniqueID": @"UniqueID",
        @"kFigCaptureSourceAttributeKey_DeviceType": @"DeviceType",
        @"kFigCaptureSourceAttributeKey_Position": @"Position",
        @"kFigCaptureSourceAttributeKey_Focus": @"Focus",
        @"kFigCaptureSourceProperty_AdjustingFocus": @"AdjustingFocus",
        @"kFigCaptureSourceNotificationKey_NewPropertyValue":
            gTestPreflightBreak ? @"NewPropertyValueV2" : @"NewPropertyValue",
        @"kFigCaptureSessionNotification_DidStartRunning": @"DidStartRunning",
        @"kFigCaptureSessionNotification_ConfigurationCommitted": @"ConfigurationCommitted",
        @"kFigCaptureSessionSinkNotification_LocalQueueUpdated": @"LocalQueueUpdated",
        @"kFigCaptureSessionLocalQueueUpdatedNotificationPayloadKey_LocalQueue": @"LocalQueue",
        @"kFigCaptureSessionNotificationPayloadKey_SectionID": @"SectionID",
        // the notification that makes AVF fire didFinishCaptureForResolvedSettings
        @"kFigCaptureSessionIrisStillImageSinkNotification_ReadyForResponsiveRequest": @"ReadyForResponsiveRequest"};
    BOOL fsaModel = class_getInstanceMethod(objc_getClass("FigCaptureSourceAttributes"),
                                            sel_registerName("initWithAttributesDictionary:")) != NULL;
    for (NSString *keyName in keys) {
        CFStringRef *symbol = dlsym(cmCapture, keyName.UTF8String);
        // 27.x no longer exports kFigCaptureSourceAttributeKey_* (attributes are parsed by FigCaptureSourceAttributes)
        if (!symbol && [keyName hasPrefix:@"kFigCaptureSourceAttributeKey_"] && fsaModel) {
            continue;
        }
        check(symbol && [(__bridge NSString *)*symbol isEqualToString:keys[keyName]],
              [NSString stringWithFormat:@"key %@ (=%@)", keyName, symbol ? (__bridge NSString *)*symbol : @"nosym"]);
    }

    return fsaModel;
}

// The source factories: the video device's class method and the format class. Returns the format class.
static Class PreflightCheckFactories(PreflightCheck check) {
    Class deviceClass = objc_getClass("AVCaptureFigVideoDevice");
    check(deviceClass && class_getClassMethod(deviceClass, sel_registerName("_newFigCaptureSources")),
          @"+[AVCaptureFigVideoDevice _newFigCaptureSources]");
    Class formatClass = objc_getClass("FigCaptureSourceVideoFormat");
    check(formatClass &&
              class_getInstanceMethod(formatClass, sel_registerName("initWithFigCaptureStreamFormatDictionary:")),
          @"-[FigCaptureSourceVideoFormat initWithFigCaptureStreamFormatDictionary:]");
    return formatClass;
}

// The session hook (objc route): its selector and ivars by name, and the session vtable's slot names and count.
static void PreflightCheckSessionHook(PreflightCheck check) {
    Class sessionClass = objc_getClass("AVCaptureSession");
    check(sessionClass && class_getInstanceMethod(sessionClass, sel_registerName("_createFigCaptureSession")),
          @"-[AVCaptureSession _createFigCaptureSession]");
    check(sessionClass && class_getInstanceVariable(sessionClass, "_internal"), @"AVCaptureSession._internal ivar");
    Class sessionInternalClass = objc_getClass("AVCaptureSessionInternal");
    Ivar figIvar = sessionInternalClass ? class_getInstanceVariable(sessionInternalClass, "figCaptureSession") : NULL;
    const char *figIvarType = figIvar ? ivar_getTypeEncoding(figIvar) : NULL;
    check(figIvar && figIvarType && (figIvarType[0] == '^' || figIvarType[0] == '@'),
          [NSString stringWithFormat:@"AVCaptureSessionInternal.figCaptureSession ivar (type %s)", figIvarType ?: "-"]);

    // the session vtable's slot names and count, before any session exists
    const BaseObjVTable *sessionVTable = PreflightFindLocalSymbol("/CMCapture", "_kFigCaptureSessionSimulatorVTable");

    if (sessionVTable && sessionVTable->derivedClass) {
        NSString *why = PreflightCheckSessionSlots(sessionVTable->derivedClass, &gSessSlots);
        check(!why, why ?: @"");
        check(sessionVTable->baseClass && sessionVTable->baseClass->version == kFigBaseClassVersion &&
                  sessionVTable->baseClass->finalize,
              @"session base class v2 with finalize (the session finalize wrapper)");
        if (!why) {
            LOG(@"preflight: session vtable %p has %d slots, names OK", sessionVTable, gSessSlots);
        }
    } else if (TestFaultOn(ENV_PF_NOSYMOK)) {
        LOG(@"preflight: _kFigCaptureSessionSimulatorVTable not found; slot check deferred to the first session "
            @"(SIMCAM_PF_NOSYMOK)");
    } else {
        check(NO, @"_kFigCaptureSessionSimulatorVTable not found in CMCapture's symbol table");
    }
}

static void PreflightCheckStockSource(PreflightCheck check, void *coreMedia, void *cmCapture) {
    // ABI canary 1: the stock simulator source's vtable shape (base class v2, derived class v1, slot names by dladdr)
    check(dlsym(cmCapture, "FigCaptureSourceCopySources") || objc_getClass("FigCaptureSourceManager"),
          @"no way to enumerate stock sources");
    const BaseObjVTable *(*getVTable)(CMBaseObjectRef) = dlsym(coreMedia, "CMBaseObjectGetVTable");

    if (getVTable) {
        CFArrayRef stockSources = PreflightCopyStockSources();
        if (stockSources && CFArrayGetCount(stockSources)) {
            const BaseObjVTable *vtable = getVTable(CFArrayGetValueAtIndex(stockSources, 0));
            Dl_info symbolInfo = {0};
            const void *const *derived = vtable->derivedClass;
            BOOL slotNamed = dladdr(derived[kSourceSlotLockForConfiguration], &symbolInfo) && symbolInfo.dli_sname &&
                             strstr(symbolInfo.dli_sname, "LockForConfiguration");
            // word 0 of a derived class is its version
            check(vtable->baseClass->version == kFigBaseClassVersion &&
                      (uintptr_t)derived[0] == kFigDerivedClassVersion && slotNamed,
                  [NSString stringWithFormat:@"source vtable shape base.v=%u derived.v=%lu slot1=%s",
                                             vtable->baseClass->version, (unsigned long)(uintptr_t)derived[0],
                                             symbolInfo.dli_sname ?: "?"]);
        } else {
            check(NO, @"stock source enumeration returned nothing");
        }
        if (stockSources) {
            CFRelease(stockSources);
        }
    }
}

// The round trip's pixel format: '420v' (0x34323076), the value of kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange.
// A plain int literal on purpose: CoreVideo's constant is an unsigned OSType, which would box to a different NSNumber
// (numberWithUnsignedInt:); @PIXEL_FORMAT_420V is the int literal @875704438.
#define PIXEL_FORMAT_420V 875704438

static void PreflightCheckRoundTrips(PreflightCheck check, Class formatClass, BOOL fsaModel) {
    // ABI canary 2: a format dictionary round-trips through Apple's parser
    @try {
        id format = [[formatClass alloc] performSelector:sel_registerName("initWithFigCaptureStreamFormatDictionary:")
                                              withObject:@{@"Name": @"pf", @"MediaType": @"vide", @"Width": @1280,
                                                           @"Height": @720, @"PixelFormatType": @PIXEL_FORMAT_420V,
                                                           @"VideoMinFrameRate": @1, @"VideoMaxFrameRate": @30}];
        float maxRate = ((float (*)(id, SEL))objc_msgSend)(format, sel_registerName("maxSupportedFrameRate"));
        check(maxRate == 30.0f, [NSString stringWithFormat:@"format round-trip maxSupportedFrameRate=%g", maxRate]);
    } @catch (NSException *exception) {
        check(NO, [NSString stringWithFormat:@"format round-trip threw %@", exception.reason]);
    }

    // ABI canary 3 (the 27.x model): an attributes dictionary round-trips through FigCaptureSourceAttributes
    if (fsaModel) {
        @try {
            id attributes = ((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("FigCaptureSourceAttributes") alloc],
                                                                sel_registerName("initWithAttributesDictionary:"),
                                                                @{@"UniqueID": @"pf", @"DeviceType": @2,
                                                                  @"Position": @1});
            id uniqueID = [attributes respondsToSelector:sel_registerName("uniqueID")]
                              ? ((id (*)(id, SEL))objc_msgSend)(attributes, sel_registerName("uniqueID"))
                              : nil;
            check(attributes && [uniqueID isEqual:@"pf"],
                  [NSString stringWithFormat:@"FigCaptureSourceAttributes round-trip uniqueID=%@", uniqueID]);
        } @catch (NSException *exception) {
            check(NO, [NSString stringWithFormat:@"attributes round-trip threw %@", exception.reason]);
        }
    }
}

// The rotation argument's index in -[AVCaptureVideoPreviewLayer didUpdatePreviewImageQueueSlot:imageQueue:
// rotationDegrees:size:] (after self, _cmd, the slot and the image queue).
enum { kPreviewHookRotationArg = 4 };
// The byte offsets client/photo.inc's photo fix reads: AVCapturePhoto._internal, and photoSurface in the
// AVCapturePhotoInternal object it points at.
enum { kPhotoInternalIvarOffset = 8, kPhotoSurfaceIvarOffset = 0x20 };

// Every client-side selector the engine replaces exists.
static void PreflightCheckClientMethods(PreflightCheck check) {
    struct {
        const char *cls;
        const char *sel;
    } methods[] = {
        {"AVCapturePhoto", "fileDataRepresentation"},
        {"AVCapturePhoto", "fileDataRepresentationWithCustomizer:"},
        {"AVCapturePhoto", "CGImageRepresentation"},
        {"AVCaptureVideoPreviewLayer", "didUpdatePreviewImageQueueSlot:imageQueue:rotationDegrees:size:"},
        {"AVCaptureDeviceRotationCoordinator", "initWithDevice:previewLayer:"},
        {"AVCaptureDeviceRotationCoordinator", "videoRotationAngleForHorizonLevelPreview"},
        {"AVCaptureDeviceRotationCoordinator", "videoRotationAngleForHorizonLevelCapture"},
    };

    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        Class cls = objc_getClass(methods[i].cls);
        check(cls && class_getInstanceMethod(cls, sel_registerName(methods[i].sel)),
              [NSString stringWithFormat:@"-[%s %s]", methods[i].cls, methods[i].sel]);
    }
}

// The microphone path (the engine's own audio source and isConnected): needed on 17.5, checked everywhere.
static void PreflightCheckAudioDevice(PreflightCheck check) {
    Class audioDeviceClass = objc_getClass("AVCaptureFigAudioDevice");
    check(audioDeviceClass && class_getClassMethod(audioDeviceClass, sel_registerName("_newFigCaptureSources")) &&
              class_getInstanceMethod(audioDeviceClass, sel_registerName("isConnected")) &&
              class_getInstanceMethod(audioDeviceClass, sel_registerName("figCaptureSource")),
          @"AVCaptureFigAudioDevice +_newFigCaptureSources / -isConnected / -figCaptureSource");
}

// The recommended-settings fix: the 7-argument form (26, 27) or the 6-argument form (17.5).
static void PreflightCheckRecommendedSettings(PreflightCheck check) {
    Class outputClass = objc_getClass("AVCaptureOutput");
    check(class_getInstanceMethod(outputClass, sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:"
                                                                "videoCodecType:fileType:isIris:outputFileURL:"
                                                                "spatialAudioChannelLayoutTag:")) ||
              class_getInstanceMethod(outputClass,
                                      sel_registerName("recommendedOutputSettingsForConnection:sourceSettings:"
                                                       "videoCodecType:fileType:isIris:outputFileURL:")),
          @"-[AVCaptureOutput recommendedOutputSettingsForConnection:...] (6 or 7 args)");
}

// The preview hook reads its rotation argument as a floating-point number.
static void PreflightCheckPreviewRotationArg(PreflightCheck check) {
    Method previewMethod = class_getInstanceMethod(objc_getClass("AVCaptureVideoPreviewLayer"),
                                                   sel_registerName("didUpdatePreviewImageQueueSlot:imageQueue:"
                                                                    "rotationDegrees:size:"));
    if (!previewMethod) {
        return;   // PreflightCheckClientMethods reports the missing method
    }
    char argType[8] = {0};
    method_getArgumentType(previewMethod, kPreviewHookRotationArg, argType, sizeof argType);
    check(argType[0] == 'd' || argType[0] == 'f',
          [NSString stringWithFormat:@"preview hook rotation arg type %s", argType]);
}

// The photo surface: client/photo.inc's photo fix reads AVCapturePhoto._internal and its photoSurface by offset.
static void PreflightCheckPhotoSurface(PreflightCheck check) {
    Ivar photoInternal = class_getInstanceVariable(objc_getClass("AVCapturePhoto"), "_internal");
    Ivar photoSurface = class_getInstanceVariable(objc_getClass("AVCapturePhotoInternal"), "photoSurface");
    check(photoInternal && ivar_getOffset(photoInternal) == kPhotoInternalIvarOffset && photoSurface &&
              ivar_getOffset(photoSurface) == kPhotoSurfaceIvarOffset,
          [NSString stringWithFormat:@"AVCapturePhoto._internal@%td photoSurface@%td (expected 8, 32)",
                                     photoInternal ? ivar_getOffset(photoInternal) : -1,
                                     photoSurface ? ivar_getOffset(photoSurface) : -1]);
}

// The settings objects: the rotation keys (degrees on 26 and 27, an enum on 17.5) and the KVC keys the session layer
// reads, and the configuration classes it reads them from.
static void PreflightCheckSettingsClasses(PreflightCheck check) {
    Class stillSettingsClass = objc_getClass("FigCaptureIrisStillImageSettings");
    Class movieSettingsClass = objc_getClass("FigCaptureMovieFileRecordingSettings");
    check(stillSettingsClass &&
              (class_getInstanceMethod(stillSettingsClass, sel_registerName("outputRotationDegrees")) ||
               class_getInstanceMethod(stillSettingsClass, sel_registerName("outputOrientation"))) &&
              class_getInstanceMethod(stillSettingsClass, sel_registerName("settingsID")),
          @"FigCaptureIrisStillImageSettings rotation/settingsID");
    check(movieSettingsClass &&
              (class_getInstanceMethod(movieSettingsClass, sel_registerName("videoRotationDegrees")) ||
               class_getInstanceMethod(movieSettingsClass, sel_registerName("videoOrientation"))) &&
              class_getInstanceMethod(movieSettingsClass, sel_registerName("outputURL")),
          @"FigCaptureMovieFileRecordingSettings rotation/outputURL");
    const char *configClasses[] = {"FigCaptureSessionConfiguration", "FigCaptureVideoDataSinkConfiguration",
                                   "FigCaptureVideoPreviewSinkConfiguration", "FigCaptureIrisSinkConfiguration",
                                   "FigCaptureMovieFileSinkConfiguration", "FigCaptureMetadataObjectSinkConfiguration",
                                   NULL};
    for (int i = 0; configClasses[i]; i++) {
        check(objc_getClass(configClasses[i]) != Nil, [NSString stringWithFormat:@"class %s", configClasses[i]]);
    }
}

// The client-side hooks: every selector, argument type and ivar offset the engine relies on, in this order.
static void PreflightCheckClientHooks(PreflightCheck check) {
    PreflightCheckClientMethods(check);
    PreflightCheckAudioDevice(check);
    PreflightCheckRecommendedSettings(check);
    PreflightCheckPreviewRotationArg(check);
    PreflightCheckPhotoSurface(check);
    PreflightCheckSettingsClasses(check);
}

static BOOL PreflightRun(void) {
    __block BOOL ok = YES;
    PreflightCheck check = ^(BOOL passed, NSString *what) {
        if (!passed) {
            ok = NO;
            LOG(@"PREFLIGHT FAIL: %@", what);
        }
    };

    void *coreMedia = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
    void *cmCapture = dlopen("/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW);
    check(coreMedia && cmCapture, @"dlopen CoreMedia/CMCapture");
    if (!coreMedia || !cmCapture) {
        return NO;
    }
    PreflightCheckSymbols(check, coreMedia, cmCapture);
    BOOL fsaModel = PreflightCheckKeys(check, cmCapture);
    Class formatClass = PreflightCheckFactories(check);
    PreflightCheckSessionHook(check);
    PreflightCheckStockSource(check, coreMedia, cmCapture);
    PreflightCheckRoundTrips(check, formatClass, fsaModel);
    PreflightCheckClientHooks(check);
    LOG(@"PREFLIGHT %@", ok ? @"PASS" : @"FAIL — leaving stock AVFoundation behaviour");
    return ok;
}
