// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"flag"
	"fmt"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/state"
)

func cmdSentinel(args []string) error {
	if len(args) == 0 || (args[0] != "ls" && args[0] != "clear") {
		return usagef("usage: stuntcam sentinel ls|clear [--udid U] [--bundle B] (the action comes first)")
	}

	flags := flag.NewFlagSet("sentinel", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	bundle := flags.String("bundle", "", "one app (default: every app with records on this simulator)")
	if err := parse(flags, args[1:], 0); err != nil {
		return err
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	cur := ""
	if _, loaderPath, err := home.ReadCurrent(device.UDID); err == nil {
		cur = filepath.Base(filepath.Dir(loaderPath))
	}

	bundles := state.Bundles(device.UDID)
	if *bundle != "" {
		bundles = []string{*bundle}
	}

	sort.Strings(bundles)
	switch args[0] {
	case "ls":
		sentinelList(device, cur, bundles)
		return nil
	case "clear":
		return sentinelClear(device, bundles)
	}

	return fmt.Errorf("unknown sentinel action %q", args[0])
}

// sentinelList prints each app's records (state dir, then container-tmp fallback), the sim's preflight-fail markers and
// the apps whose fallback couldn't be checked.
func sentinelList(device simctl.Device, cur string, bundles []string) {
	finder := newFallbackFinder(device)
	unchecked := map[string]string{}
	for _, bundle := range bundles {
		if reason := listBundle(device, finder, bundle, cur); reason != nil {
			unchecked[bundle] = reason.Error()
		}
	}

	for _, fail := range state.PreflightFails(device.UDID) { // per sim, not per bundle
		fmt.Printf("sim: %s\n", preflightLine(fail))
	}

	if len(unchecked) > 0 {
		fmt.Printf("container-tmp fallback not checked: %s\n", uncheckedText(unchecked))
	}
}

// listBundle prints one app's records for build cur ("" (not armed): the last activation's build) and returns why its
// container-tmp fallback couldn't be checked (nil if it was).
func listBundle(device simctl.Device, finder *fallbackFinder, bundle, cur string) error {
	app, err := state.Load(device.UDID, bundle, cur)
	if err == nil {
		fmt.Printf("%s: %d activations since the last clear", bundle, app.Activations)
		if latched := app.LatchedFeatures(); len(latched) > 0 {
			fmt.Printf(", LATCHED %s (%s)", strings.Join(latched, ","), state.Effect(latched))
		}

		fmt.Println()
		printSentinel(app, state.Markers(device.UDID, bundle))
	}

	fallback, reason := finder.find(bundle)
	if reason != nil {
		return reason
	}

	found := fallback.Read(bundle, cur)
	if found.Empty() {
		return nil
	}

	if err != nil {
		fmt.Printf("%s: no records in the state dir\n", bundle)
	}

	fmt.Printf("  container-tmp fallback %s:", fallback.Dir)
	if found.Events {
		fmt.Printf(" %d activations since the last clear", found.App.Activations)
		if latched := found.App.LatchedFeatures(); len(latched) > 0 { // conditional, as in status
			fmt.Printf(", LATCHED %s if the app again can't open its state dir (%s)", strings.Join(latched, ","),
				state.Effect(latched))
		}
	} else {
		fmt.Printf(" no events.log")
	}

	fmt.Println()
	printSentinel(found.App, found.Markers)
	for _, fail := range found.PreflightFails {
		fmt.Printf("  %s\n", preflightLine(fail))
	}

	return nil
}

// sentinelClear clears each app's records and fails when an app had nothing cleared, or a location the engine reads
// was left latched or unchecked for a reason other than "not booted"/"not installed": scripts must not read success.
func sentinelClear(device simctl.Device, bundles []string) error {
	finder := newFallbackFinder(device)
	unchecked := map[string]string{}
	hint := ""
	var failed, incomplete []string
	for _, bundle := range bundles {
		result := clearBundle(device, finder, bundle)
		if result.fallbackUnchecked != nil {
			unchecked[bundle] = result.fallbackUnchecked.Error()
		}

		if errors.Is(result.fallbackUnchecked, errNotBooted) {
			hint = " (an app that fell back there stays latched: clear again once the simulator is booted)"
		}

		if result.nothingCleared {
			failed = append(failed, bundle)
		}

		if result.incomplete {
			incomplete = append(incomplete, bundle)
		}
	}

	if len(unchecked) > 0 {
		fmt.Printf("container-tmp fallback not checked: %s%s\n", uncheckedText(unchecked), hint)
	}

	if len(failed) > 0 {
		return fmt.Errorf("nothing could be cleared for %s", strings.Join(failed, ", "))
	}

	if len(incomplete) > 0 {
		return fmt.Errorf("not fully cleared for %s (see above)", strings.Join(incomplete, ", "))
	}

	return nil
}

// clearResult is what clearBundle did for one app.
type clearResult struct {
	// fallbackUnchecked says why the container-tmp fallback wasn't checked (nil if it was).
	fallbackUnchecked error
	// nothingCleared is true when neither the state dir nor the fallback was cleared.
	nothingCleared bool
	// incomplete is true when a location the engine reads was reached and left latched, or couldn't be checked for a
	// reason other than "simulator not booted" or "app not installed".
	incomplete bool
}

// clearBundle clears one app's state dir and container-tmp fallback and prints one line saying what it did.
func clearBundle(device simctl.Device, finder *fallbackFinder, bundle string) clearResult {
	stateErr := state.Clear(device.UDID, bundle, "stuntcam sentinel clear")
	fallback, uncheckedReason := finder.find(bundle)
	var done []string
	var fallbackErr error
	if uncheckedReason == nil {
		done, fallbackErr = fallback.Clear("stuntcam sentinel clear")
	}

	var msg []string
	if stateErr == nil {
		msg = append(msg, "cleared (state dir)")
	} else {
		msg = append(msg, "state dir not cleared: "+stateErr.Error())
	}

	if len(done) > 0 {
		msg = append(msg, fmt.Sprintf("container-tmp fallback %s: %s", fallback.Dir, strings.Join(done, ", ")))
	}

	if fallbackErr != nil {
		msg = append(msg, fmt.Sprintf("container-tmp fallback %s not fully cleared: %v", fallback.Dir, fallbackErr))
	}

	// "Not booted" and "not installed" don't fail the clear: the summary says to clear again once the simulator is
	// booted, and an app that isn't installed has no container to clear.
	uncheckedUnexpectedly := uncheckedReason != nil && !errors.Is(uncheckedReason, errNotInstalled) &&
		!errors.Is(uncheckedReason, errNotBooted)
	// A state dir the engine can't open either (stateDirBlocked) leaves the engine on the fallback, which was reached.
	stateDirFailed := stateErr != nil && !stateDirBlocked(stateErr)
	// The camera claim holds when every location the engine reads was reached (no container: nothing there).
	if stateErr == nil && fallbackErr == nil && (uncheckedReason == nil || errors.Is(uncheckedReason, errNotInstalled)) {
		msg = append(msg, "its next launch gets the camera again")
	}

	fmt.Printf("%s: %s\n", bundle, strings.Join(msg, "; "))
	return clearResult{
		fallbackUnchecked: uncheckedReason,
		nothingCleared:    stateErr != nil && len(done) == 0,
		incomplete:        fallbackErr != nil || uncheckedUnexpectedly || stateDirFailed,
	}
}

// printSentinel prints the counted events, the debug-end note and the markers of one location.
func printSentinel(app state.App, markers []state.Marker) {
	for _, event := range app.Counted {
		fmt.Printf("  %s %s %s (activation %d, build %s) %s\n", time.Unix(event.At, 0).Format("2006-01-02 15:04:05"),
			event.Kind, event.Feature, event.Activation, event.Build, event.Detail)
	}

	if app.UnknownDebugEnd != "" {
		fmt.Printf("  last debug run ended inside a %s op (not counted; if stuntcam crashed there, report it)\n",
			app.UnknownDebugEnd)
	}

	for _, marker := range markers {
		effect := "expired"
		if marker.Valid(time.Now()) {
			effect = marker.Effect()
		}

		fmt.Printf("  marker %s: %s until %s (%s)\n", marker.Name(), marker.Features,
			marker.Until.Format("2006-01-02 15:04:05"), effect)
	}
}

func preflightLine(fail state.PreflightFail) string {
	effect := "expired"
	if fail.Valid(time.Now()) {
		effect = "launches of this build get stock CoreMotion; the camera runs its own preflight"
	}
	return fmt.Sprintf("marker preflight-fail-%s: %s until %s (%s)", fail.Build, fail.What,
		fail.Until.Format("2006-01-02 15:04:05"), effect)
}
