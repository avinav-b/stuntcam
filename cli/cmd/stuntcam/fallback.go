// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"os"
	"sort"
	"strings"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/state"
)

// fallbackOf finds an app's container-tmp fallback on a sim through `simctl get_app_container <udid> <bundle> data`.
// why != "" says why it couldn't be checked: the sim isn't booted (simctl answers only for a booted device), the app
// isn't installed, or simctl's error.
func fallbackOf(d simctl.Device, bundle string) (state.Fallback, string) {
	if d.State != "Booted" {
		return state.Fallback{}, "simulator not booted"
	}
	// the sim already timed out or turned out not booted: skip its other bundles
	if why := fallbackSkip[d.UDID]; why != "" {
		return state.Fallback{}, why
	}
	c := ctl
	if c.Timeout == 0 || c.Timeout > fallbackTimeout {
		c.Timeout = fallbackTimeout // a wedged "Booted" sim must not hold status or a clear for 120 s per bundle
	}
	out, err := c.AppContainer(d.UDID, bundle, "data")
	switch {
	case simctl.NotBooted(err):
		fallbackSkip[d.UDID] = "simulator not booted"
		return state.Fallback{}, "simulator not booted"
	case simctl.NotInstalled(err):
		return state.Fallback{}, "app not installed"
	case err != nil && strings.Contains(err.Error(), "timed out"):
		fallbackSkip[d.UDID] = "simctl get_app_container timed out (simulator not responding)"
		return state.Fallback{}, fallbackSkip[d.UDID]
	case err != nil:
		return state.Fallback{}, firstLine(err.Error())
	}
	cpath, err := state.ParseAppContainer(out)
	if err != nil {
		return state.Fallback{}, err.Error()
	}
	f, err := state.FallbackFor(d.UDID, bundle, cpath)
	if err != nil {
		return state.Fallback{}, err.Error()
	}
	if err := f.Check(); err != nil {
		return state.Fallback{}, err.Error()
	}
	return f, ""
}

// stateDirBlocked: the state dir is missing, not a directory or not writable. The engine can't open it either, so it
// keeps its state in the fallback, which the clear reached: not a reason to exit 1 (e.g. a state dir blocked with mode
// 555).
func stateDirBlocked(err error) bool {
	return errors.Is(err, os.ErrNotExist) || errors.Is(err, syscall.ENOTDIR) || errors.Is(err, os.ErrPermission)
}

// fallbackTimeout bounds each get_app_container call; fallbackSkip remembers, per sim, why its remaining bundles are
// skipped.
var (
	fallbackTimeout = 10 * time.Second
	fallbackSkip    = map[string]string{}
)

// uncheckedText groups bundle -> reason by reason: "simulator not booted" (it applies to every app), else "dev.a,
// dev.b: app not installed; …".
func uncheckedText(m map[string]string) string {
	by := map[string][]string{}
	var reasons []string
	for _, b := range sortedKeys(m) {
		if by[m[b]] == nil {
			reasons = append(reasons, m[b])
		}
		by[m[b]] = append(by[m[b]], b)
	}
	sort.Strings(reasons)
	var out []string
	for _, why := range reasons {
		if why == "simulator not booted" {
			out = append(out, why)
		} else {
			out = append(out, strings.Join(by[why], ", ")+": "+why)
		}
	}
	return strings.Join(out, "; ")
}
