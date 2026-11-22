// SPDX-License-Identifier: Apache-2.0

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"syscall"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// controllable says why live control (source/orient) would reach no app on the sim, or nil.
func controllable(device simctl.Device, key string) error {
	if _, _, err := home.ReadCurrent(device.UDID); err != nil {
		return fmt.Errorf("%s isn't armed: `stuntcam up` first", device.Name)
	}

	record, err := arm.ReadRecord(device.UDID)
	switch {
	case err != nil:
		return fmt.Errorf("%s wasn't armed by the stuntcam command (the scripts don't set up live control): "+
			"`stuntcam up`", device.Name)
	case device.State != simctl.Booted:
		return notRunning(device.Name, key)
	case record.Mode == arm.Boot && !arm.SameBoot(record, true, device.LastBooted):
		return fmt.Errorf("%s was rebooted without stuntcam since `up`: `stuntcam up --reboot` first", device.Name)
	// env's exports come from this binary, so they always have it
	case record.Mode != arm.Env && !slices.Contains(record.Vars, "SIMCAM_CONTROL"):
		return fmt.Errorf("%s was armed before live control existed: run `stuntcam up` again (add --reboot for a "+
			"boot-mode simulator)", device.Name)
	case record.Mode == arm.Launchd:
		// The record keeps every name any arm set, so that `down` unsets them all. But an older stuntcam's `up` may
		// have unset this one since then. So ask the sim. Otherwise source would report success while no app polls the
		// file.
		value, err := ctl.Getenv(device.UDID, "SIMCAM_CONTROL")
		if err != nil {
			return fmt.Errorf("reading %s's launchd environment failed: %w", device.Name, err)
		}

		if value != home.ControlFile(device.UDID) {
			return fmt.Errorf("%s's launchd environment has no stuntcam control file (re-armed by an older "+
				"stuntcam?): run `stuntcam up` again", device.Name)
		}
	}

	return nil
}

// notRunning is controllable's error for a sim that isn't booted: how to get the key's setting to its apps.
func notRunning(name, key string) error {
	if key == "source" {
		return fmt.Errorf("%s isn't running; `stuntcam up --source …` sets the picture when it boots", name)
	}
	return fmt.Errorf("%s isn't running: `stuntcam up` boots it armed, then `stuntcam %s`", name, controlCommand(key))
}

// controlCommand is the stuntcam command that sets a control-file key other than source ("" for an unknown key).
func controlCommand(key string) string {
	switch key {
	case "orientation":
		return "orient"
	default:
		return ""
	}
}

// setControl sets key=value in a simulator's control file (other lines kept), atomically. The sim must have been armed
// with the control file (stuntcam 0.1 arms without it: a re-arm is needed).
func setControl(device simctl.Device, key, value string) error {
	if err := controllable(device, key); err != nil {
		return err
	}
	return writeControl(device, key, value)
}

// controlValue is the last value of key in the sim's control file ("" if it has none): the one the engine applies.
func controlValue(udid, key string) string {
	data, err := os.ReadFile(home.ControlFile(udid))
	if err != nil {
		return ""
	}

	value := ""
	for _, line := range strings.Split(string(data), "\n") {
		if name, lineValue, ok := strings.Cut(strings.TrimSpace(line), "="); ok && name == key {
			value = lineValue
		}
	}

	return value
}

// writeControl is setControl without the checks, for a caller that has just run them (each launchd check is a
// `simctl spawn`, ~0.3 s).
func writeControl(device simctl.Device, key, value string) error {
	path := home.ControlFile(device.UDID)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}

	// read-modify-write under a lock, so a concurrent source and orient don't lose one change
	lock, err := os.OpenFile(path+".lock", os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer lock.Close()

	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX); err != nil {
		return err
	}
	defer syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)

	data, _ := os.ReadFile(path) // no file yet: no lines to keep
	var lines []string
	for _, line := range strings.Split(string(data), "\n") {
		if name, _, ok := strings.Cut(strings.TrimSpace(line), "="); ok && name != key {
			lines = append(lines, strings.TrimSpace(line))
		}
	}

	lines = append(lines, key+"="+value)
	return home.WriteAtomic(path, []byte(strings.Join(lines, "\n")+"\n"))
}
