// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"cmp"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// privateDevice is the private canary device for a runtime build.
type privateDevice struct {
	udid    string
	state   simctl.State // as simctl lists it (Shutdown for a new one)
	created bool         // made by this verify
}

// deviceCreateTimeout bounds a device create: a first create on a busy Mac can be slow, and a timed-out one may still
// finish.
const deviceCreateTimeout = 10 * time.Minute

// device finds or creates the private device for a runtime build: the newest "iPhone N Pro" the runtime supports.
func device(ctl simctl.Ctl, simRuntime simctl.Runtime, say func(string)) (privateDevice, error) {
	name := "stuntcam-canary-" + simRuntime.Build
	find := func() (privateDevice, error) {
		devices, err := ctl.Devices()
		if err != nil {
			return privateDevice{}, err
		}

		for _, listed := range devices {
			if listed.Name == name {
				return privateDevice{udid: listed.UDID, state: listed.State}, nil
			}
		}

		return privateDevice{}, nil
	}

	if found, err := find(); err != nil || found.udid != "" {
		return found, err
	}

	types := proPhonesNewestFirst(simRuntime.Supported)
	if len(types) == 0 {
		return privateDevice{}, fmt.Errorf("iOS %s supports no iPhone Pro device type", simRuntime.Version)
	}

	say(fmt.Sprintf("creating %s in the private set %s (first use of iOS %s: about a minute)", name, home.Devices(),
		simRuntime.Version))
	createCtl := ctl
	createCtl.Timeout = deviceCreateTimeout
	udid, err := createCtl.Create(name, types[0].Identifier, simRuntime.Identifier)
	if err == nil && udid != "" {
		return privateDevice{udid: udid, state: simctl.Shutdown, created: true}, nil
	}

	if found, _ := find(); found.udid != "" { // it finished anyway
		found.created = true
		return found, nil
	}

	// %v, not %w: err is nil when simctl exited 0 without a udid, and %w would print that as %!w(<nil>)
	return privateDevice{}, fmt.Errorf("create %s (%s): %v", name, types[0].Name, err)
}

var proRe = regexp.MustCompile(`^iPhone (\d+) Pro$`)

// proPhonesNewestFirst returns the "iPhone <n> Pro" device types, highest n first. Types with the same n keep simctl's
// order.
func proPhonesNewestFirst(types []simctl.DeviceType) []simctl.DeviceType {
	type numberedType struct {
		deviceType simctl.DeviceType
		number     int // the n in "iPhone <n> Pro"
	}

	var pros []numberedType
	for _, deviceType := range types {
		match := proRe.FindStringSubmatch(deviceType.Name)
		if match == nil {
			continue
		}

		var number int
		fmt.Sscanf(match[1], "%d", &number) // fails only past int's range, leaving 0: no device is numbered that high
		pros = append(pros, numberedType{deviceType, number})
	}

	slices.SortStableFunc(pros, func(a, b numberedType) int { return cmp.Compare(b.number, a.number) })

	var out []simctl.DeviceType
	for _, pro := range pros {
		out = append(out, pro.deviceType)
	}

	return out
}

// PostBootSettle is the earliest the canary is launched after bootstatus returns, on runtimes that need it
// (settleFloor). iOS 27.0 has a post-boot window in which the canary's photos stall the frame pump.
//
// Measured on an idle 27.0, by floor:
//   - 0 s (launch at bootstatus + 2.2 s): 5 of 5 runs missed a photo's 100 ms frame-gap check.
//   - 5 s: 4 of 10 missed frame window A's count, and 5 of 10 missed a photo gap.
//   - 8 s: 0 of 8 missed. The largest gap was 95 ms.
//   - 11 s: 1 of 6 missed, at 3 s more. So the floor is 8 s.
//
// The floor overlaps the SpringBoard guard and any install or grant, so only the remainder is slept. That is about
// 5.8 s on a warm verify.
// 26.3 and 17.5 get none. On 26.3, a launch at bootstatus + 2.2 s gave no misses in 106 warm verifies.
const PostBootSettle = 8 * time.Second

// firstSettledMajor is the first iOS major version that gets PostBootSettle.
const firstSettledMajor = 27

// settleFloor is the post-boot settle for an iOS version: PostBootSettle on 27 and later (and on a version it can't
// read, e.g. a future runtime), 0 before 27. SIMCAM_VERIFY_SETTLE_S=<seconds> overrides it (measurements).
func settleFloor(version string) time.Duration {
	if v := os.Getenv("SIMCAM_VERIFY_SETTLE_S"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil && f >= 0 {
			return time.Duration(f * float64(time.Second))
		}
	}

	major, err := strconv.Atoi(strings.SplitN(version, ".", 2)[0])
	if err == nil && major < firstSettledMajor {
		return 0
	}

	return PostBootSettle
}

// waitSettle sleeps until settle has passed since booted (bootstatus returned); booted zero: no wait.
func waitSettle(booted time.Time, settle time.Duration) {
	if booted.IsZero() || settle <= 0 {
		return
	}
	left := settle - time.Since(booted)
	if left > 0 {
		time.Sleep(left)
	}
	mark("settle floor=%.1f waited=%.1f", settle.Seconds(), max(left, 0).Seconds())
}

// bootInstall boots the device armed, and makes sure this build's canary is installed and granted.
// force reinstalls and re-grants whatever is on the device, as for the reboot after refused launches. Otherwise
// planInstall decides, before the boot.
// It returns no earlier than settle after bootstatus returned (PostBootSettle).
func bootInstall(ctl simctl.Ctl, udid string, set artifacts.Set, force bool, settle time.Duration,
	say func(string)) (installPlan, error) {
	plan := planInstall(filepath.Join(ctl.Set, udid), udid, set.Canary)
	if force {
		plan.Install, plan.Grant, plan.Why = true, true, "reinstalling the canary after refused launches"
	}
	mark("install-plan install=%v grant=%v why=%q", plan.Install, plan.Grant, plan.Why)
	if plan.Install {
		_ = os.Remove(CanaryRecordFile(udid)) // until this install has succeeded
	}
	// a throwaway device: no sentinel history from earlier verifies (two attributed crashes would latch the canary and
	// turn the next verdict into a fail whose checks hide the cause)
	_ = os.RemoveAll(home.StateDir(udid))
	mark("armboot-start")
	timed := timedSim{simSteps: ctl, booted: new(time.Time)}
	if err := arm.ArmBoot(timed, udid, set, arm.Vars(udid, set, arm.Options{StubLog: true})); err != nil {
		return plan, err
	}
	// SpringBoard guard: install and launch only once SpringBoard has kept one pid for 2 s
	guardSpringBoard(ctl, filepath.Join(ctl.Set, udid), udid, springboardPoll)
	if err := installGrant(ctl, udid, plan); err != nil {
		return plan, err
	}
	waitSettle(*timed.booted, settle)
	return plan, nil
}

// redoSkippedInstall reinstalls and re-grants the canary before the second run when the first run never started (no
// CANARY output) on a verify that skipped the install or the grant, so a broken install or a lost grant can't make
// both runs inconclusive. It reports whether it did.
func redoSkippedInstall(ctl simctl.Ctl, udid string, plan *installPlan, ran bool) (bool, error) {
	if ran || (plan.Install && plan.Grant) {
		return false, nil
	}
	plan.Install, plan.Grant = true, true
	_ = os.Remove(CanaryRecordFile(udid)) // until the reinstall has succeeded
	return true, installGrant(ctl, udid, *plan)
}

// realInstallGrant carries out a plan's install and grants and records what is now on the device.
func realInstallGrant(ctl simctl.Ctl, udid string, plan installPlan) error {
	if plan.Install {
		if err := ctl.Install(udid, plan.canary); err != nil {
			return fmt.Errorf("install failed: %w", err)
		}
	}

	mark("install-done skipped=%v", !plan.Install)
	granted := !plan.Grant
	if plan.Grant { // idempotent; it terminates the app if it's running
		cameraErr := ctl.Grant(udid, "camera", CanaryBundle)
		micErr := ctl.Grant(udid, "microphone", CanaryBundle)
		granted = cameraErr == nil && micErr == nil
	}

	mark("grant-done skipped=%v", !plan.Grant)
	if plan.Install || plan.Grant {
		// without a record the next verify only reinstalls
		_ = recordInstall(filepath.Join(ctl.Set, udid), udid, plan.hash, granted)
	}

	return nil
}
