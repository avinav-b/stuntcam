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
	flags := flag.NewFlagSet("env", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	build := flags.String("build", "", "build dir")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	device, _, err := pickSim(*udid)
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

	if err := envArm(device, set, "env"); err != nil {
		return err
	}

	fmt.Printf("# stuntcam for launches on %s (%s); a launch that sets its own DYLD_INSERT_LIBRARIES replaces this "+
		"one\n", device.Name, device.UDID)
	for _, kv := range arm.Vars(device.UDID, set, arm.Options{}) {
		key, value, _ := strings.Cut(kv, "=")
		fmt.Printf("export SIMCTL_CHILD_%s=%s\n", key, shellQuote(value))
	}

	return nil
}

// envArm prepares a booted sim for per-launch arming, for `stuntcam env` and for exec's runner hook on an unarmed
// destination. It writes:
//   - the sim's dirs;
//   - `current`, if it is missing. An existing one is kept, because replacing it would undo a `stuntcam swap`;
//   - the Xcode record;
//   - an "env" arm record, unless there is one of another mode.
func envArm(device simctl.Device, set artifacts.Set, source string) error {
	if _, _, err := home.ReadCurrent(device.UDID); err == nil {
		set.Loader = "" // Prepare skips current when Loader is empty
	} else if rec, err := arm.ReadRecord(device.UDID); err == nil &&
		arm.SameBoot(rec, device.State == simctl.Booted, device.LastBooted) {
		return fmt.Errorf("%s was booted armed and then `stuntcam down`: writing current would re-arm every launch "+
			"on it, not just yours; `stuntcam up` to re-arm, or reboot it unarmed first", device.Name)
	}

	wroteCurrent := set.Loader != ""
	if err := arm.Prepare(device.UDID, set); err != nil {
		return err
	}

	// keep the record of an `up` (it may be for another Xcode on purpose)
	if _, err := arm.ReadConfig(device.UDID); err != nil || wroteCurrent {
		recordXcode(device.UDID, set.Rules, source, noXcodeSummary, signalsFromLaunchdSim)
	}

	// mark it env-armed so status and the service don't take it for script-armed; another mode's record (its Vars,
	// which `down` needs) stays
	if old, err := arm.ReadRecord(device.UDID); wroteCurrent && (err != nil || old.Mode == arm.Env) {
		rec := arm.Record{Mode: arm.Env, Build: set.BuildID(), At: time.Now().Unix()}
		if err := arm.WriteRecord(device.UDID, rec); err != nil {
			return err
		}
	}

	return nil
}
