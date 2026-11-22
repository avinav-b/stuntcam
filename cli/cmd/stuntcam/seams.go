// SPDX-License-Identifier: Apache-2.0

package main

import (
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/procenv"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// Test seams: the hooks tests replace to stand in for the Mac. Production code only reads them; each defaults to the
// real implementation or value.
var (
	bootEnv         = procenv.BootEnv    // a booted simulator's launchd_sim environment
	xcodeActive     = xcode.Active       // the Xcode xcodebuild and xcrun use now (xcoderecord.go)
	selectedXcode   = xcode.Selected     // the Xcode xcode-select names (xcoderecord.go)
	installedXcodes = xcode.Installed    // every installed Xcode (xcoderecord.go)
	sourceRulesDir  = realSourceRulesDir // the restore rules `up` would stage (doctor.go)
)

// fallbackTimeout bounds the per-app simctl calls of status, sentinel clear and trace: get_app_container, and trace's
// launchctl list inside the simulator (tests shorten it).
var fallbackTimeout = 10 * time.Second
