// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the Vision barcode revision pin's internal interface.
// It defines the pin's install state; client/vision.inc defines the functions it declares, and providers/barcodes.inc
// calls VisionInstall before that fragment.
#ifndef SIMCAM_VISION_H
#define SIMCAM_VISION_H

// The install state: VisionInstall's once-flag, and SWAP's (core/lazy.inc reads both).
static _Atomic int gVisionState;   // 0 not installed, 1 installed (or being installed)
static BOOL gVisionInSwap;         // SWAP is installing the hooks

// ---- functions other fragments call

static void VisionInstall(void);      // providers/barcodes.inc
static void VisionResolveFix(void);   // core/lazy.inc
static void VisionStart(void);        // core/lazy.inc

#endif
