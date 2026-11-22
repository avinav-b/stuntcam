// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): mirroring (the flip after a rotation) and the preview
// layers' notes the pump mirrors the preview by.
//
// Provides: MirrorFlip (providers/pump.inc, providers/photo.inc, providers/audio.inc)
//           PreviewNoteFor (providers/pump.inc, providers/webcam_path.inc)
// Owns:     gPreviewNotes, the preview layers' notes (written by client/rotation.inc, under scene.inc's lock)
// Uses:     providers/scene.inc
#include "mirror.h"
#include "scene.h"

// ---- mirroring: the flip that, after a rotation by `angle`, gives the same picture as mirroring after the rotation.
// For an angle of 0 or 180 it is a horizontal flip; for 90 or 270, a vertical one.
// the values of *flipKind (SimcamSink's and SimcamRecording's flipKind): which flip the session was made for (0: none
// yet)
enum { kFlipKindVertical = 1, kFlipKindHorizontal = 2 };

// src flipped for a picture turned by angle (+1; src retained as it is if the flip fails). *flipSess is made, or
// remade when *flipKind says it was made for the other flip.
static CVPixelBufferRef MirrorFlip(CVPixelBufferRef src, int angle, VTPixelRotationSessionRef *flipSess,
                                   int *flipKind) {
    BOOL vertical = ((angle % 180) + 180) % 180 == 90;

    if (!*flipSess || *flipKind != (vertical ? kFlipKindVertical : kFlipKindHorizontal)) {
        if (*flipSess) {
            VTPixelRotationSessionInvalidate(*flipSess);
            CFRelease(*flipSess);
            *flipSess = NULL;   // never left pointing at the released session if the create below fails
        }
        // A failed create leaves *flipSess NULL: the rotation below fails, and src is returned. A failed property
        // leaves the session unflipped: the picture goes on unflipped.
        (void)VTPixelRotationSessionCreate(NULL, flipSess);
        *flipKind = vertical ? kFlipKindVertical : kFlipKindHorizontal;
        (void)VTSessionSetProperty(*flipSess,
                                   vertical ? kVTPixelRotationPropertyKey_FlipVerticalOrientation
                                            : kVTPixelRotationPropertyKey_FlipHorizontalOrientation,
                                   kCFBooleanTrue);
    }

    // a failed create leaves dst NULL: the rotation then fails, and src is returned
    CVPixelBufferRef dst = NULL;
    (void)CVPixelBufferCreate(NULL, CVPixelBufferGetWidth(src), CVPixelBufferGetHeight(src),
                              CVPixelBufferGetPixelFormatType(src),
                              (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &dst);
    if (VTPixelRotationSessionRotateImage(*flipSess, src, dst)) {
        CVPixelBufferRelease(dst);
        return CVPixelBufferRetain(src);
    }

    return dst;
}

// the note of the preview layer whose sink ID starts with its key, or nil
static NSArray *PreviewNoteFor(NSString *sinkID) {
    if (!gPreviewNotes) {
        return nil;
    }
    @synchronized (gScene.request.lock) {
        for (NSString *prefix in gPreviewNotes) {
            if ([sinkID hasPrefix:prefix]) {
                return gPreviewNotes[prefix];
            }
        }
    }
    return nil;
}
