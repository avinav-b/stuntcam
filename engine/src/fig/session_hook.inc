// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): session adoption, teardown, session-creation hook.
static void M5AdoptSession(CMBaseObjectRef obj, const char *via);
static BOOL gSessBad;
static M2BaseClass gSessBase; static void (*gOrigSessFinalize)(CMBaseObjectRef); static char gPumpQKey;
static void M10ForgetSession(CMBaseObjectRef obj) {
    M5Sess *S; @synchronized (gSessions) { S = gSessions[[NSValue valueWithPointer:obj]]; [gSessions removeObjectForKey:[NSValue valueWithPointer:obj]]; }
    if (!S) return;
    // on the pump queue, synchronously: a tick in flight must not touch the object after finalize returns
    void (^b)(void) = ^{ S.running = NO; if (S.timer) { dispatch_source_cancel(S.timer); S.timer = nil; } M5UpdateMic(S);
                         for (M5Sink *k in S.sinks) if (k.queue) { CFRelease(k.queue); k.queue = NULL; } S.obj = NULL; };
    if (dispatch_get_specific(&gPumpQKey)) b(); else dispatch_sync(gPumpQ, b);
}
static void sess_Finalize(CMBaseObjectRef obj) {
    LOG(@"session %p finalize -> forgotten", obj);
    M10ForgetSession(obj);
    M40Forget(obj);
    if (gOrigSessFinalize) gOrigSessFinalize(obj);
}
static int gSessHookMode; // 1 objc (_createFigCaptureSession swizzle, default), 2 got rebind (M6_SESSHOOK=got). m10: no __interpose.
static OSStatus m5_FigCaptureSessionCreate(CFAllocatorRef a, void *b, void *c, CMBaseObjectRef *out) {   // GOT route only
    OSStatus err = pFigCaptureSessionCreate(a, b, c, out);
    LOG(@"FigCaptureSessionCreate -> %d obj=%p", (int)err, out ? *out : NULL);
    if (err || !out || !*out) return err;
    M5AdoptSession(*out, "FigCaptureSessionCreate GOT hook");
    return err;
}
static void M5AdoptSession(CMBaseObjectRef obj, const char *via) {
    CMBaseObjectRef *out = &obj; OSStatus err = 0;
    if (M5S(obj)) {
        // m10: a FigCaptureSession can be allocated at the address of one that was just freed (seen on 27.0 in Flutter's back->front switch).
        // Ours iff its vtable is already ours; otherwise the entry is stale (pre-finalize-hook leftover) -> drop it and adopt the new object.
        const M2VTable *cur = ((const M2VTable *(*)(CMBaseObjectRef))dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable"))(obj);
        if (cur == &gSessVTable) { LOG(@"session %p already adopted (%s)", obj, via); return; }
        LOG(@"session %p: stale entry for a reused address (vtable %p is not ours) — re-adopting", obj, cur);
        M10ForgetSession(obj);
    }
    LOG(@"adopting session %p via %s", obj, via);
    static dispatch_once_t once;
    const M2VTable *vt = ((const M2VTable *(*)(CMBaseObjectRef))dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable"))(*out);
    dispatch_once(&once, ^{
        gSessQ = dispatch_queue_create("m5.session.notify", 0);
        gPumpQ = dispatch_queue_create("m5.pump", 0); dispatch_queue_set_specific(gPumpQ, &gPumpQKey, &gPumpQKey, NULL);
        gSessions = [NSMutableDictionary dictionary];
        pCMNotificationCenterGetDefaultLocalCenter = dlsym(RTLD_DEFAULT, "CMNotificationCenterGetDefaultLocalCenter");
        if (!pCMNotificationCenterPostNotification) pCMNotificationCenterPostNotification = dlsym(RTLD_DEFAULT, "CMNotificationCenterPostNotification");
        void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
        pCAImageQueueCreate = dlsym(qc, "CAImageQueueCreate");
        pCAImageQueueRegisterIOSurfaceBuffer = dlsym(qc, "CAImageQueueRegisterIOSurfaceBuffer");
        pCAImageQueueInsertImage = dlsym(qc, "CAImageQueueInsertImage");
        // m9/m10: copy exactly the class's slots (19 on 17.5/26.3, 24 on 27.0) after checking the names; refuse on a mismatch
        int n = 0; NSString *why = M10CheckSessionDerived(vt->derivedClass, &n);
        if (why) { LOG(@"MISMATCH: %@ — not repointing any session", why); gSessBad = YES; return; }
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
        // m10: wrap the base class's finalize so a dying session is forgotten (stops its pump/mic; fixes address-reuse + m6's gSessions leak)
        gSessBase = *vt->baseClass; gOrigSessFinalize = vt->baseClass->finalize;
        if (!getenv("M10_NO_FINALIZE") && vt->baseClass->version == 2) gSessBase.finalize = sess_Finalize;
        gSessVTable = (M2VTable){ vt->reserved, getenv("M10_NO_FINALIZE") ? vt->baseClass : &gSessBase, gSessDerived };
    });
    if (gSessBad) return;
    M5Sess *S = [M5Sess new]; S.obj = *out; S.recs = [NSMutableDictionary dictionary]; S.sinks = [NSMutableArray array];
    @synchronized (gSessions) { gSessions[[NSValue valueWithPointer:*out]] = S; }
    const void **p = (const void **)*out;
    for (int i = 0; i < 8; i++) {
        if (p[i] == vt) { p[i] = &gSessVTable; LOG(@"session vtable repointed at word %d", i); (void)err; return; }
    }
    LOG(@"MISMATCH: session vtable pointer not found in object");
}

// ---- m6 (Q7): __interpose only applies when this dylib is present at launch (DYLD_INSERT_LIBRARIES). When a stub dlopen()s us,
// it is silently ignored. Two replacements:
//  objc: swizzle -[AVCaptureSession _createFigCaptureSession] (the only caller of FigCaptureSessionCreate in AVFCapture), call the
//        original, then find the new FigCaptureSession in AVCaptureSessionInternal's ivars (validated by its vtable's slot names).
//  got:  rebind AVFCapture's GOT entry for FigCaptureSessionCreate (value scan of __got/__auth_got), fishhook-style.
#import <mach-o/loader.h>
#import <mach/mach.h>
#import <malloc/malloc.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
static BOOL M6IsSimSession(const void *p) {
    if (!p || (uintptr_t)p < 0x100000000ULL || ((uintptr_t)p & 7) || !malloc_zone_from_ptr(p)) return NO;
    static const M2VTable *(*getVT)(CMBaseObjectRef); if (!getVT) getVT = dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    const M2VTable *vt = *(const M2VTable **)((const char *)p + 16); // fast reject: CF objects hold the vtable in their first words
    (void)vt;
    @try {
        if (CFGetTypeID(p) == 0) return NO;
        const M2VTable *v = getVT(p);
        if (!v || !v->derivedClass) return NO;
        if (v == &gSessVTable) return YES;
        Dl_info di = {0};
        return dladdr(((void *const *)v->derivedClass)[3], &di) && di.dli_sname && strstr(di.dli_sname, "captureSessionSimulator_SetConfiguration");
    } @catch (id e) { return NO; }
}
static CMBaseObjectRef M6FindFigSession(id avSession) {
    Ivar iv = class_getInstanceVariable(object_getClass(avSession), "_internal");
    id internal = iv ? object_getIvar(avSession, iv) : nil;
    if (!internal) { LOG(@"objc route: no _internal ivar"); return NULL; }
    static BOOL dumped; unsigned n = 0; Ivar *ivs = class_copyIvarList(object_getClass(internal), &n);
    CMBaseObjectRef found = NULL; NSMutableArray *names = [NSMutableArray array];
    for (unsigned i = 0; i < n; i++) {
        const char *nm = ivar_getName(ivs[i]), *ty = ivar_getTypeEncoding(ivs[i]); ptrdiff_t off = ivar_getOffset(ivs[i]);
        if (!dumped) [names addObject:[NSString stringWithFormat:@"%s %s @%td", nm, ty, off]];
        if (!found && ty && (ty[0] == '^' || ty[0] == '@') && strcasestr(nm, "figCaptureSession") && !strcasestr(nm, "Configuration")) {
            const void *v = *(const void **)((char *)(__bridge void *)internal + off);
            if (M6IsSimSession(v)) { found = v; LOG(@"objc route: fig session in ivar %s (%s) @%td = %p", nm, ty, off, v); }
        }
    }
    free(ivs);
    if (!dumped) { dumped = YES; LOG(@"AVCaptureSessionInternal ivars: %@", [names componentsJoinedByString:@", "]); }
    return found;
}
static IMP gOrigCreateFig;
static void m6_createFigCaptureSession(id self, SEL c) {
    ((void (*)(id, SEL))gOrigCreateFig)(self, c);
    CMBaseObjectRef s = M6FindFigSession(self);
    if (s) M5AdoptSession(s, "_createFigCaptureSession swizzle"); else LOG(@"MISMATCH: objc route found no FigCaptureSession");
}
// W4: the GOT route is split. PREPARE 4 walks the images and records the slots (M6FindGOTSlots, logged there); SWAP only does
// vm_protect and the store (M6GOTSwap). No image walk after ARMED.
#define M6_MAXGOT 8
static void **gGotSlots[M6_MAXGOT]; static int gGotN, gGotPatched;
static int M6FindGOTSlots(const char *imageSuffix, void *target) {
    gGotN = 0; size_t sl = strlen(imageSuffix);
    size_t cacheLen = 0; const void *cache = _dyld_get_shared_cache_range(&cacheLen);
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i); size_t l = strlen(nm);
        if (l < sl || strcmp(nm + l - sl, imageSuffix)) continue;
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i); intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        BOOL inCache = cache && (const char *)mh >= (const char *)cache && (const char *)mh < (const char *)cache + cacheLen;
        LOG(@"got route: %s header %p in shared cache: %d", nm, mh, inCache);
        const struct load_command *lc = (const void *)(mh + 1);
        for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *seg = (const void *)lc; const struct section_64 *sec = (const void *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; j++, sec++) {
                if (strncmp(sec->sectname, "__got", 16) && strncmp(sec->sectname, "__auth_got", 16)) continue;
                void **p = (void **)(sec->addr + slide); size_t cnt = sec->size / sizeof(void *);
                for (size_t q = 0; q < cnt; q++) if (p[q] == target && gGotN < M6_MAXGOT) {
                    gGotSlots[gGotN++] = &p[q];
                    LOG(@"got route: %.16s,%.16s[%zu] at %p = %p (prepare: found, not yet patched)", seg->segname, sec->sectname, q, &p[q], p[q]);
                }
            }
        }
    }
    return gGotN;
}
static void M6GOTSwap(void) {   // SWAP 3, GOT route: vm_protect + store only
    gGotPatched = 0;
    for (int i = 0; i < gGotN; i++) {
        vm_address_t pg = (vm_address_t)gGotSlots[i] & ~(vm_page_size - 1);
        kern_return_t kr = vm_protect(mach_task_self(), pg, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (kr == KERN_SUCCESS) { *gGotSlots[i] = (void *)m5_FigCaptureSessionCreate; vm_protect(mach_task_self(), pg, vm_page_size, 0, VM_PROT_READ); gGotPatched++; }
    }
}
// PREPARE 4: the session hook (objc route: -[AVCaptureSession _createFigCaptureSession]; got: the slots)
static BOOL M6SessResolve(void) {
    if (!getenv("M2_SESSION_HOOK") || !getenv("M2_FAKE")) return YES;
    const char *m = getenv("M6_SESSHOOK"); gSessHookMode = m && !strcmp(m, "got") ? 2 : 1;
    if (gSessHookMode == 1) return ActHookResolve(HG_SESSION, "session hook", objc_getClass("AVCaptureSession"), "_createFigCaptureSession", NO, NO, (IMP)m6_createFigCaptureSession, &gOrigCreateFig, YES);
    if (!pFigCaptureSessionCreate) { snprintf(gActMissing, sizeof gActMissing, "session hook (got): the symbol FigCaptureSessionCreate"); return NO; }
    if (!M6FindGOTSlots("/AVFCapture", (void *)pFigCaptureSessionCreate)) { snprintf(gActMissing, sizeof gActMissing, "session hook (got): an AVFCapture GOT slot for FigCaptureSessionCreate"); return NO; }
    return YES;
}
