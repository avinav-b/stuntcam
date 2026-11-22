// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"os"
	"sort"
	"strings"
	"syscall"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/state"
)

// Why a fallback wasn't checked. Callers treat the first two as expected rather than as failures; the third makes
// fallbackFinder skip the sim's other apps.
var (
	errNotBooted     = errors.New("simulator not booted")
	errNotInstalled  = errors.New("app not installed")
	errNotResponding = errors.New("simctl get_app_container timed out (simulator not responding)")
)

// fallbackFinder finds the container-tmp fallbacks of one sim's apps. Once the sim turns out not booted or not
// responding, it gives the same reason for the rest of its apps without asking simctl again.
type fallbackFinder struct {
	device simctl.Device
	skip   error
}

func newFallbackFinder(device simctl.Device) *fallbackFinder {
	return &fallbackFinder{device: device}
}

// find finds an app's fallback through `simctl get_app_container <udid> <bundle> data`. A non-nil error says why it
// couldn't be checked: the sim isn't booted (simctl answers only for a booted device), the app isn't installed, simctl
// timed out, or simctl's error.
func (f *fallbackFinder) find(bundle string) (state.Fallback, error) {
	if f.device.State != simctl.Booted {
		return state.Fallback{}, errNotBooted
	}

	if f.skip != nil {
		return state.Fallback{}, f.skip
	}

	// a wedged "Booted" sim must not hold status or a clear for 120 s per bundle
	bounded := ctl
	if bounded.Timeout == 0 || bounded.Timeout > fallbackTimeout {
		bounded.Timeout = fallbackTimeout
	}

	out, err := bounded.AppContainer(f.device.UDID, bundle, "data")
	switch {
	case simctl.NotBooted(err):
		f.skip = errNotBooted
		return state.Fallback{}, f.skip
	case simctl.NotInstalled(err):
		return state.Fallback{}, errNotInstalled
	case simctl.TimedOut(err):
		f.skip = errNotResponding
		return state.Fallback{}, f.skip
	case err != nil:
		return state.Fallback{}, errors.New(firstLine(err.Error()))
	}

	containerPath, err := state.ParseAppContainer(out)
	if err != nil {
		return state.Fallback{}, err
	}

	fallback, err := state.FallbackFor(f.device.UDID, bundle, containerPath)
	if err != nil {
		return state.Fallback{}, err
	}

	if err := fallback.Check(); err != nil {
		return state.Fallback{}, err
	}

	return fallback, nil
}

// stateDirBlocked reports whether a state-dir error means the dir is missing, not a directory or not writable. The
// engine can't open such a dir either, so it keeps its state in the fallback, which the clear reached; that is not a
// reason to exit 1 (for example, a state dir blocked with mode 555).
func stateDirBlocked(err error) bool {
	return errors.Is(err, os.ErrNotExist) || errors.Is(err, syscall.ENOTDIR) || errors.Is(err, os.ErrPermission)
}

// uncheckedText groups bundle -> reason by reason: "simulator not booted" (it applies to every app), else "dev.a,
// dev.b: app not installed; …".
func uncheckedText(unchecked map[string]string) string {
	byReason := map[string][]string{}
	var reasons []string
	for _, bundle := range sortedKeys(unchecked) {
		if byReason[unchecked[bundle]] == nil {
			reasons = append(reasons, unchecked[bundle])
		}

		byReason[unchecked[bundle]] = append(byReason[unchecked[bundle]], bundle)
	}

	sort.Strings(reasons)
	var out []string
	for _, reason := range reasons {
		if reason == errNotBooted.Error() {
			out = append(out, reason)
		} else {
			out = append(out, strings.Join(byReason[reason], ", ")+": "+reason)
		}
	}

	return strings.Join(out, "; ")
}
