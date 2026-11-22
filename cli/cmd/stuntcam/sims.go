// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

var ctl = simctl.Ctl{}

// pickSim resolves the target: --udid, else the only booted simulator, else the most recently booted available one.
func pickSim(udid string) (simctl.Device, string, error) {
	devices, err := ctl.Devices()
	if err != nil {
		return simctl.Device{}, "", err
	}

	if udid != "" {
		for _, device := range devices {
			if device.UDID == udid {
				return device, "given with --udid", nil
			}
		}

		return simctl.Device{}, "", fmt.Errorf("no simulator %s (xcrun simctl list devices)", udid)
	}

	var booted []simctl.Device
	for _, device := range devices {
		if device.State == simctl.Booted && strings.Contains(device.RuntimeID, "iOS") {
			booted = append(booted, device)
		}
	}

	switch {
	case len(booted) == 1:
		return booted[0], "the only booted iOS simulator", nil
	case len(booted) > 1:
		var names []string
		for _, device := range booted {
			names = append(names, fmt.Sprintf("%s (%s)", device.Name, device.UDID))
		}

		return simctl.Device{}, "", fmt.Errorf("%d simulators are booted; pick one with --udid: %s", len(booted),
			strings.Join(names, ", "))
	}

	var best simctl.Device
	for _, device := range devices {
		if device.Available && strings.Contains(device.RuntimeID, "iOS") && device.LastBooted.After(best.LastBooted) {
			best = device
		}
	}

	if best.UDID == "" {
		return best, "", errors.New("no booted simulator and none booted before; " +
			"pass --udid (xcrun simctl list devices)")
	}

	return best, "the most recently booted iOS simulator", nil
}

func runtimeName(id string) string {
	s := strings.TrimPrefix(id, "com.apple.CoreSimulator.SimRuntime.")
	return strings.Replace(strings.Replace(s, "-", " ", 1), "-", ".", -1)
}

// springboardWait is how long an armed boot gets to bring SpringBoard up (up then falls back to launchd mode; exec runs
// the command anyway).
const springboardWait = 60 * time.Second

const (
	// springboardSpawnTimeout bounds each poll of springboardUp: one slow spawn must not outlast its bound.
	springboardSpawnTimeout = 10 * time.Second
	// springboardUpPoll is the pause between springboardUp's polls.
	springboardUpPoll = time.Second
)

// springboardUp waits until the sim's launchd lists a running SpringBoard.
func springboardUp(udid string, bound time.Duration) bool {
	spawnCtl := ctl
	spawnCtl.Timeout = springboardSpawnTimeout
	for end := time.Now().Add(bound); time.Now().Before(end); time.Sleep(springboardUpPoll) {
		out, err := spawnCtl.Spawn(udid, "launchctl", "list")
		if err != nil {
			continue
		}

		for _, line := range strings.Split(out, "\n") {
			fields := strings.Fields(line)
			if len(fields) == 3 && fields[2] == "com.apple.SpringBoard" && fields[0] != "-" {
				return true
			}
		}
	}

	return false
}
