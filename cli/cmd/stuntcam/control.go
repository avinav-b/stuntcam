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
func controllable(d simctl.Device, key string) error {
	if _, _, err := home.ReadCurrent(d.UDID); err != nil {
		return fmt.Errorf("%s isn't armed: `stuntcam up` first", d.Name)
	}
	r, err := arm.ReadRecord(d.UDID)
	switch {
	case err != nil:
		return fmt.Errorf("%s wasn't armed by the stuntcam command (the scripts don't set up live control): `stuntcam up`",
			d.Name)
	case d.State != "Booted" && key == "source":
		return fmt.Errorf("%s isn't running; `stuntcam up --source …` sets the picture when it boots", d.Name)
	case d.State != "Booted":
		return fmt.Errorf("%s isn't running: `stuntcam up` boots it armed, then `stuntcam %s`", d.Name,
			map[string]string{"orientation": "orient"}[key])
	case r.Mode == arm.Boot && !arm.SameBoot(r, true, d.LastBooted):
		return fmt.Errorf("%s was rebooted without stuntcam since `up`: `stuntcam up --reboot` first", d.Name)
	case r.Mode != arm.Env && !slices.Contains(r.Vars, "SIMCAM_CONTROL"): // env's exports come from this binary
		return fmt.Errorf("%s was armed before live control existed: run `stuntcam up` again (add --reboot for a boot-mode "+
			"simulator)", d.Name)
	case r.Mode == arm.Launchd:
		// the record keeps every name any arm set (so `down` unsets them all); an older stuntcam's `up` since then may have
		// unset this one, so ask the sim (otherwise source would report success while no app polls the file)
		v, err := ctl.Getenv(d.UDID, "SIMCAM_CONTROL")
		if err != nil {
			return fmt.Errorf("reading %s's launchd environment failed: %w", d.Name, err)
		}
		if v != home.ControlFile(d.UDID) {
			return fmt.Errorf("%s's launchd environment has no stuntcam control file (re-armed by an older stuntcam?): run "+
				"`stuntcam up` again", d.Name)
		}
	}
	return nil
}

// setControl sets key=value in a simulator's control file (other lines kept), atomically. The sim must have been armed
// with the control file (stuntcam 0.1 arms without it: a re-arm is needed).
func setControl(d simctl.Device, key, value string) error {
	if err := controllable(d, key); err != nil {
		return err
	}
	return writeControl(d, key, value)
}

// writeControl is setControl without the checks, for a caller that has just run them (each launchd check is a
// `simctl spawn`, ~0.3 s).
func writeControl(d simctl.Device, key, value string) error {
	p := home.ControlFile(d.UDID)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	// read-modify-write under a lock, so a concurrent source and orient don't lose one change
	lk, err := os.OpenFile(p+".lock", os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer lk.Close()
	if err := syscall.Flock(int(lk.Fd()), syscall.LOCK_EX); err != nil {
		return err
	}
	defer syscall.Flock(int(lk.Fd()), syscall.LOCK_UN)
	b, _ := os.ReadFile(p)
	var lines []string
	for _, l := range strings.Split(string(b), "\n") {
		if k, _, ok := strings.Cut(strings.TrimSpace(l), "="); ok && k != key {
			lines = append(lines, strings.TrimSpace(l))
		}
	}
	lines = append(lines, key+"="+value)
	return home.WriteAtomic(p, []byte(strings.Join(lines, "\n")+"\n"))
}
