// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

func cmdEnv(args []string) error {
	fs := flag.NewFlagSet("env", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	build := fs.String("build", "", "build dir")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	set, err := artifacts.Find(*build)
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return err
	}
	if err := envArm(d, set, "env"); err != nil {
		return err
	}
	fmt.Printf("# stuntcam for launches on %s (%s); a launch that sets its own DYLD_INSERT_LIBRARIES replaces this one\n",
		d.Name, d.UDID)
	for _, kv := range arm.Vars(d.UDID, set, arm.Options{}) {
		k, v, _ := strings.Cut(kv, "=")
		fmt.Printf("export SIMCTL_CHILD_%s=%s\n", k, shellQuote(v))
	}
	return nil
}

// envArm prepares a booted sim for per-launch arming (`stuntcam env`, and exec's runner hook on an unarmed
// destination): its dirs, `current` if missing (an existing one is kept: a `stuntcam swap` would otherwise be undone),
// the Xcode record, and an "env" arm record unless one of another mode exists.
func envArm(d simctl.Device, set artifacts.Set, source string) error {
	if _, _, err := home.ReadCurrent(d.UDID); err == nil {
		set.Loader = "" // Prepare skips current when Loader is empty
	} else if rec, err := arm.ReadRecord(d.UDID); err == nil && arm.SameBoot(rec, d.State == "Booted", d.LastBooted) {
		return fmt.Errorf("%s was booted armed and then `stuntcam down`: writing current would re-arm every launch on it, "+
			"not just yours; `stuntcam up` to re-arm, or reboot it unarmed first", d.Name)
	}
	wroteCurrent := set.Loader != ""
	if err := arm.Prepare(d.UDID, set); err != nil {
		return err
	}
	// keep the record of an `up` (it may be for another Xcode on purpose)
	if _, err := arm.ReadConfig(d.UDID); err != nil || wroteCurrent {
		recordXcode(d.UDID, set.Rules, source, false, false)
	}
	// mark it env-armed so status and the service don't take it for script-armed; another mode's record (its Vars, which
	// `down` needs) stays
	if old, err := arm.ReadRecord(d.UDID); wroteCurrent && (err != nil || old.Mode == arm.Env) {
		rec := arm.Record{Mode: arm.Env, Build: set.BuildID(), At: time.Now().Unix()}
		if err := arm.WriteRecord(d.UDID, rec); err != nil {
			return err
		}
	}
	return nil
}
