// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"strings"
)

func cmdOrient(args []string) error {
	fs := flag.NewFlagSet("orient", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	// UIDevice orientation names: landscapeLeft = the home button (or bottom edge) on the right
	valid := map[string]string{"portrait": "portrait", "upsidedown": "upsideDown", "portraitupsidedown": "upsideDown",
		"landscapeleft": "landscapeLeft", "landscaperight": "landscapeRight", "auto": "auto"}
	v, ok := "", false
	if fs.NArg() == 1 {
		v, ok = valid[strings.ToLower(fs.Arg(0))]
	}
	if !ok {
		return usagef("usage: stuntcam orient [--udid U] portrait|upsideDown|landscapeLeft|landscapeRight|auto")
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if err := setControl(d, "orientation", v); err != nil {
		return err
	}
	fmt.Printf("%s: orientation %s (apps follow within a second, if their UI allows it)\n", d.Name, v)
	return nil
}
