// SPDX-License-Identifier: Apache-2.0

package main

import (
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
	fs := flag.NewFlagSet("sentinel", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	bundle := fs.String("bundle", "", "one app (default: every app with records on this simulator)")
	if err := parse(fs, args[1:], 0); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	cur := ""
	if _, l, err := home.ReadCurrent(d.UDID); err == nil {
		cur = filepath.Base(filepath.Dir(l))
	}
	bundles := state.Bundles(d.UDID)
	if *bundle != "" {
		bundles = []string{*bundle}
	}
	sort.Strings(bundles)
	switch args[0] {
	case "ls":
		sentinelList(d, cur, bundles)
		return nil
	case "clear":
		return sentinelClear(d, bundles)
	}
	return fmt.Errorf("unknown sentinel action %q", args[0])
}

// sentinelList prints each app's records (state dir, then container-tmp fallback), the sim's preflight-fail markers and
// the apps whose fallback couldn't be checked.
func sentinelList(d simctl.Device, cur string, bundles []string) {
	unchecked := map[string]string{}
	for _, b := range bundles {
		if why := listBundle(d, b, cur); why != "" {
			unchecked[b] = why
		}
	}
	for _, p := range state.PreflightFails(d.UDID) { // per sim, not per bundle
		fmt.Printf("sim: %s\n", preflightLine(p))
	}
	if len(unchecked) > 0 {
		fmt.Printf("container-tmp fallback not checked: %s\n", uncheckedText(unchecked))
	}
}

// listBundle prints one app's records for build cur ("" (not armed): the last activation's build) and returns why its
// container-tmp fallback couldn't be checked ("" if it was).
func listBundle(d simctl.Device, b, cur string) string {
	a, err := state.Load(d.UDID, b, cur)
	if err == nil {
		fmt.Printf("%s: %d activations since the last clear", b, a.Activations)
		if f := a.LatchedFeatures(); len(f) > 0 {
			fmt.Printf(", LATCHED %s (%s)", strings.Join(f, ","), state.Effect(f))
		}
		fmt.Println()
		printSentinel(a, state.Markers(d.UDID, b))
	}
	f, why := fallbackOf(d, b)
	if why != "" {
		return why
	}
	fb := f.Read(b, cur)
	if fb.Empty() {
		return ""
	}
	if err != nil {
		fmt.Printf("%s: no records in the state dir\n", b)
	}
	fmt.Printf("  container-tmp fallback %s:", f.Dir)
	if fb.Events {
		fmt.Printf(" %d activations since the last clear", fb.App.Activations)
		if lf := fb.App.LatchedFeatures(); len(lf) > 0 { // conditional, as in status
			fmt.Printf(", LATCHED %s if the app again can't open its state dir (%s)", strings.Join(lf, ","), state.Effect(lf))
		}
	} else {
		fmt.Printf(" no events.log")
	}
	fmt.Println()
	printSentinel(fb.App, fb.Markers)
	for _, p := range fb.PreflightFails {
		fmt.Printf("  %s\n", preflightLine(p))
	}
	return ""
}

// sentinelClear clears each app's records and fails when an app had nothing cleared, or a location the engine reads
// was left latched or unchecked for a reason other than "not booted"/"not installed": scripts must not read success.
func sentinelClear(d simctl.Device, bundles []string) error {
	unchecked := map[string]string{}
	var failed, incomplete []string
	for _, b := range bundles {
		why, nothing, partly := clearBundle(d, b)
		if why != "" {
			unchecked[b] = why
		}
		if nothing {
			failed = append(failed, b)
		}
		if partly {
			incomplete = append(incomplete, b)
		}
	}
	if len(unchecked) > 0 {
		hint := ""
		for _, w := range unchecked {
			if w == "simulator not booted" {
				hint = " (an app that fell back there stays latched: clear again once the simulator is booted)"
			}
		}
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

// clearBundle clears one app's state dir and container-tmp fallback and prints one line saying what it did. why: why
// the fallback wasn't checked; nothing: neither location was cleared; partly: a location the engine reads was reached
// and left latched, or couldn't be checked for a reason other than "not booted"/"not installed".
func clearBundle(d simctl.Device, b string) (why string, nothing, partly bool) {
	stateErr := state.Clear(d.UDID, b, "stuntcam sentinel clear")
	f, why := fallbackOf(d, b)
	var done []string
	var fallbackErr error
	if why == "" {
		done, fallbackErr = f.Clear("stuntcam sentinel clear")
	}
	var msg []string
	if stateErr == nil {
		msg = append(msg, "cleared (state dir)")
	} else {
		msg = append(msg, "state dir not cleared: "+stateErr.Error())
	}
	if len(done) > 0 {
		msg = append(msg, fmt.Sprintf("container-tmp fallback %s: %s", f.Dir, strings.Join(done, ", ")))
	}
	if fallbackErr != nil {
		msg = append(msg, fmt.Sprintf("container-tmp fallback %s not fully cleared: %v", f.Dir, fallbackErr))
	}
	nothing = stateErr != nil && len(done) == 0
	partly = fallbackErr != nil || (why != "" && why != "app not installed" && why != "simulator not booted") ||
		(stateErr != nil && !stateDirBlocked(stateErr))
	// the camera claim holds when every location the engine reads was reached (no container: nothing there)
	if stateErr == nil && fallbackErr == nil && (why == "" || why == "app not installed") {
		msg = append(msg, "its next launch gets the camera again")
	}
	fmt.Printf("%s: %s\n", b, strings.Join(msg, "; "))
	return why, nothing, partly
}

// printSentinel prints the counted events, the debug-end note and the markers of one location.
func printSentinel(a state.App, ms []state.Marker) {
	for _, e := range a.Counted {
		fmt.Printf("  %s %s %s (activation %d, build %s) %s\n", time.Unix(e.At, 0).Format("2006-01-02 15:04:05"), e.Kind,
			e.Feature, e.Activation, e.Build, e.Detail)
	}
	if a.UnknownDebugEnd != "" {
		fmt.Printf("  last debug run ended inside a %s op (not counted; if stuntcam crashed there, report it)\n",
			a.UnknownDebugEnd)
	}
	for _, m := range ms {
		v := "expired"
		if m.Valid(time.Now()) {
			v = m.Effect()
		}
		fmt.Printf("  marker %s: %s until %s (%s)\n", m.Name(), m.Features, m.Until.Format("2006-01-02 15:04:05"), v)
	}
}

func preflightLine(p state.PreflightFail) string {
	v := "expired"
	if p.Valid(time.Now()) {
		v = "launches of this build get stock CoreMotion; the camera runs its own preflight"
	}
	return fmt.Sprintf("marker preflight-fail-%s: %s until %s (%s)", p.Build, p.What,
		p.Until.Format("2006-01-02 15:04:05"), v)
}
