// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): session adoption, teardown, session-creation hook.
static void SessAdopt(CMBaseObjectRef obj, const char *via);
static BOOL gSessBad;
static BaseObjClass gSessBase;
static void (*gOrigSessFinalize)(CMBaseObjectRef);
static char gPumpQKey;
// on the pump queue: the session stops running (its pump timer cancelled, its microphone updated)
static void SessStopPump(SimcamSession *S) {
    S.running = NO;
    if (S.timer) {
        dispatch_source_cancel(S.timer);
        S.timer = nil;
    }
    MicUpdate(S);
}
static void SessForget(CMBaseObjectRef obj) {
    SimcamSession *S;
    @synchronized (gSessions) {
        S = gSessions[[NSValue valueWithPointer:obj]];
        [gSessions removeObjectForKey:[NSValue valueWithPointer:obj]];
    }
    if (!S) return;
    // on the pump queue, synchronously: a tick in flight must not touch the object after finalize returns
    void (^forget)(void) = ^{
        SessStopPump(S);
        for (SimcamSink *k in S.sinks) {
            if (k.queue) {
                CFRelease(k.queue);
                k.queue = NULL;
            }
        }
        S.obj = NULL;
    };
    if (dispatch_get_specific(&gPumpQKey)) forget();
    else dispatch_sync(gPumpQ, forget);
}
static void sess_Finalize(CMBaseObjectRef obj) {
    LOG(@"session %p finalize -> forgotten", obj);
    SessForget(obj);
    StopWaitForget(obj);
    if (gOrigSessFinalize) gOrigSessFinalize(obj);
}
// which session hook PREPARE 4 resolved (0: none, the session layer is off). No __interpose.
enum {
    SESS_HOOK_OBJC = 1,   // the _createFigCaptureSession swizzle (default)
    SESS_HOOK_GOT = 2,    // AVFCapture's GOT entry for FigCaptureSessionCreate rebound (SIMCAM_SESSION_ROUTE=got)
};
static int gSessHookMode;
// GOT route only
static OSStatus hook_FigCaptureSessionCreate(CFAllocatorRef a, void *b, void *c, CMBaseObjectRef *out) {
    OSStatus err = pFigCaptureSessionCreate(a, b, c, out);
    LOG(@"FigCaptureSessionCreate -> %d obj=%p", (int)err, out ? *out : NULL);
    if (err || !out || !*out) return err;
    SessAdopt(*out, "FigCaptureSessionCreate GOT hook");
    return err;
}
// an entry for obj already: ours if its vtable is ours (YES: nothing to do); else a stale entry, forgotten (NO)
static BOOL SessionAlreadyAdopted(CMBaseObjectRef obj, const char *via) {
    if (!SessLookup(obj)) return NO;
    // A FigCaptureSession can be allocated at the address of one that was just freed (seen on 27.0 in Flutter's
    // back->front switch). Ours iff its vtable is already ours; otherwise the entry is stale (its finalize wasn't
    // seen) -> drop it and adopt the new object.
    const BaseObjVTable *cur =
        ((const BaseObjVTable *(*)(CMBaseObjectRef))dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable"))(obj);
    if (cur == &gSessVTable) {
        LOG(@"session %p already adopted (%s)", obj, via);
        return YES;
    }
    LOG(@"session %p: stale entry for a reused address (vtable %p is not ours) — re-adopting", obj, cur);
    SessForget(obj);
    return NO;
}
// once, at the first adoption: the session queues and table, the notification and CAImageQueue symbols, and our copy
// of the simulator session's vtable (gSessVTable); a slot-name mismatch sets gSessBad and repoints nothing
static void SessionLayerSetUp(const BaseObjVTable *vt) {
    gSessQ = dispatch_queue_create("simcam.session.notify", 0);
    gPumpQ = dispatch_queue_create("simcam.pump", 0);
    dispatch_queue_set_specific(gPumpQ, &gPumpQKey, &gPumpQKey, NULL);
    gSessions = [NSMutableDictionary dictionary];
    pCMNotificationCenterGetDefaultLocalCenter = dlsym(RTLD_DEFAULT, "CMNotificationCenterGetDefaultLocalCenter");
    if (!pCMNotificationCenterPostNotification)
        pCMNotificationCenterPostNotification = dlsym(RTLD_DEFAULT, "CMNotificationCenterPostNotification");
    void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
    pCAImageQueueCreate = dlsym(qc, "CAImageQueueCreate");
    pCAImageQueueRegisterIOSurfaceBuffer = dlsym(qc, "CAImageQueueRegisterIOSurfaceBuffer");
    pCAImageQueueInsertImage = dlsym(qc, "CAImageQueueInsertImage");
    // copy exactly the class's slots (19 on 17.5/26.3, 24 on 27.0) after checking the names; refuse on a mismatch
    int n = 0;
    NSString *why = PreflightCheckSessionSlots(vt->derivedClass, &n);
    if (why) {
        LOG(@"MISMATCH: %@ — not repointing any session", why);
        gSessBad = YES;
        return;
    }
    memcpy(gSessDerived, vt->derivedClass, (n + 1) * sizeof(void *));
    memcpy(gOrigDerived, vt->derivedClass, (n + 1) * sizeof(void *));
    LOG(@"session vtable: %d slots copied (preflight saw %d)", n, gSessSlots);
    gSessDerived[1] = (void *)sess_SetSectionProperty;
    gSessDerived[2] = (void *)sess_CopySectionProperty;
    gSessDerived[3] = (void *)sess_SetConfiguration;
    gSessDerived[4] = (void *)sess_StartRunning;
    gSessDerived[5] = (void *)sess_StopRunning;
    gSessDerived[6] = (void *)sess_FileStart;
    gSessDerived[7] = (void *)sess_FileStop;
    gSessDerived[8] = (void *)sess_FilePause;
    gSessDerived[9] = (void *)sess_FileResume;
    gSessDerived[12] = (void *)sess_IrisCapture;
    gSessDerived[13] = (void *)sess_IrisPrepare;
    // wrap the base class's finalize so a dying session is forgotten (stops its pump/mic; handles address reuse;
    // gSessions doesn't leak)
    gSessBase = *vt->baseClass;
    gOrigSessFinalize = vt->baseClass->finalize;
    if (!getenv(ENV_OFF_SESSION_FINALIZE) && vt->baseClass->version == 2) gSessBase.finalize = sess_Finalize;
    gSessVTable = (BaseObjVTable){ vt->reserved, getenv(ENV_OFF_SESSION_FINALIZE) ? vt->baseClass : &gSessBase,
                              gSessDerived };
}
// point obj's vtable word (one of its first 8) at gSessVTable
static void SessionRepoint(CMBaseObjectRef obj, const BaseObjVTable *vt) {
    const void **words = (const void **)obj;
    for (int i = 0; i < 8; i++) {
        if (words[i] == vt) {
            words[i] = &gSessVTable;
            LOG(@"session vtable repointed at word %d", i);
            return;
        }
    }
    LOG(@"MISMATCH: session vtable pointer not found in object");
}
static void SessAdopt(CMBaseObjectRef obj, const char *via) {
    if (SessionAlreadyAdopted(obj, via)) return;   // (it also forgets a stale entry for a reused address)
    LOG(@"adopting session %p via %s", obj, via);
    static dispatch_once_t once;
    const BaseObjVTable *vt =
        ((const BaseObjVTable *(*)(CMBaseObjectRef))dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable"))(obj);
    dispatch_once(&once, ^{
        SessionLayerSetUp(vt);
    });
    if (gSessBad) return;
    SimcamSession *S = [SimcamSession new];
    S.obj = obj;
    S.recs = [NSMutableDictionary dictionary];
    S.sinks = [NSMutableArray array];
    @synchronized (gSessions) {
        gSessions[[NSValue valueWithPointer:obj]] = S;
    }
    SessionRepoint(obj, vt);
}

// ---- __interpose only applies when this dylib is present at launch (DYLD_INSERT_LIBRARIES). When a stub dlopen()s us,
// it is silently ignored. Two replacements:
//  objc: swizzle -[AVCaptureSession _createFigCaptureSession] (the only caller of FigCaptureSessionCreate in
//        AVFCapture), call the original, then find the new FigCaptureSession in AVCaptureSessionInternal's ivars
//        (validated by its vtable's slot names).
//  got:  rebind AVFCapture's GOT entry for FigCaptureSessionCreate (value scan of __got/__auth_got), fishhook-style.
#import <mach-o/loader.h>
#import <mach/mach.h>
#import <malloc/malloc.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
static BOOL SessIsSimulatorSession(const void *p) {
    if (!p || (uintptr_t)p < 0x100000000ULL || ((uintptr_t)p & 7) || !malloc_zone_from_ptr(p)) return NO;
    static const BaseObjVTable *(*getVT)(CMBaseObjectRef);
    if (!getVT) getVT = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    @try {
        if (CFGetTypeID(p) == 0) return NO;
        const BaseObjVTable *v = getVT(p);
        if (!v || !v->derivedClass) return NO;
        if (v == &gSessVTable) return YES;
        Dl_info di = {0};
        return dladdr(((void *const *)v->derivedClass)[3], &di) && di.dli_sname &&
               strstr(di.dli_sname, "captureSessionSimulator_SetConfiguration");
    } @catch (id e) {
        return NO;
    }
}
static CMBaseObjectRef SessFindFig(id avSession) {
    Ivar iv = class_getInstanceVariable(object_getClass(avSession), "_internal");
    id internal = iv ? object_getIvar(avSession, iv) : nil;
    if (!internal) {
        LOG(@"objc route: no _internal ivar");
        return NULL;
    }
    static BOOL dumped;
    unsigned n = 0;
    Ivar *ivars = class_copyIvarList(object_getClass(internal), &n);
    CMBaseObjectRef found = NULL;
    NSMutableArray *names = [NSMutableArray array];
    for (unsigned i = 0; i < n; i++) {
        const char *name = ivar_getName(ivars[i]), *type = ivar_getTypeEncoding(ivars[i]);
        ptrdiff_t off = ivar_getOffset(ivars[i]);
        if (!dumped) [names addObject:[NSString stringWithFormat:@"%s %s @%td", name, type, off]];
        if (!found && type && (type[0] == '^' || type[0] == '@') && strcasestr(name, "figCaptureSession") &&
            !strcasestr(name, "Configuration")) {
            const void *v = *(const void **)((char *)(__bridge void *)internal + off);
            if (SessIsSimulatorSession(v)) {
                found = v;
                LOG(@"objc route: fig session in ivar %s (%s) @%td = %p", name, type, off, v);
            }
        }
    }
    free(ivars);
    if (!dumped) {
        dumped = YES;
        LOG(@"AVCaptureSessionInternal ivars: %@", [names componentsJoinedByString:@", "]);
    }
    return found;
}
static IMP gOrigCreateFig;
static void hook_createFigCaptureSession(id self, SEL c) {
    ((void (*)(id, SEL))gOrigCreateFig)(self, c);
    CMBaseObjectRef s = SessFindFig(self);
    if (s) SessAdopt(s, "_createFigCaptureSession swizzle");
    else LOG(@"MISMATCH: objc route found no FigCaptureSession");
}
// The GOT route is split. PREPARE 4 walks the images and records the slots (SessFindGOTSlots, logged there); SWAP only
// does vm_protect and the store (SessGOTSwap). No image walk after ARMED.
#define SESS_MAXGOT 8
static void **gGotSlots[SESS_MAXGOT];
static int gGotN, gGotPatched;
static int SessFindGOTSlots(const char *imageSuffix, void *target) {
    gGotN = 0;
    size_t suffixLen = strlen(imageSuffix);
    size_t cacheLen = 0;
    const void *cache = _dyld_get_shared_cache_range(&cacheLen);
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *imageName = _dyld_get_image_name(i);
        size_t nameLen = strlen(imageName);
        if (nameLen < suffixLen || strcmp(imageName + nameLen - suffixLen, imageSuffix)) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        BOOL inCache =
            cache && (const char *)mh >= (const char *)cache && (const char *)mh < (const char *)cache + cacheLen;
        LOG(@"got route: %s header %p in shared cache: %d", imageName, mh, inCache);
        MachOForEachCommand(mh, ^BOOL(const struct load_command *lc) {
            if (lc->cmd != LC_SEGMENT_64) return NO;
            const struct segment_command_64 *seg = (const void *)lc;
            const struct section_64 *sec = (const void *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; j++, sec++) {
                if (strncmp(sec->sectname, "__got", 16) && strncmp(sec->sectname, "__auth_got", 16)) continue;
                void **entries = (void **)(sec->addr + slide);
                size_t count = sec->size / sizeof(void *);
                for (size_t q = 0; q < count; q++) {
                    if (entries[q] == target && gGotN < SESS_MAXGOT) {
                        gGotSlots[gGotN++] = &entries[q];
                        LOG(@"got route: %.16s,%.16s[%zu] at %p = %p (prepare: found, not yet patched)", seg->segname,
                            sec->sectname, q, &entries[q], entries[q]);
                    }
                }
            }
            return NO;
        });
    }
    return gGotN;
}
static void SessGOTSwap(void) {   // SWAP 3, GOT route: vm_protect + store only
    gGotPatched = 0;
    for (int i = 0; i < gGotN; i++) {
        vm_address_t page = (vm_address_t)gGotSlots[i] & ~(vm_page_size - 1);
        kern_return_t kr =
            vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (kr == KERN_SUCCESS) {
            *gGotSlots[i] = (void *)hook_FigCaptureSessionCreate;
            vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ);
            gGotPatched++;
        }
    }
}
// PREPARE 4: the session hook (objc route: -[AVCaptureSession _createFigCaptureSession]; got: the slots)
static BOOL SessResolveHook(void) {
    if (!EnvConfig(ENV_FIG_SESSION, ENV_LEGACY_FIG_SESSION) || !EnvConfig(ENV_FIG_SOURCES, ENV_LEGACY_FIG_SOURCES))
        return YES;
    const char *mode = getenv(ENV_SESSION_ROUTE);
    gSessHookMode = mode && !strcmp(mode, "got") ? SESS_HOOK_GOT : SESS_HOOK_OBJC;
    if (gSessHookMode == SESS_HOOK_OBJC)
        return ActHookResolve(HG_SESSION, "session hook", objc_getClass("AVCaptureSession"), "_createFigCaptureSession",
                              NO, NO, (IMP)hook_createFigCaptureSession, &gOrigCreateFig, YES);
    if (!pFigCaptureSessionCreate) {
        snprintf(gActMissing, sizeof gActMissing, "session hook (got): the symbol FigCaptureSessionCreate");
        return NO;
    }
    if (!SessFindGOTSlots("/AVFCapture", (void *)pFigCaptureSessionCreate)) {
        snprintf(gActMissing, sizeof gActMissing,
                 "session hook (got): an AVFCapture GOT slot for FigCaptureSessionCreate");
        return NO;
    }
    return YES;
}
