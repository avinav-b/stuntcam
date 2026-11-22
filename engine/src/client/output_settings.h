// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the recommended-output-settings fix's internal interface.
// client/output_settings.inc includes it first. It declares the resolve step core/lazy.inc calls.
#ifndef SIMCAM_OUTPUT_SETTINGS_H
#define SIMCAM_OUTPUT_SETTINGS_H

static BOOL RecSettingsResolveFix(void);   // core/lazy.inc (PREPARE's resolve step)

#endif
