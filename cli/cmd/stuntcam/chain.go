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

// cmdChain runs `stuntcam chain add|rm|ls`: it lists or edits the extra libraries the loader loads into the
// simulator's apps.
func cmdChain(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return usagef("usage: stuntcam chain add|rm|ls [--udid U] [LIB] (the action comes first)")
	}

	action := args[0]
	if action != "add" && action != "rm" && action != "ls" {
		return usagef("unknown chain action %q (add, rm, ls)", action)
	}

	flags := flag.NewFlagSet("chain", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	maxPos := 1
	if action == "ls" {
		maxPos = 0
	}

	if err := parse(flags, args[1:], maxPos); err != nil {
		return err
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	chainPath := home.ChainFile(device.UDID)
	current := readChain(chainPath)
	if action == "ls" {
		for _, lib := range current {
			fmt.Println(lib)
		}

		return nil
	}

	if flags.NArg() != 1 {
		return usagef("usage: stuntcam chain %s [--udid U] LIB", action)
	}

	lib, _ := filepath.Abs(flags.Arg(0)) // fails only if the working directory is gone
	next := without(current, lib)
	if action == "add" {
		if err := checkChainable(lib); err != nil {
			return err
		}

		next = append(next, lib)
	}

	if err := writeChain(chainPath, next); err != nil {
		return err
	}

	fmt.Printf("chain for %s: %d %s (loaded from the next app launch)\n", device.Name, len(next),
		plural(len(next), "library", "libraries"))
	return nil
}

// readChain returns the chain file's libraries, one per non-blank line. A missing file is an empty chain.
func readChain(chainPath string) []string {
	contents, _ := os.ReadFile(chainPath) // no file: an empty chain
	var libs []string
	for _, line := range strings.Split(string(contents), "\n") {
		if line = strings.TrimSpace(line); line != "" {
			libs = append(libs, line)
		}
	}

	return libs
}

// checkChainable refuses a library that `chain add` can't use: one of stuntcam's own, a missing file or a directory,
// or a file the simulator can't read.
func checkChainable(lib string) error {
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

	return nil
}

// writeChain replaces the chain file with libs, one per line.
func writeChain(chainPath string, libs []string) error {
	if err := os.MkdirAll(filepath.Dir(chainPath), 0o755); err != nil {
		return err
	}

	data := strings.Join(libs, "\n")
	if data != "" {
		data += "\n"
	}

	return home.WriteAtomic(chainPath, []byte(data))
}
