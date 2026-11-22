// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the photo provider's internal interface. providers/photo.inc includes it
// first. It defines the next photo's test error and declares the photo slots fig/session_hook.inc installs.
#ifndef SIMCAM_PHOTO_H
#define SIMCAM_PHOTO_H

// The control file's event photo-error:<status> (core/control.inc) makes the next photo complete with that
// ErrorStatus and no image, to map Fig statuses to the NSError apps get (what a photo latch delivers is chosen from
// that map).
static _Atomic int gPhotoErrNext;

// ---- functions other fragments call

// The still-image slots (fig/session_hook.inc).
static OSStatus sess_IrisCapture(CMBaseObjectRef session, CFStringRef sinkIDRef, id settings);
static OSStatus sess_IrisPrepare(CMBaseObjectRef session, CFStringRef sinkID, id settings);

#endif
