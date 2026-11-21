// SPDX-License-Identifier: Apache-2.0

package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/trace"
)

// cmdTrace prints an app's op ring (design §4.5; engine/src/core/trace.inc): the newest trace-<pid>.txt in its state
// dir or its container-tmp fallback (found as `sentinel` finds it), or the one of --pid. The app may be running (the
// file is rewritten at most every 150 ms after an op) or gone (its last copy). No trace: exit 1.
func cmdTrace(args []string) error {
	fs := flag.NewFlagSet("trace", flag.ContinueOnError)
	asJSON := fs.Bool("json", false, "JSON output (schema: cli/README.md)")
	udid := fs.String("udid", "", "simulator UDID")
	pid := fs.Int("pid", 0, "that process's trace (default: the newest)")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	if fs.NArg() != 1 {
		return usagef("usage: stuntcam trace [--json] [--udid U] [--pid N] BUNDLE")
	}
	b := fs.Arg(0)
	if b == "" || b == "." || b == ".." || strings.ContainsAny(b, "/\x00") {
		return usagef("bad bundle id %q", b)
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	locs := []trace.Location{{Dir: filepath.Join(home.StateDir(d.UDID), b), Kind: "state-dir"}}
	f, why := fallbackOf(d, b)
	if why == "" {
		locs = append(locs, trace.Location{Dir: f.Dir, Kind: "container-tmp"})
	}
	files, skipped := trace.Find(locs)
	for _, s := range skipped {
		fmt.Fprintln(os.Stderr, "stuntcam: skipped", s)
	}
	if len(files) == 0 {
		msg := fmt.Sprintf("no trace for %s on %s: none in %s", b, d.Name, locs[0].Dir)
		if why == "" {
			msg += " or " + f.Dir
		} else {
			msg += fmt.Sprintf(" (container-tmp fallback not checked: %s)", why)
		}
		return errors.New(msg + "; an app writes one once it uses the camera on an armed simulator")
	}
	t, others, err := trace.Pick(files, *pid)
	if err != nil {
		return err
	}
	r := trace.Build(t, others, d.UDID, d.Name, appRunning(d, b, t.Pid))
	if *asJSON {
		e := json.NewEncoder(os.Stdout)
		e.SetIndent("", " ")
		return e.Encode(r)
	}
	trace.Human(os.Stdout, r, time.Now(), time.Local)
	return nil
}

// appRunning: whether pid is bundle's running app process on the sim (nil: couldn't tell)
func appRunning(d simctl.Device, bundle string, pid int) *bool {
	no := false
	if d.State != "Booted" {
		return &no
	}
	c := ctl
	if c.Timeout == 0 || c.Timeout > fallbackTimeout {
		c.Timeout = fallbackTimeout
	}
	out, err := c.Spawn(d.UDID, "launchctl", "list")
	if err != nil {
		return nil
	}
	v := trace.Running(out, bundle, pid)
	return &v
}
