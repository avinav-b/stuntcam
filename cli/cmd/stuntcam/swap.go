// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
)

func cmdSwap(args []string) error {
	flags := flag.NewFlagSet("swap", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	if err := parse(flags, args, 1); err != nil {
		return err
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	if _, _, err := home.ReadCurrent(device.UDID); err != nil {
		return fmt.Errorf("%s isn't armed (no current): `stuntcam up` first; swap only changes the build of an armed "+
			"simulator", device.Name)
	}

	var set artifacts.Set
	if flags.NArg() > 0 {
		set, err = artifacts.Find(filepath.Dir(flags.Arg(0)))
	} else {
		set, err = artifacts.Find("")
	}

	if err != nil {
		return err
	}

	if set, err = artifacts.Stage(set); err != nil {
		return err
	}

	if err := home.WriteCurrent(device.UDID, set.Loader); err != nil {
		return err
	}

	// as `env` does: a sim armed before config.json existed gets its record
	if _, err := arm.ReadConfig(device.UDID); err != nil {
		recordXcode(device.UDID, set.Rules, "swap", noXcodeSummary, signalsFromLaunchdSim)
	} else if set.Rules != "" { // the new build's restore rules apply from the next launch too
		if err := arm.SetRulesDir(device.UDID, set.Rules); err != nil {
			fmt.Fprintf(os.Stderr, "warning: pointing %s at the new build's restore rules: %v\n", device.Name, err)
		}
	}

	if r, err := arm.ReadRecord(device.UDID); err == nil {
		r.Build = set.BuildID()
		_ = arm.WriteRecord(device.UDID, r) // the swap worked; an old build in the record only shows in `status`
	}

	fmt.Printf("%s now uses build %s from the next app launch (running apps keep theirs)\n", device.Name, set.BuildID())
	return nil
}
