// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/procenv"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

var ctl = simctl.Ctl{}

// pickSim resolves the target: --udid, else the only booted simulator, else the most recently booted available one.
func pickSim(udid string) (simctl.Device, string, error) {
	ds, err := ctl.Devices()
	if err != nil {
		return simctl.Device{}, "", err
	}
	if udid != "" {
		for _, d := range ds {
			if d.UDID == udid {
				return d, "given with --udid", nil
			}
		}
		return simctl.Device{}, "", fmt.Errorf("no simulator %s (xcrun simctl list devices)", udid)
	}
	var booted []simctl.Device
	for _, d := range ds {
		if d.State == "Booted" && strings.Contains(d.RuntimeID, "iOS") {
			booted = append(booted, d)
		}
	}
	switch {
	case len(booted) == 1:
		return booted[0], "the only booted iOS simulator", nil
	case len(booted) > 1:
		var n []string
		for _, d := range booted {
			n = append(n, fmt.Sprintf("%s (%s)", d.Name, d.UDID))
		}
		return simctl.Device{}, "", fmt.Errorf("%d simulators are booted; pick one with --udid: %s", len(booted),
			strings.Join(n, ", "))
	}
	var best simctl.Device
	for _, d := range ds {
		if d.Available && strings.Contains(d.RuntimeID, "iOS") && d.LastBooted.After(best.LastBooted) {
			best = d
		}
	}
	if best.UDID == "" {
		return best, "", errors.New("no booted simulator and none booted before; pass --udid (xcrun simctl list devices)")
	}
	return best, "the most recently booted iOS simulator", nil
}

func runtimeName(id string) string {
	s := strings.TrimPrefix(id, "com.apple.CoreSimulator.SimRuntime.")
	return strings.Replace(strings.Replace(s, "-", " ", 1), "-", ".", -1)
}

var bootEnv = procenv.BootEnv // replaced in tests

// springboardWait is how long an armed boot gets to bring SpringBoard up (up then falls back to launchd mode; exec runs
// the command anyway).
const springboardWait = 60 * time.Second

// springboardUp waits until the sim's launchd lists a running SpringBoard.
func springboardUp(udid string, bound time.Duration) bool {
	c := ctl
	c.Timeout = 10 * time.Second // one slow spawn must not outlast the bound
	for end := time.Now().Add(bound); time.Now().Before(end); time.Sleep(time.Second) {
		out, err := c.Spawn(udid, "launchctl", "list")
		if err != nil {
			continue
		}
		for _, l := range strings.Split(out, "\n") {
			if f := strings.Fields(l); len(f) == 3 && f[2] == "com.apple.SpringBoard" && f[0] != "-" {
				return true
			}
		}
	}
	return false
}
