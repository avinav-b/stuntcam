// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
)

func cmdDown(args []string) error {
	fs := flag.NewFlagSet("down", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if err := arm.Disarm(ctl, d.UDID, d.State == "Booted"); err != nil {
		return err
	}
	fmt.Printf("%s (%s): new launches no longer load stuntcam; apps already running keep it until they quit\n",
		d.Name, d.UDID)
	return endWebcam(d.UDID) // its camera helper stops unless another simulator still uses it
}
