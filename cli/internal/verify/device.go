// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
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
	state   string // as simctl lists it ("Shutdown" for a new one)
	created bool   // made by this verify
}

// deviceCreateTimeout bounds a device create: a first create on a busy Mac can be slow, and a timed-out one may still
// finish.
const deviceCreateTimeout = 10 * time.Minute

// device finds or creates the private device for a runtime build: the newest "iPhone N Pro" the runtime supports.
func device(ctl simctl.Ctl, rt simctl.Runtime, say func(string)) (privateDevice, error) {
	name := "stuntcam-canary-" + rt.Build
	find := func() (privateDevice, error) {
		ds, err := ctl.Devices()
		if err != nil {
			return privateDevice{}, err
		}
		for _, d := range ds {
			if d.Name == name {
				return privateDevice{udid: d.UDID, state: d.State}, nil
			}
		}
		return privateDevice{}, nil
	}
	if found, err := find(); err != nil || found.udid != "" {
		return found, err
	}
	types := proPhonesNewestFirst(rt.Supported)
	if len(types) == 0 {
		return privateDevice{}, fmt.Errorf("iOS %s supports no iPhone Pro device type", rt.Version)
	}
	say(fmt.Sprintf("creating %s in the private set %s (first use of iOS %s: about a minute)", name, home.Devices(), rt.Version))
	c := ctl
	c.Timeout = deviceCreateTimeout
	udid, err := c.Create(name, types[0].Identifier, rt.Identifier)
	if err == nil && udid != "" {
		return privateDevice{udid: udid, state: "Shutdown", created: true}, nil
	}
	if found, _ := find(); found.udid != "" { // it finished anyway
		found.created = true
		return found, nil
	}
	return privateDevice{}, fmt.Errorf("create %s (%s): %v", name, types[0].Name, err)
}

var proRe = regexp.MustCompile(`^iPhone (\d+) Pro$`)

func proPhonesNewestFirst(ts []simctl.DeviceType) []simctl.DeviceType {
	var out []simctl.DeviceType
	for _, t := range ts {
		if proRe.MatchString(t.Name) {
			out = append(out, t)
		}
	}
	num := func(t simctl.DeviceType) int {
		var n int
		fmt.Sscanf(proRe.FindStringSubmatch(t.Name)[1], "%d", &n)
		return n
	}
	for i := 0; i < len(out); i++ {
		for j := i + 1; j < len(out); j++ {
			if num(out[j]) > num(out[i]) {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	return out
}

// PostBootSettle is the earliest the canary is launched after bootstatus returns, on runtimes that need it (settleFloor).
// iOS 27.0 has a post-boot window in which the canary's photos stall the frame pump. Measured on 27.0 idle, by floor:
// 0 s (launch at bootstatus + 2.2 s): 5/5 runs missed a photo's 100 ms frame-gap check; 5 s: 4/10 missed frames A's
// count and 5/10 a photo gap; 8 s: 0/8 (largest gap 95 ms); 11 s: 1/6 at +3 s more, so 8 s. The floor overlaps the
// SpringBoard guard and any install or grant: only the remainder is slept (~5.8 s on a warm verify). 26.3 and 17.5 get
// none: on 26.3 a launch at bootstatus + 2.2 s gave 0 misses in 106 warm verifies.
const PostBootSettle = 8 * time.Second

// settleFloor is the post-boot settle for an iOS version: PostBootSettle on 27 and later (and on a version it can't read,
// e.g. a future runtime), 0 before 27. SIMCAM_VERIFY_SETTLE_S=<seconds> overrides it (measurements).
func settleFloor(version string) time.Duration {
	if v := os.Getenv("SIMCAM_VERIFY_SETTLE_S"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil && f >= 0 {
			return time.Duration(f * float64(time.Second))
		}
	}
	major, err := strconv.Atoi(strings.SplitN(version, ".", 2)[0])
	if err == nil && major < 27 {
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

// bootInstall boots the device armed and makes sure this build's canary is installed and granted. force reinstalls and
// re-grants whatever is on the device (the reboot after refused launches); otherwise planInstall decides, before the boot.
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
	// a throwaway device: no sentinel history from earlier verifies (two attributed crashes would latch the canary and turn
	// the next verdict into a fail whose checks hide the cause)
	_ = os.RemoveAll(home.StateDir(udid))
	mark("armboot-start")
	ts := timedSim{simSteps: ctl, booted: new(time.Time)}
	if err := arm.ArmBoot(ts, udid, set, arm.Vars(udid, set, arm.Options{StubLog: true})); err != nil {
		return plan, err
	}
	// SpringBoard guard: install and launch only once SpringBoard has kept one pid for 2 s
	guardSpringBoard(ctl, filepath.Join(ctl.Set, udid), udid, 150*time.Millisecond)
	if err := installGrantFn(ctl, udid, plan); err != nil {
		return plan, err
	}
	waitSettle(*ts.booted, settle)
	return plan, nil
}

// installGrantFn is installGrant; a var so tests can stand in for it.
var installGrantFn = installGrant

// redoSkippedInstall: after a first run that never started (no CANARY output) on a verify that skipped the install or the
// grant, reinstall and re-grant before the second run, so a broken install or a lost grant can't make both runs
// inconclusive. It reports whether it did.
func redoSkippedInstall(ctl simctl.Ctl, udid string, plan *installPlan, ran bool) (bool, error) {
	if ran || (plan.Install && plan.Grant) {
		return false, nil
	}
	plan.Install, plan.Grant = true, true
	_ = os.Remove(CanaryRecordFile(udid))
	return true, installGrantFn(ctl, udid, *plan)
}

// installGrant carries out a plan's install and grants and records what is now on the device.
func installGrant(ctl simctl.Ctl, udid string, plan installPlan) error {
	if plan.Install {
		if err := ctl.Install(udid, plan.canary); err != nil {
			return fmt.Errorf("install failed: %v", err)
		}
	}
	mark("install-done skipped=%v", !plan.Install)
	granted := !plan.Grant
	if plan.Grant { // idempotent; it terminates the app if it's running
		e1 := ctl.Grant(udid, "camera", CanaryBundle)
		e2 := ctl.Grant(udid, "microphone", CanaryBundle)
		granted = e1 == nil && e2 == nil
	}
	mark("grant-done skipped=%v", !plan.Grant)
	if plan.Install || plan.Grant {
		_ = recordInstall(filepath.Join(ctl.Set, udid), udid, plan.hash, granted)
	}
	return nil
}
