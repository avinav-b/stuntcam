// SPDX-License-Identifier: Apache-2.0

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/verify"
)

func cmdVerify(args []string) error {
	flags := flag.NewFlagSet("verify", flag.ContinueOnError)
	runtime := flags.String("runtime", "", "iOS version (e.g. 26.3) or runtime identifier; default the newest")
	fresh := flags.Bool("fresh", false, "ignore the cached verdict")
	keep := flags.Bool("keep", false, "leave the private device booted")
	asJSON := flags.Bool("json", false, "JSON output")
	build := flags.String("build", "", "build dir")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	set, err := artifacts.Find(*build)
	if err != nil {
		return usageError{err}
	}

	if set, err = artifacts.Stage(set); err != nil {
		return usageError{err}
	}

	progress := func(s string) { fmt.Fprintln(os.Stderr, s) }
	result, err := verify.Run(set, verify.Options{Runtime: *runtime, Fresh: *fresh, Keep: *keep, Progress: progress})
	if err != nil {
		return usageError{err} // could not start: not a verdict (exit 64, as scripts/verify.sh)
	}

	if *asJSON {
		encoder := json.NewEncoder(os.Stdout)
		encoder.SetIndent("", " ")
		if err := encoder.Encode(result); err != nil {
			return err
		}
	} else {
		origin := fmt.Sprintf("device %s: create %ds, boot %ds, canary %ds", result.Device, result.CreateSeconds,
			result.BootSeconds, result.CanarySeconds)
		if result.Shipped {
			origin = "shipped with this package (" + result.ShippedSource() + ")"
		} else if result.Cached {
			origin = "cached " + time.Unix(result.At, 0).Format("2006-01-02 15:04")
		}

		fmt.Printf("verdict %s  key=%s  iOS %s (%s)  %s\n", result.Verdict, result.Key, result.RuntimeVer,
			result.RuntimeBuild, origin)
		if result.RetriedPass() {
			fmt.Println(retriedLine(result))
		}

		for _, n := range result.Notes {
			fmt.Println("  " + n)
		}
	}

	switch result.State {
	case verify.Fail:
		return exitStatus(exitFailed)
	case verify.Inconclusive:
		return exitStatus(exitInconclusive)
	}

	return nil
}

// retriedLine is the one line `up` and `verify` print for a retried pass.
func retriedLine(result verify.Result) string {
	what := "the canary's retry of a photo's frame-gap check (" + result.Retried + ")"
	switch {
	case result.FailedFirst != "": // one launch failed on content, the launch run to confirm it passed
		what = "a second canary launch after the first FAILED (" + result.FailedFirst + "); an intermittent engine " +
			"bug looks like this too"
	case result.Retried == "":
		what = "a second canary launch (the first: " + result.Relaunched + ")"
	}

	cache := "it is cached for " + verify.RetriedPassTTLText() + " only"
	if result.NoCache != "" { // SIMCTL_CHILD_* & co. bypass the cache: nothing was cached
		cache = "it is not cached (" + result.NoCache + ")"
	}

	return fmt.Sprintf("note: this pass needed %s; %s. `stuntcam verify --fresh` re-runs it", what, cache)
}

// cmdPrivateShutdown is verify.ShutdownCommand: shut a private canary device down and release its verify lock.
func cmdPrivateShutdown(args []string) int {
	flags := flag.NewFlagSet(verify.ShutdownCommand, flag.ContinueOnError)
	udid := flags.String("udid", "", "private device")
	lock := flags.String("lock", "", "verify lock dir handed over")
	parent := flags.Int("parent", 0, "pid of the verify that handed it over")
	if err := flags.Parse(args); err != nil || *udid == "" || *lock == "" {
		return exitUsage
	}

	if err := verify.FinishShutdown(*udid, *lock, *parent); err != nil {
		fmt.Println("error:", err)
		return exitFailed
	}

	return exitOK
}
