// SPDX-License-Identifier: Apache-2.0

package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/trace"
)

// cmdTrace prints an app's op ring, the trace the engine writes (engine/src/core/trace.inc).
// It reads the newest trace-<pid>.txt, or the one for --pid. It looks in the app's state dir or its container-tmp
// fallback, found the way `sentinel` finds them.
// The app may be running, and then the file is rewritten at most every 150 ms after an op. Or the app may be gone, and
// then the file is its last copy.
// With no trace it exits 1.
func cmdTrace(args []string) error {
	flags := flag.NewFlagSet("trace", flag.ContinueOnError)
	asJSON := flags.Bool("json", false, "JSON output (schema: cli/README.md)")
	udid := flags.String("udid", "", "simulator UDID")
	pid := flags.Int("pid", 0, "that process's trace (default: the newest)")
	if err := parse(flags, args, 1); err != nil {
		return err
	}

	if flags.NArg() != 1 {
		return usagef("usage: stuntcam trace [--json] [--udid U] [--pid N] BUNDLE")
	}

	bundle := flags.Arg(0)
	if bundle == "" || bundle == "." || bundle == ".." || strings.ContainsAny(bundle, "/\x00") {
		return usagef("bad bundle id %q", bundle)
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	locs := []trace.Location{{Dir: filepath.Join(home.StateDir(device.UDID), bundle), Kind: "state-dir"}}
	fallback, unchecked := newFallbackFinder(device).find(bundle)
	if unchecked == nil {
		locs = append(locs, trace.Location{Dir: fallback.Dir, Kind: "container-tmp"})
	}

	files, skipped := trace.Find(locs)
	for _, skip := range skipped {
		fmt.Fprintln(os.Stderr, "stuntcam: skipped", skip)
	}

	if len(files) == 0 {
		msg := fmt.Sprintf("no trace for %s on %s: none in %s", bundle, device.Name, locs[0].Dir)
		if unchecked == nil {
			msg += " or " + fallback.Dir
		} else {
			msg += fmt.Sprintf(" (container-tmp fallback not checked: %s)", unchecked)
		}

		return errors.New(msg + "; an app writes one once it uses the camera on an armed simulator")
	}

	picked, others, err := trace.Pick(files, *pid)
	if err != nil {
		return err
	}

	report := trace.Build(picked, others, device.UDID, device.Name, appRunning(device, bundle, picked.Pid))
	if *asJSON {
		encoder := json.NewEncoder(os.Stdout)
		encoder.SetIndent("", " ")
		return encoder.Encode(report)
	}

	trace.Human(os.Stdout, report, time.Now(), time.Local)
	return nil
}

// appRunning reports whether pid is bundle's running app process on the sim, or nil if it can't tell.
func appRunning(device simctl.Device, bundle string, pid int) *bool {
	notRunning := false
	if device.State != simctl.Booted {
		return &notRunning
	}

	spawnCtl := ctl
	if spawnCtl.Timeout == 0 || spawnCtl.Timeout > fallbackTimeout {
		spawnCtl.Timeout = fallbackTimeout
	}

	out, err := spawnCtl.Spawn(device.UDID, "launchctl", "list")
	if err != nil {
		return nil
	}

	running := trace.Running(out, bundle, pid)
	return &running
}
