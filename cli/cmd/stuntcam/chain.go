// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

func cmdChain(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return usagef("usage: stuntcam chain add|rm|ls [--udid U] [LIB] (the action comes first)")
	}
	if args[0] != "add" && args[0] != "rm" && args[0] != "ls" {
		return usagef("unknown chain action %q (add, rm, ls)", args[0])
	}
	fs := flag.NewFlagSet("chain", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	maxPos := 1
	if args[0] == "ls" {
		maxPos = 0
	}
	if err := parse(fs, args[1:], maxPos); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	p := home.ChainFile(d.UDID)
	b, _ := os.ReadFile(p)
	var cur []string
	for _, l := range strings.Split(string(b), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			cur = append(cur, l)
		}
	}
	switch args[0] {
	case "ls":
		for _, l := range cur {
			fmt.Println(l)
		}
		return nil
	case "add", "rm":
		if fs.NArg() != 1 {
			return usagef("usage: stuntcam chain %s [--udid U] LIB", args[0])
		}
		lib, _ := filepath.Abs(fs.Arg(0))
		var next []string
		for _, l := range cur {
			if l != lib {
				next = append(next, l)
			}
		}
		if args[0] == "add" {
			if strings.HasPrefix(filepath.Base(lib), "libsimcam") {
				return errors.New("stuntcam's own libraries are never chained (the loader skips them)")
			}
			if fi, err := os.Stat(lib); err != nil || fi.IsDir() {
				return fmt.Errorf("%s: no such library", lib)
			}
			if home.Protected(lib) {
				return fmt.Errorf("%s is in a privacy-protected folder the simulator can't read; copy it under %s", lib,
					home.Root())
			}
			next = append(next, lib)
		}
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		data := strings.Join(next, "\n")
		if data != "" {
			data += "\n"
		}
		if err := home.WriteAtomic(p, []byte(data)); err != nil {
			return err
		}
		fmt.Printf("chain for %s: %d %s (loaded from the next app launch)\n", d.Name, len(next),
			plural(len(next), "library", "libraries"))
		return nil
	}
	return fmt.Errorf("unknown chain action %q", args[0])
}
