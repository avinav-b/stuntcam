// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included in order): mirroring (the flip after a rotation) and the preview
// layers' notes the pump mirrors the preview by.
// ---- mirroring: flip so that (rotate by `angle`, then this flip) == (mirror after rotating); angle 0/180 ->
// horizontal, 90/270 -> vertical
// the values of *sessAngle (SimcamSink/SimcamRecording flipKind): which flip the session was made for (0: none yet)
enum { kFlipKindVertical = 1, kFlipKindHorizontal = 2 };
static CVPixelBufferRef MirrorFlip(CVPixelBufferRef src, int angle, VTPixelRotationSessionRef *sess, int *sessAngle) {
    BOOL vert = ((angle % 180) + 180) % 180 == 90;
    if (!*sess || *sessAngle != (vert ? kFlipKindVertical : kFlipKindHorizontal)) {
        if (*sess) {
            VTPixelRotationSessionInvalidate(*sess);
            CFRelease(*sess);
        }
        VTPixelRotationSessionCreate(NULL, sess);
        *sessAngle = vert ? kFlipKindVertical : kFlipKindHorizontal;
        VTSessionSetProperty(*sess, vert ? kVTPixelRotationPropertyKey_FlipVerticalOrientation
                                         : kVTPixelRotationPropertyKey_FlipHorizontalOrientation, kCFBooleanTrue);
    }
    CVPixelBufferRef dst = NULL;
    CVPixelBufferCreate(NULL, CVPixelBufferGetWidth(src), CVPixelBufferGetHeight(src),
                        CVPixelBufferGetPixelFormatType(src),
                        (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &dst);
    if (VTPixelRotationSessionRotateImage(*sess, src, dst)) {
        CVPixelBufferRelease(dst);
        return CVPixelBufferRetain(src);
    }
    return dst;
}
// "<Class: 0xptr" -> @[angle, mirrored], written by the preview layer hook (client/rotation.inc); guarded by
// @synchronized(gSrcSpecLock)
static NSMutableDictionary *gPrevNotes;
static NSArray *PreviewNoteFor(NSString *sinkID) {
    if (!gPrevNotes) return nil;
    @synchronized (gSrcSpecLock) {
        for (NSString *k in gPrevNotes)
            if ([sinkID hasPrefix:k]) return gPrevNotes[k];
    }
    return nil;
}
