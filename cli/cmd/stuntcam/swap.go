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
	fs := flag.NewFlagSet("swap", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if _, _, err := home.ReadCurrent(d.UDID); err != nil {
		return fmt.Errorf("%s isn't armed (no current): `stuntcam up` first; swap only changes the build of an armed "+
			"simulator", d.Name)
	}
	var set artifacts.Set
	if fs.NArg() > 0 {
		set, err = artifacts.Find(filepath.Dir(fs.Arg(0)))
	} else {
		set, err = artifacts.Find("")
	}
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return err
	}
	if err := home.WriteCurrent(d.UDID, set.Loader); err != nil {
		return err
	}
	// as `env` does: a sim armed before config.json existed gets its record
	if _, err := arm.ReadConfig(d.UDID); err != nil {
		recordXcode(d.UDID, set.Rules, "swap", false, false)
	} else if set.Rules != "" { // the new build's restore rules apply from the next launch too
		if err := arm.SetRulesDir(d.UDID, set.Rules); err != nil {
			fmt.Fprintf(os.Stderr, "warning: pointing %s at the new build's restore rules: %v\n", d.Name, err)
		}
	}
	if r, err := arm.ReadRecord(d.UDID); err == nil {
		r.Build = set.BuildID()
		_ = arm.WriteRecord(d.UDID, r)
	}
	fmt.Printf("%s now uses build %s from the next app launch (running apps keep theirs)\n", d.Name, set.BuildID())
	return nil
}
