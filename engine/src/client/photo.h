// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the photo fixes' internal interface. client/photo.inc includes it first.
// It declares the resolve steps core/lazy.inc calls; client/photo.inc keeps no state another fragment reads.
#ifndef SIMCAM_CLIENT_PHOTO_H
#define SIMCAM_CLIENT_PHOTO_H

// ---- functions other fragments call

// PREPARE's resolve step (core/lazy.inc): the photo data fix, the shutter trace, the diagnostics and the stop wait's
// photo hooks.
static BOOL PhotoResolveDataFix(void);
static void PhotoResolveShutter(void);
static void PhotoResolveUnsupported(void);
static void DiagResolveSessionHooks(void);
static void DiagResolveMetadataHooks(void);
static void StopWaitResolveHooks(void);

#endif
