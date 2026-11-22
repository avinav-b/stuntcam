// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

func cmdDown(args []string) error {
	flags := flag.NewFlagSet("down", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	if err := arm.Disarm(ctl, device.UDID, device.State == simctl.Booted); err != nil {
		return err
	}

	fmt.Printf("%s (%s): new launches no longer load stuntcam; apps already running keep it until they quit\n",
		device.Name, device.UDID)
	return endWebcam(device.UDID) // its camera helper stops unless another simulator still uses it
}
