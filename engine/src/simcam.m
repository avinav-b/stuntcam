// SPDX-License-Identifier: Apache-2.0
// simcam.m — spike m10: ONE merged prototype of the iOS-Simulator virtual camera (m2 source fake + m5 session layer
// + m6 audio/gate/objc session hook/17.5 + m7 Flutter/ado/tone/orientation/Vision + m8 VisionCamera fixes + m9 iOS 27 + m10 fixes).
// Loaded by the boot stub (dlopen) or SIMCTL_CHILD_DYLD_INSERT_LIBRARIES. No __interpose; nothing linked from CMCapture
// (every private C symbol is dlsym'd and checked by the preflight, so a missing symbol can never stop the app from launching).
// Env (all off switches kept from the spikes):
//   SIMCAM=0 nothing at all · M2_FAKE=1 fake sources · M2_SESSION_HOOK=1 session layer · M2_PLIST profile · M5_LOG log (%p -> pid)
#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <sys/mman.h>
#import <stdatomic.h>

static _Atomic(FILE *) gLogF;   // set once by ActOpenLog (activation worker, record queue, or inline with SIMCAM_LAZY=0 / M16_NO_ACTTIMEOUT), read by LOG on any thread (w4 e46: TSan race since an earlier change)
#define LOG(fmt, ...) do { NSString *_s = [NSString stringWithFormat:@"[simcam %d] " fmt, getpid(), ##__VA_ARGS__]; fprintf(stderr, "%s\n", _s.UTF8String); FILE *_lf = atomic_load_explicit(&gLogF, memory_order_acquire); if (_lf) { fprintf(_lf, "%.3f %s\n", CFAbsoluteTimeGetCurrent(), _s.UTF8String); fflush(_lf); } } while (0)


// The engine is one translation unit split into fragments by concern; order matters (statics, constructors).
#include "fig/sources.inc"
#include "core/act.inc"
#include "core/preflight.inc"
#include "core/safety.inc"
#include "core/trace.inc"
#include "core/init.inc"
#include "fig/session_state.inc"
#include "providers/video.inc"
#include "providers/scene.inc"
#include "providers/audio.inc"
#include "providers/pump.inc"
#include "providers/photo.inc"
#include "fig/session_hook.inc"
#include "client/motion.inc"
#include "client/photo.inc"
#include "client/rotation.inc"
#include "client/output_settings.inc"
#include "client/vision.inc"
#include "core/control.inc"
#include "core/lazy.inc"
