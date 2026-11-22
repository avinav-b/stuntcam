// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"strings"
)

func cmdOrient(args []string) error {
	flags := flag.NewFlagSet("orient", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	if err := parse(flags, args, 1); err != nil {
		return err
	}

	// UIDevice orientation names: landscapeLeft = the home button (or bottom edge) on the right
	valid := map[string]string{"portrait": "portrait", "upsidedown": "upsideDown", "portraitupsidedown": "upsideDown",
		"landscapeleft": "landscapeLeft", "landscaperight": "landscapeRight", "auto": "auto"}
	orientation, ok := "", false
	if flags.NArg() == 1 {
		orientation, ok = valid[strings.ToLower(flags.Arg(0))]
	}

	if !ok {
		return usagef("usage: stuntcam orient [--udid U] portrait|upsideDown|landscapeLeft|landscapeRight|auto")
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	if err := setControl(device, "orientation", orientation); err != nil {
		return err
	}

	fmt.Printf("%s: orientation %s (apps follow within a second, if their UI allows it)\n", device.Name, orientation)
	return nil
}
