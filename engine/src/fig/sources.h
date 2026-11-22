// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): what fig/sources.inc defines and later fragments use. fig/sources.inc
// comes before every fragment that includes this header, so its definitions are already in scope here: this header
// repeats them (tentative definitions, so fig/sources.inc must stay first), so that a fragment's #include says
// where each name comes from. The CMBaseObject types (CMBaseObjectRef, BaseObjClass, BaseObjVTable, CMBaseClassID)
// are defined there too.
#ifndef SIMCAM_SOURCES_H
#define SIMCAM_SOURCES_H

// ---- the CoreMedia and CMCapture functions core/init.inc resolves at PREPARE (dlsym)

static OSStatus (*pCMDerivedObjectCreate)(CFAllocatorRef, const BaseObjVTable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*pCMBaseObjectGetDerivedStorage)(CMBaseObjectRef);
static CMBaseClassID (*pFigCaptureSourceGetClassID)(void);
static void *(*pFigCaptureDeviceGetNotificationCenter)(void);
// also called by fig/session_state.inc, providers/video.inc and fig/session_hook.inc
static OSStatus (*pCMNotificationCenterPostNotification)(void *, CFTypeRef, CFTypeRef, CFTypeRef, uint32_t);

// ---- the sources' state and hooks, which core/init.inc makes at PREPARE and installs

static NSMutableSet *gKeysSeen;   // the property keys already logged once
static NSDictionary *gOverride;   // the SIMCAM_PROFILE plist's devices and formats, or nil
static IMP gOrigAudioSources;
static IMP gOrigIsConnected;
static id hook_videoNewFigCaptureSources(id self, SEL _cmd);
static id hook_audioNewFigCaptureSources(id self, SEL _cmd);
static BOOL hook_audioIsConnected(id self, SEL c);

static _Atomic BOOL gQuietPost;   // silences the post log line (providers/video.inc sets it)

#endif
