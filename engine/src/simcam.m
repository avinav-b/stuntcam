// SPDX-License-Identifier: Apache-2.0
// simcam.m — the iOS-Simulator virtual camera engine, one translation unit: fake capture sources, the session layer,
// audio (mic or tone), the Mac webcam, the activation gate, client fixes (orientation, Vision, Flutter, VisionCamera).
// Loaded by the boot stub (dlopen) or SIMCTL_CHILD_DYLD_INSERT_LIBRARIES. No __interpose; nothing linked from CMCapture
// (every private C symbol is dlsym'd and checked by the preflight, so a missing symbol can never stop the app from
// launching).
// Env: core/env.inc lists every variable the engine reads. The main ones:
//   SIMCAM=0 nothing at all · SIMCAM_FIG_SOURCES=1 fake sources · SIMCAM_FIG_SESSION=1 session layer ·
//   SIMCAM_PROFILE profile · SIMCAM_ENGINE_LOG log (%p -> pid)
#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <sys/mman.h>
#import <stdatomic.h>

// set once by ActOpenLog, read by LOG on any thread (atomic: a plain pointer is a data race under TSan)
static _Atomic(FILE *) gLogF;
// LOG(fmt, ...): one line "[simcam <pid>] <message>" to stderr and, once ActOpenLog has opened it, to the per-pid log
// file (SIMCAM_ENGINE_LOG), there prefixed with the CFAbsoluteTime.
#define LOG(fmt, ...)                                                                           \
    do {                                                                                        \
        NSString *_s = [NSString stringWithFormat:@"[simcam %d] " fmt, getpid(), ##__VA_ARGS__]; \
        fprintf(stderr, "%s\n", _s.UTF8String);                                                 \
        FILE *_lf = atomic_load_explicit(&gLogF, memory_order_acquire);                         \
        if (_lf) {                                                                              \
            fprintf(_lf, "%.3f %s\n", CFAbsoluteTimeGetCurrent(), _s.UTF8String);               \
            fflush(_lf);                                                                        \
        }                                                                                       \
    } while (0)


// The engine is one translation unit split into fragments by concern; order matters (statics, constructors).
#include "core/env.inc"           // names of the environment variables read below
#include "fig/sources.inc"
#include "core/act.inc"
#include "core/preflight.inc"
#include "core/safety.inc"
#include "core/crash_handler.inc"
#include "core/markers.inc"
#include "core/events_file.inc"
#include "core/sentinel_activate.inc"
#include "core/trace.inc"
#include "core/init.inc"
#include "fig/session_state.inc"
#include "providers/video.inc"
#include "webcam/ring.c"          // the webcam ring (docs/webcam-ring.md): shared with the camera helper
#include "webcam/ring_shm.c"
#include "providers/webcam.inc"
#include "providers/scene.inc"
#include "providers/mirror.inc"
#include "providers/barcodes.inc"
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
