// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"path/filepath"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
)

// version and commit are set by cli/build.sh: -ldflags "-X main.version=<VERSION file> -X main.commit=<git sha>"
// (scripts/package.sh builds the packaged binary the same way, so `stuntcam version` names the package version)
var (
	version = "0.0.0-dev"
	commit  = ""
)

func cmdVersion(args []string) error {
	flags := flag.NewFlagSet("version", flag.ContinueOnError)
	check := flags.Bool("check", false, "verify every installed file against the package manifest and unpack the "+
		"payload (dylibs, canary) into the host directory")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	fmt.Println(versionLine())
	if !*check {
		return nil
	}

	// the binary's own package, whatever the working directory (a checkout), --build or STUNTCAM_BUILD say
	set, err := artifacts.InstalledHere()
	if err != nil {
		return err
	}

	fmt.Printf("package %s: every file matches %s\n", set.Version, filepath.Join(set.From, artifacts.ManifestName))
	if ok, manifestSum := artifacts.Anchored(); ok {
		fmt.Printf("manifest.txt and payload.tar are the ones this binary was packaged with "+
			"(manifest sha256 %.12s)\n", manifestSum)
	} else {
		fmt.Println("this binary carries no package hashes (a dev build): manifest.txt is checked for consistency only")
	}

	for _, path := range []string{set.Engine, set.Loader, set.Stub, set.Hook, set.Canary, set.Helper} {
		if path != "" {
			fmt.Println("unpacked " + path)
		}
	}

	return nil
}

func versionLine() string {
	if commit == "" {
		return "stuntcam " + version
	}
	return "stuntcam " + version + " (commit " + commit + ")"
}
