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

// xcodeSummary says whether recordXcode prints which Xcode it recorded and whether that Xcode's restore rules are
// verified.
type xcodeSummary bool

const (
	noXcodeSummary    xcodeSummary = false
	printXcodeSummary xcodeSummary = true
)

// bootSignalSource says where recordXcode takes the boot's signal variables from (config.json's boot_signals: the
// variables the loader reads as signs that another tool displaced stuntcam's insert).
type bootSignalSource bool

const (
	// signalsFromLaunchdSim is for a sim stuntcam doesn't boot: the signals are read from its launchd_sim's
	// environment (none if the sim isn't booted).
	signalsFromLaunchdSim bootSignalSource = false
	// signalsFromThisProcess is for a sim stuntcam boots with this process's environment: simctl boot passes this
	// process's SIMCTL_CHILD_ variables into the boot, so the signals are taken from those.
	signalsFromThisProcess bootSignalSource = true
)

// recordXcode writes sims/<udid>/config.json, which tells the loader whose restore rules to apply. It records the
// Xcode active now (DEVELOPER_DIR, else xcode-select), the source command, and rulesDir, the per-content rules
// directory that artifacts.Stage made. When an app is launched with ⌘R nothing in it names the launching Xcode, so
// this record is the loader's only way to know. If the active Xcode can't be identified, the Xcode of an existing
// record is kept.
func recordXcode(udid, rulesDir, source string, summary xcodeSummary, signals bootSignalSource) xcode.Info {
	active, err := xcodeActive()
	if err != nil {
		if old, rerr := arm.ReadConfig(udid); rerr == nil && old.Build != "" {
			// keep only the Xcode: the rules dir and boot signals are this arm's
			fmt.Fprintf(os.Stderr, "warning: could not identify the Xcode in use (%v); keeping the Xcode recorded "+
				"earlier, %s (%s)\n", err, old.Build, old.DeveloperDir)
			active = old.Info
		} else {
			fmt.Fprintf(os.Stderr, "warning: could not identify the Xcode in use (%v); apps will use the newest "+
				"restore rules and say so\n", err)
		}
	}

	if err := arm.WriteConfig(udid, active, rulesDir, source, bootSignals(udid, signals)); err != nil {
		fmt.Fprintf(os.Stderr, "warning: recording the Xcode for %s: %v\n", udid, err)
	}

	if summary {
		rulesFile, ok := "", false
		if rulesDir != "" {
			rulesFile, ok = xcode.Rules(rulesDir, active.Build)
		}

		switch {
		case ok:
			fmt.Printf("Xcode %s (%s, %s): restore rules verified\n", active.Version, active.Build, active.DeveloperDir)
		case rulesFile != "":
			fmt.Printf("Xcode %s (%s, %s): %s (best guess: %s)\n", active.Version, active.Build, active.DeveloperDir,
				xcode.UnverifiedLine(active.Build), strings.TrimSuffix(filepath.Base(rulesFile), ".json"))
		default:
			fmt.Printf("Xcode %s (%s): %s (no usable rules staged%s: the loader's built-in 27A266a rules)\n",
				active.Version, active.Build, xcode.UnverifiedLine(active.Build), prefix(" in ", rulesDir))
		}
	}

	return active
}

// bootSignals returns the displaced-insert signal variables of a sim's boot environment, taken from where source says.
func bootSignals(udid string, source bootSignalSource) []string {
	if source == signalsFromThisProcess {
		return arm.ChildSignals(os.Environ())
	}

	env, err := bootEnv(udid)
	if err != nil {
		return nil
	}

	var pairs []string
	for k, v := range env {
		pairs = append(pairs, k+"="+v)
	}

	return arm.Signals(pairs)
}
