// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): process start's internal interface. core/init.inc includes it first. It
// declares the functions other fragments call; init.inc keeps no state another fragment reads.
#ifndef SIMCAM_INIT_H
#define SIMCAM_INIT_H

// ---- functions other fragments call

static double ProcessStartTime(void);   // providers/video.inc, providers/scene.inc, core/lazy.inc

// Load and PREPARE's lookups and resolve steps (core/lazy.inc).
static BOOL IsPrimaryEngine(void);
static void OpenLog(void);
static void ActivationLoadFrameworks(void);
static BOOL ActivationResolveSourceHooks(void);

#endif
