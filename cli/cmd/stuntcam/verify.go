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
	fs := flag.NewFlagSet("verify", flag.ContinueOnError)
	rt := fs.String("runtime", "", "iOS version (e.g. 26.3) or runtime identifier; default the newest")
	fresh := fs.Bool("fresh", false, "ignore the cached verdict")
	keep := fs.Bool("keep", false, "leave the private device booted")
	asJSON := fs.Bool("json", false, "JSON output")
	build := fs.String("build", "", "build dir")
	if err := parse(fs, args, 0); err != nil {
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
	r, err := verify.Run(set, verify.Options{Runtime: *rt, Fresh: *fresh, Keep: *keep, Progress: progress})
	if err != nil {
		return usageError{err} // could not start: not a verdict (exit 64, as scripts/verify.sh)
	}
	if *asJSON {
		e := json.NewEncoder(os.Stdout)
		e.SetIndent("", " ")
		if err := e.Encode(r); err != nil {
			return err
		}
	} else {
		src := fmt.Sprintf("device %s: create %ds, boot %ds, canary %ds", r.Device, r.CreateSeconds, r.BootSeconds,
			r.CanarySeconds)
		if r.Shipped {
			src = "shipped with this package (" + r.ShippedSource() + ")"
		} else if r.Cached {
			src = "cached " + time.Unix(r.At, 0).Format("2006-01-02 15:04")
		}
		fmt.Printf("verdict %s  key=%s  iOS %s (%s)  %s\n", r.Verdict, r.Key, r.RuntimeVer, r.RuntimeBuild, src)
		if r.RetriedPass() {
			fmt.Println(retriedLine(r))
		}
		for _, n := range r.Notes {
			fmt.Println("  " + n)
		}
	}
	switch r.State {
	case verify.Fail:
		return exitStatus(exitFailed)
	case verify.Inconclusive:
		return exitStatus(exitInconclusive)
	}
	return nil
}

// retriedLine is the one line `up` and `verify` print for a retried pass.
func retriedLine(r verify.Result) string {
	what := "the canary's retry of a photo's frame-gap check (" + r.Retried + ")"
	switch {
	case r.FailedFirst != "": // one launch failed on content, the launch run to confirm it passed
		what = "a second canary launch after the first FAILED (" + r.FailedFirst + "); an intermittent engine bug looks " +
			"like this too"
	case r.Retried == "":
		what = "a second canary launch (the first: " + r.Relaunched + ")"
	}
	cache := "it is cached for " + verify.RetriedPassTTLText() + " only"
	if r.NoCache != "" { // SIMCTL_CHILD_* & co. bypass the cache: nothing was cached
		cache = "it is not cached (" + r.NoCache + ")"
	}
	return fmt.Sprintf("note: this pass needed %s; %s. `stuntcam verify --fresh` re-runs it", what, cache)
}

// cmdPrivateShutdown is verify.ShutdownCommand: shut a private canary device down and release its verify lock.
func cmdPrivateShutdown(args []string) int {
	fs := flag.NewFlagSet(verify.ShutdownCommand, flag.ContinueOnError)
	udid := fs.String("udid", "", "private device")
	lock := fs.String("lock", "", "verify lock dir handed over")
	parent := fs.Int("parent", 0, "pid of the verify that handed it over")
	if err := fs.Parse(args); err != nil || *udid == "" || *lock == "" {
		return exitUsage
	}
	if err := verify.FinishShutdown(*udid, *lock, *parent); err != nil {
		fmt.Println("error:", err)
		return exitFailed
	}
	return exitOK
}
