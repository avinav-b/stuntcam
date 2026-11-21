// SPDX-License-Identifier: Apache-2.0

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// recordXcode writes sims/<udid>/config.json: the Xcode active now (DEVELOPER_DIR, else xcode-select), whose restore
// rules the loader applies (design §4.1 "Which Xcode's libs"), from rulesDir (the per-content dir artifacts.Stage
// made). Under ⌘R nothing in the app names the launching Xcode, so this record is the loader's only source. If the
// Xcode can't be identified, an existing record is kept. booting: stuntcam boots the sim with this process's
// environment, so its SIMCTL_CHILD_ signal variables become boot signals; otherwise they are read from the booted sim's
// launchd_sim. With say, prints which Xcode and whether its rules are verified.
func recordXcode(udid, rulesDir, source string, say, booting bool) xcode.Info {
	x, err := xcodeActive()
	if err != nil {
		if old, rerr := arm.ReadConfig(udid); rerr == nil && old.Build != "" {
			// keep only the Xcode: the rules dir and boot signals are this arm's
			fmt.Fprintf(os.Stderr, "warning: could not identify the Xcode in use (%v); keeping the Xcode recorded earlier, "+
				"%s (%s)\n", err, old.Build, old.DeveloperDir)
			x = old.Info
		} else {
			fmt.Fprintf(os.Stderr, "warning: could not identify the Xcode in use (%v); apps will use the newest restore rules "+
				"and say so\n", err)
		}
	}
	if err := arm.WriteConfig(udid, x, rulesDir, source, bootSignals(udid, booting)); err != nil {
		fmt.Fprintf(os.Stderr, "warning: recording the Xcode for %s: %v\n", udid, err)
	}
	if say {
		f, ok := "", false
		if rulesDir != "" {
			f, ok = xcode.Rules(rulesDir, x.Build)
		}
		switch {
		case ok:
			fmt.Printf("Xcode %s (%s, %s): restore rules verified\n", x.Version, x.Build, x.DeveloperDir)
		case f != "":
			fmt.Printf("Xcode %s (%s, %s): %s (best guess: %s)\n", x.Version, x.Build, x.DeveloperDir,
				xcode.UnverifiedLine(x.Build), strings.TrimSuffix(filepath.Base(f), ".json"))
		default:
			fmt.Printf("Xcode %s (%s): %s (no usable rules staged%s: the loader's built-in 27A266a rules)\n", x.Version,
				x.Build, xcode.UnverifiedLine(x.Build), prefix(" in ", rulesDir))
		}
	}
	return x
}

// bootSignals: the displaced-insert signal variables in a sim's boot environment (config.json boot_signals). booting:
// the SIMCTL_CHILD_ ones of this process (simctl boot passes them on); else those of its launchd_sim, if booted.
func bootSignals(udid string, booting bool) []string {
	if booting {
		return arm.ChildSignals(os.Environ())
	}
	env, err := bootEnv(udid)
	if err != nil {
		return nil
	}
	var l []string
	for k, v := range env {
		l = append(l, k+"="+v)
	}
	return arm.Signals(l)
}

var xcodeActive, selectedXcode, installedXcodes = xcode.Active, xcode.Selected, xcode.Installed // replaced in tests
