// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): session adoption, teardown, session-creation hook.
//
// Provides: SessResolveHook, SessGOTSwap, kSessionHook* (core/lazy.inc)
//           SessStopPump (core/control.inc)
// Owns:     gSessionHook, the route chosen, the session's base class and the GOT slots, declared in
//           fig/session_hook.h (read by core/lazy.inc)
// Uses:     fig/sources.inc, core/act.inc, core/preflight.inc, fig/session_state.inc (makes its queues and table),
//           providers/video.inc, providers/audio.inc, providers/pump.inc, providers/photo.inc
#include "session_hook.h"
#include "../core/activation.h"
#include "session.h"
#include "../providers/audio.h"
#include "../core/preflight.h"
#include "sources.h"
#include "../providers/pump.h"
#include "../providers/photo.h"

static void SessAdopt(CMBaseObjectRef obj, const char *via);   // below
static char gPumpQKey;   // dispatch_queue_set_specific's key on the pump queue: only its address is used

// On the pump queue: the session stops running (its pump timer is cancelled and its microphone use updated).
static void SessStopPump(SimcamSession *sess) {
    sess.running = NO;
    if (sess.timer) {
        dispatch_source_cancel(sess.timer);
        sess.timer = nil;
    }
    MicUpdate(sess);
}

static void SessForget(CMBaseObjectRef obj) {
    SimcamSession *sess;

    @synchronized (gSessionLayer.sessions) {
        sess = gSessionLayer.sessions[[NSValue valueWithPointer:obj]];
        [gSessionLayer.sessions removeObjectForKey:[NSValue valueWithPointer:obj]];
    }

    if (!sess) {
        return;
    }

    // on the pump queue, synchronously: a tick in flight must not touch the object after finalize returns
    void (^forget)(void) = ^{
        SessStopPump(sess);
        for (SimcamSink *sink in sess.sinks) {
            if (sink.queue) {
                CFRelease(sink.queue);
                sink.queue = NULL;
            }
        }
        sess.obj = NULL;
    };

    if (dispatch_get_specific(&gPumpQKey)) {
        forget();
    } else {
        dispatch_sync(gSessionLayer.pumpQueue, forget);
    }
}

static void sess_Finalize(CMBaseObjectRef obj) {
    LOG(@"session %p finalize -> forgotten", obj);
    SessForget(obj);
    StopWaitForget(obj);
    if (gSessionHook.origFinalize) {
        gSessionHook.origFinalize(obj);
    }
}

// The GOT route's replacement for FigCaptureSessionCreate: the original, then the new session adopted. The two
// middle arguments are passed on as they are.
static OSStatus hook_FigCaptureSessionCreate(CFAllocatorRef allocator, void *arg1, void *arg2, CMBaseObjectRef *out) {
    OSStatus err = pFigCaptureSessionCreate(allocator, arg1, arg2, out);
    LOG(@"FigCaptureSessionCreate -> %d obj=%p", (int)err, out ? *out : NULL);
    if (err || !out || !*out) {
        return err;
    }
    SessAdopt(*out, "FigCaptureSessionCreate GOT hook");
    return err;
}

// Whether obj is adopted already. YES: the table has an entry for it and its vtable is the engine's, so there is
// nothing to do. An entry whose object's vtable isn't the engine's is stale; it is forgotten and this returns NO.
static BOOL SessionAlreadyAdopted(CMBaseObjectRef obj, const char *via) {
    if (!SessLookup(obj)) {
        return NO;
    }
    // A FigCaptureSession can be allocated at the address of one that was just freed (seen on 27.0 in Flutter's switch
    // from the back to the front camera). It is the adopted session only if its vtable is already the engine's;
    // otherwise the entry is stale (its finalize wasn't seen), so it is dropped and the new object adopted.
    CMBaseObjectGetVTableFn getVTable = (CMBaseObjectGetVTableFn)dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    const BaseObjVTable *current = getVTable(obj);
    if (current == &gSessionLayer.vtable) {
        LOG(@"session %p already adopted (%s)", obj, via);
        return YES;
    }
    LOG(@"session %p: stale entry for a reused address (vtable %p is not ours) — re-adopting", obj, current);
    SessForget(obj);
    return NO;
}

// Runs once, at the first adoption. It makes the session queues and table, resolves the notification and CAImageQueue
// symbols, and copies the simulator session's vtable into gSessionLayer.vtable. A slot-name mismatch sets
// gSessionHook.slotMismatch and repoints nothing.
static void SessionLayerSetUp(const BaseObjVTable *stockVTable) {
    gSessionLayer.notifyQueue = dispatch_queue_create("simcam.session.notify", 0);
    gSessionLayer.pumpQueue = dispatch_queue_create("simcam.pump", 0);
    dispatch_queue_set_specific(gSessionLayer.pumpQueue, &gPumpQKey, &gPumpQKey, NULL);
    gSessionLayer.sessions = [NSMutableDictionary dictionary];
    pCMNotificationCenterGetDefaultLocalCenter = dlsym(RTLD_DEFAULT, "CMNotificationCenterGetDefaultLocalCenter");
    if (!pCMNotificationCenterPostNotification) {
        pCMNotificationCenterPostNotification = dlsym(RTLD_DEFAULT, "CMNotificationCenterPostNotification");
    }
    void *quartzCore = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
    pCAImageQueueCreate = dlsym(quartzCore, "CAImageQueueCreate");
    pCAImageQueueRegisterIOSurfaceBuffer = dlsym(quartzCore, "CAImageQueueRegisterIOSurfaceBuffer");
    pCAImageQueueInsertImage = dlsym(quartzCore, "CAImageQueueInsertImage");

    // Copy exactly the class's slots (19 on 17.5 and 26.3, 24 on 27.0) after checking their names; refuse on a
    // mismatch.
    int slotCount = 0;
    NSString *why = PreflightCheckSessionSlots(stockVTable->derivedClass, &slotCount);

    if (why) {
        LOG(@"MISMATCH: %@ — not repointing any session", why);
        gSessionHook.slotMismatch = YES;
        return;
    }

    memcpy(gSessionLayer.derived, stockVTable->derivedClass, (slotCount + 1) * sizeof(void *));
    memcpy(gSessionLayer.originalDerived, stockVTable->derivedClass, (slotCount + 1) * sizeof(void *));
    LOG(@"session vtable: %d slots copied (preflight saw %d)", slotCount, gSessSlots);
    gSessionLayer.derived[kSessSlotSetSectionProperty] = (void *)sess_SetSectionProperty;
    gSessionLayer.derived[kSessSlotCopySectionProperty] = (void *)sess_CopySectionProperty;
    gSessionLayer.derived[kSessSlotSetConfiguration] = (void *)sess_SetConfiguration;
    gSessionLayer.derived[kSessSlotStartRunning] = (void *)sess_StartRunning;
    gSessionLayer.derived[kSessSlotStopRunning] = (void *)sess_StopRunning;
    gSessionLayer.derived[kSessSlotFileSinkStartRecording] = (void *)sess_FileStart;
    gSessionLayer.derived[kSessSlotFileSinkStopRecording] = (void *)sess_FileStop;
    gSessionLayer.derived[kSessSlotFileSinkPauseRecording] = (void *)sess_FilePause;
    gSessionLayer.derived[kSessSlotFileSinkResumeRecording] = (void *)sess_FileResume;
    gSessionLayer.derived[kSessSlotIrisCaptureImage] = (void *)sess_IrisCapture;
    gSessionLayer.derived[kSessSlotIrisPrepareToCapture] = (void *)sess_IrisPrepare;

    // Wrap the base class's finalize so that a dying session is forgotten: its pump and microphone stop, a reused
    // address is handled, and gSessionLayer.sessions doesn't leak.
    gSessionHook.base = *stockVTable->baseClass;
    gSessionHook.origFinalize = stockVTable->baseClass->finalize;
    if (SessionFinalizeOn() && stockVTable->baseClass->version == kFigBaseClassVersion) {
        gSessionHook.base.finalize = sess_Finalize;
    }
    gSessionLayer.vtable = (BaseObjVTable){ stockVTable->reserved,
                                            SessionFinalizeOn() ? &gSessionHook.base : stockVTable->baseClass,
                                            gSessionLayer.derived };
}

// SessionRepoint looks for the vtable pointer in this many of the object's first words.
enum { kSessVTableWordScan = 8 };

// Points obj's vtable word (one of its first kSessVTableWordScan words) at gSessionLayer.vtable.
static void SessionRepoint(CMBaseObjectRef obj, const BaseObjVTable *stockVTable) {
    const void **words = (const void **)obj;
    for (int i = 0; i < kSessVTableWordScan; i++) {
        if (words[i] == stockVTable) {
            words[i] = &gSessionLayer.vtable;
            LOG(@"session vtable repointed at word %d", i);
            return;
        }
    }
    LOG(@"MISMATCH: session vtable pointer not found in object");
}

static void SessAdopt(CMBaseObjectRef obj, const char *via) {
    if (SessionAlreadyAdopted(obj, via)) {
        return;   // (it also forgets a stale entry for a reused address)
    }
    LOG(@"adopting session %p via %s", obj, via);
    static dispatch_once_t once;
    CMBaseObjectGetVTableFn getVTable = (CMBaseObjectGetVTableFn)dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    const BaseObjVTable *stockVTable = getVTable(obj);
    dispatch_once(&once, ^{
        SessionLayerSetUp(stockVTable);
    });
    if (gSessionHook.slotMismatch) {
        return;
    }
    SimcamSession *sess = [SimcamSession new];
    sess.obj = obj;
    sess.recs = [NSMutableDictionary dictionary];
    sess.sinks = [NSMutableArray array];
    @synchronized (gSessionLayer.sessions) {
        gSessionLayer.sessions[[NSValue valueWithPointer:obj]] = sess;
    }
    SessionRepoint(obj, stockVTable);
}

// ---- The session hook. __interpose only applies when this dylib is present at launch (DYLD_INSERT_LIBRARIES); when
// a stub dlopen()s the engine, it is silently ignored. So the engine replaces the session's creation in one of two
// ways:
//  objc: swizzle -[AVCaptureSession _createFigCaptureSession] (the only caller of FigCaptureSessionCreate in
//        AVFCapture), call the original, then find the new FigCaptureSession in AVCaptureSessionInternal's ivars
//        (validated by its vtable's slot names).
//  got:  rebind AVFCapture's GOT entry for FigCaptureSessionCreate (found by scanning __got and __auth_got for its
//        value), as fishhook does.
#import <mach-o/loader.h>
#import <mach/mach.h>
#import <malloc/malloc.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
// The lowest address a heap object can have: a 64-bit process maps nothing below 4 GiB (the zero page), so a smaller
// value read from an ivar is not an object pointer.
#define kHeapAddressMin 0x100000000ULL

// Whether candidate, a value read from an ivar, is a simulator FigCaptureSession (or one the engine adopted).
static BOOL SessIsSimulatorSession(const void *candidate) {
    if (!candidate) {
        return NO;
    }
    if ((uintptr_t)candidate < kHeapAddressMin) {
        return NO;
    }
    if ((uintptr_t)candidate & 7) {   // not 8-byte aligned, so not a heap object
        return NO;
    }
    if (!malloc_zone_from_ptr(candidate)) {
        return NO;
    }
    static CMBaseObjectGetVTableFn getVTable;
    if (!getVTable) {
        getVTable = (CMBaseObjectGetVTableFn)dlsym(RTLD_DEFAULT, "CMBaseObjectGetVTable");
    }
    @try {
        if (CFGetTypeID(candidate) == 0) {
            return NO;
        }
        const BaseObjVTable *vtable = getVTable(candidate);
        if (!vtable || !vtable->derivedClass) {
            return NO;
        }
        if (vtable == &gSessionLayer.vtable) {
            return YES;
        }
        Dl_info symbolInfo = {0};
        return dladdr(((void *const *)vtable->derivedClass)[kSessSlotSetConfiguration], &symbolInfo) &&
               symbolInfo.dli_sname && strstr(symbolInfo.dli_sname, "captureSessionSimulator_SetConfiguration");
    } @catch (id exception) {
        return NO;
    }
}

// an ivar type encoding of a C pointer (^) or an object (@)
static BOOL IvarHoldsPointer(const char *type) {
    return type && (type[0] == '^' || type[0] == '@');
}

// an ivar named like the Fig session (figCaptureSession), not its configuration
static BOOL IvarNamesFigSession(const char *name) {
    return strcasestr(name, "figCaptureSession") && !strcasestr(name, "Configuration");
}

// The FigCaptureSession of avSession, found among its AVCaptureSessionInternal's ivars (all of them are logged once).
static CMBaseObjectRef SessFindFig(id avSession) {
    Ivar internalIvar = class_getInstanceVariable(object_getClass(avSession), "_internal");
    id internal = internalIvar ? object_getIvar(avSession, internalIvar) : nil;

    if (!internal) {
        LOG(@"objc route: no _internal ivar");
        return NULL;
    }

    static BOOL dumped;
    unsigned ivarCount = 0;
    Ivar *ivars = class_copyIvarList(object_getClass(internal), &ivarCount);
    CMBaseObjectRef found = NULL;
    NSMutableArray *names = [NSMutableArray array];

    for (unsigned i = 0; i < ivarCount; i++) {
        const char *name = ivar_getName(ivars[i]);
        const char *type = ivar_getTypeEncoding(ivars[i]);
        ptrdiff_t offset = ivar_getOffset(ivars[i]);
        if (!dumped) {
            [names addObject:[NSString stringWithFormat:@"%s %s @%td", name, type, offset]];
        }
        if (!found && IvarHoldsPointer(type) && IvarNamesFigSession(name)) {
            const void *value = *(const void **)((char *)(__bridge void *)internal + offset);
            if (SessIsSimulatorSession(value)) {
                found = value;
                LOG(@"objc route: fig session in ivar %s (%s) @%td = %p", name, type, offset, value);
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

static void hook_createFigCaptureSession(id self, SEL cmd) {
    ((void (*)(id, SEL))gSessionHook.origCreateFig)(self, cmd);
    CMBaseObjectRef session = SessFindFig(self);
    if (session) {
        SessAdopt(session, "_createFigCaptureSession swizzle");
    } else {
        LOG(@"MISMATCH: objc route found no FigCaptureSession");
    }
}

// The GOT route is split. The resolve step walks the images and records the slots (SessFindGOTSlots, which logs
// them); SWAP only does vm_protect and the store (SessGOTSwap). No image is walked after ARMED.
// the size of a Mach-O section's name field (struct section_64's sectname)
enum { kMachOSectNameSize = 16 };

// Records (in gSessionHook.got.slots) every GOT entry holding target in the images whose path ends in imageSuffix, up
// to kSessionHookMaxGotSlots. Returns how many.
static int SessFindGOTSlots(const char *imageSuffix, void *target) {
    gSessionHook.got.count = 0;
    size_t suffixLen = strlen(imageSuffix);
    size_t cacheLen = 0;
    const void *cache = _dyld_get_shared_cache_range(&cacheLen);

    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *imageName = _dyld_get_image_name(i);
        size_t nameLen = strlen(imageName);
        if (nameLen < suffixLen || strcmp(imageName + nameLen - suffixLen, imageSuffix)) {
            continue;
        }
        const struct mach_header_64 *header = (const void *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        BOOL inCache = cache && (const char *)header >= (const char *)cache &&
                       (const char *)header < (const char *)cache + cacheLen;
        LOG(@"got route: %s header %p in shared cache: %d", imageName, header, inCache);
        MachOForEachCommand(header, ^BOOL(const struct load_command *command) {
            if (command->cmd != LC_SEGMENT_64) {
                return NO;
            }
            const struct segment_command_64 *segment = (const void *)command;
            const struct section_64 *section = (const void *)(segment + 1);
            for (uint32_t j = 0; j < segment->nsects; j++, section++) {
                if (strncmp(section->sectname, "__got", kMachOSectNameSize) &&
                    strncmp(section->sectname, "__auth_got", kMachOSectNameSize)) {
                    continue;
                }
                void **entries = (void **)(section->addr + slide);
                size_t count = section->size / sizeof(void *);
                for (size_t entry = 0; entry < count; entry++) {
                    if (entries[entry] == target && gSessionHook.got.count < kSessionHookMaxGotSlots) {
                        gSessionHook.got.slots[gSessionHook.got.count++] = &entries[entry];
                        LOG(@"got route: %.16s,%.16s[%zu] at %p = %p (prepare: found, not yet patched)",
                            segment->segname, section->sectname, entry, &entries[entry], entries[entry]);
                    }
                }
            }
            return NO;
        });
    }

    return gSessionHook.got.count;
}

// SWAP, the session hook's GOT route: only vm_protect and the store
static void SessGOTSwap(void) {
    gSessionHook.got.patched = 0;
    for (int i = 0; i < gSessionHook.got.count; i++) {
        vm_address_t page = (vm_address_t)gSessionHook.got.slots[i] & ~(vm_page_size - 1);
        kern_return_t protectStatus =
            vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (protectStatus == KERN_SUCCESS) {
            *gSessionHook.got.slots[i] = (void *)hook_FigCaptureSessionCreate;
            // a page that can't go back to read-only stays writable: only the protection is lost
            (void)vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ);
            gSessionHook.got.patched++;
        }
    }
}

// The resolve step for the session hook: the objc route's method (-[AVCaptureSession _createFigCaptureSession]), or
// the GOT route's slots.
static BOOL SessResolveHook(void) {
    if (!EnvConfig(ENV_FIG_SESSION, ENV_LEGACY_FIG_SESSION) || !EnvConfig(ENV_FIG_SOURCES, ENV_LEGACY_FIG_SOURCES)) {
        return YES;
    }
    const char *route = getenv(ENV_SESSION_ROUTE);
    gSessionHook.mode = route && !strcmp(route, "got") ? kSessionHookGot : kSessionHookObjc;
    if (gSessionHook.mode == kSessionHookObjc) {
        return ActivationHookResolve(kHookGroupSession, "session hook", objc_getClass("AVCaptureSession"),
                                     "_createFigCaptureSession", NO, NO, (IMP)hook_createFigCaptureSession,
                                     &gSessionHook.origCreateFig, YES);
    }
    if (!pFigCaptureSessionCreate) {
        snprintf(gActivation.hooks.missing, sizeof gActivation.hooks.missing,
                 "session hook (got): the symbol FigCaptureSessionCreate");
        return NO;
    }
    if (!SessFindGOTSlots("/AVFCapture", (void *)pFigCaptureSessionCreate)) {
        snprintf(gActivation.hooks.missing, sizeof gActivation.hooks.missing,
                 "session hook (got): an AVFCapture GOT slot for FigCaptureSessionCreate");
        return NO;
    }
    return YES;
}
