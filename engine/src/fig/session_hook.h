// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the session-creation hook's internal interface. fig/session_hook.inc
// includes it first. It defines the hook's install state (gSessionHook) and declares the functions other fragments
// call.
//
// Who owns what:
//   PREPARE's resolve step (on the activation queue) picks the route and records the GOT slots; SWAP installs the
//   route; START logs the result.
//   The first adoption (once, under dispatch_once) copies the session's base class with our finalize and sets
//   slotMismatch. Nothing writes them after that, so every later adoption reads them without a lock.
#ifndef SIMCAM_SESSION_HOOK_H
#define SIMCAM_SESSION_HOOK_H

// Which session hook the resolve step resolved (0: none, the session layer is off). There is no __interpose route.
enum {
    kSessionHookObjc = 1,   // the _createFigCaptureSession swizzle (default)
    kSessionHookGot = 2,    // AVFCapture's GOT entry for FigCaptureSessionCreate rebound (SIMCAM_SESSION_ROUTE=got)
};

#define kSessionHookMaxGotSlots 8

// CoreMedia's CMBaseObjectGetVTable, looked up with dlsym.
typedef const BaseObjVTable *(*CMBaseObjectGetVTableFn)(CMBaseObjectRef object);

struct SessionHook {
    int mode;            // kSessionHookObjc or kSessionHookGot (0: none)
    BOOL slotMismatch;   // the session's slot names didn't match: no session is repointed
    BaseObjClass base;   // our copy of the session's base class, with our finalize
    void (*origFinalize)(CMBaseObjectRef);   // the session's own finalize, called after ours
    IMP origCreateFig;                       // -[AVCaptureSession _createFigCaptureSession] (the objc route)

    // The GOT route: the entries holding FigCaptureSessionCreate, recorded in the resolve step and rebound by SWAP.
    struct {
        void **slots[kSessionHookMaxGotSlots];
        int count;
        int patched;   // how many SWAP rebound
    } got;
};

static struct SessionHook gSessionHook;

// ---- functions other fragments call

static void SessStopPump(SimcamSession *sess);   // core/control.inc
static BOOL SessResolveHook(void);               // core/lazy.inc (the resolve step)
static void SessGOTSwap(void);                   // core/lazy.inc (SWAP)

#endif
