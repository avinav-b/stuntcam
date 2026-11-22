// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the rotation coordinator's internal interface. client/rotation.inc
// defines what it declares; providers/scene.inc and client/motion.inc call into it before that fragment.
//
// Who owns what:
//   The coordinator table is made in the resolve step and published (release) at the end of SWAP; CoordTable reads
//   it (acquire). Everything else in client/rotation.inc is local to it.
#ifndef SIMCAM_ROTATION_H
#define SIMCAM_ROTATION_H

// ---- functions other fragments call

static NSHashTable *CoordTable(void);                           // client/motion.inc
static void CoordOrientationChanged(void);                      // client/motion.inc, core/control.inc
static double CoordAngleFor(long orientation, long position);   // providers/scene.inc
static BOOL CoordResolveHooks(void);                            // core/lazy.inc
static void CoordPublish(void);                                 // core/lazy.inc
static BOOL PreviewResolveRotation(void);                       // core/lazy.inc

#endif
