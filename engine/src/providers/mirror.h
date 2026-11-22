// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the mirroring's and the preview notes' internal interface. It defines
// the preview notes (gPreviewNotes) and declares the functions providers/mirror.inc defines; providers/webcam_path.inc
// calls PreviewNoteFor before that fragment.
#ifndef SIMCAM_MIRROR_H
#define SIMCAM_MIRROR_H
#import <VideoToolbox/VideoToolbox.h>

// The preview layers' notes: "<Class: 0xptr" (the start of a preview sink's ID) -> @[angle, mirrored], written by the
// preview layer hook (client/rotation.inc). Guarded by @synchronized(gScene.request.lock).
static NSMutableDictionary *gPreviewNotes;

// ---- functions other fragments call

// providers/pump.inc, providers/photo.inc, providers/audio.inc
static CVPixelBufferRef MirrorFlip(CVPixelBufferRef src, int angle, VTPixelRotationSessionRef *flipSess,
                                   int *flipKind);
static NSArray *PreviewNoteFor(NSString *sinkID);   // providers/pump.inc, providers/webcam_path.inc

#endif
