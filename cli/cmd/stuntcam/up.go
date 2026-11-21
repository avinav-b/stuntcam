// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/procenv"
	"github.com/avinav-b/stuntcam/cli/internal/scheme"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/verify"
	"github.com/avinav-b/stuntcam/cli/internal/webcam"
)

// upOptions are up's flags.
type upOptions struct {
	udid, mode, source, build                 string
	reboot, logs, skipVerify, allowUnverified bool
	grants                                    multi
}

func parseUpArgs(args []string) (upOptions, error) {
	var o upOptions
	fs := flag.NewFlagSet("up", flag.ContinueOnError)
	fs.StringVar(&o.udid, "udid", "", "simulator UDID")
	fs.StringVar(&o.mode, "mode", "", "boot | launchd (default: boot if the sim is shut down, launchd if it is booted)")
	fs.BoolVar(&o.reboot, "reboot", false, "reboot a booted simulator into boot mode (Xcode Run and tests need it)")
	fs.StringVar(&o.source, "source", "", "scene | image:PATH | video:PATH | webcam[:NAME] (default scene)")
	fs.BoolVar(&o.logs, "log", false, "log stub and engine decisions under the host directory's logs/<udid>/")
	fs.StringVar(&o.build, "build", "", "use this build dir (loader + engine pair) instead of the default")
	fs.BoolVar(&o.skipVerify, "skip-verify", false, "arm without a canary verdict")
	fs.BoolVar(&o.allowUnverified, "allow-unverified", false, "arm even if the canary failed on this runtime")
	fs.Var(&o.grants, "grant", "grant camera + microphone to this bundle id (repeatable; terminates it if running)")
	err := parse(fs, args, 0)
	return o, err
}

// cmdUp arms a simulator: it stages the build, picks the simulator, refuses an arm that couldn't take effect, gets the
// canary's verdict, and then re-points a sim already boot-armed this boot, or arms it in boot or launchd mode with a
// fresh control file and webcam session.
func cmdUp(args []string) error {
	o, err := parseUpArgs(args)
	if err != nil {
		return err
	}
	srcSpec := ""
	wcName, wcOn := webcamSpec(o.source)
	if o.source != "" && !wcOn { // checked before the self-test, which can take a minute
		if srcSpec, err = stageSource(o.source); err != nil {
			return usageError{err}
		}
	}
	set, err := artifacts.Find(o.build)
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return fmt.Errorf("staging the build under %s: %w", home.Root(), err)
	}
	if wcOn && set.Helper == "" {
		return usagef("--source webcam: this build has no camera helper (in a checkout: helper/build.sh)")
	}
	d, why, err := pickSim(o.udid)
	if err != nil {
		return err
	}
	fmt.Printf("simulator: %s (%s, %s), %s\n", d.Name, d.UDID, runtimeName(d.RuntimeID), why)
	if err := refuseUnchangeableArm(d, arm.Mode(o.mode), o.reboot); err != nil {
		return err
	}
	if !o.skipVerify {
		if err := canaryVerdict(set, d, o.allowUnverified); err != nil {
			return err
		}
	}
	m := arm.Mode(o.mode)
	rec, recErr := arm.ReadRecord(d.UDID)
	if recErr == nil && arm.InProgress(rec) && !o.reboot {
		return fmt.Errorf("%s is being booted armed by another stuntcam (pid %d) right now; try again in a minute, or "+
			"--reboot", d.Name, rec.Pid)
	}
	sameBoot := recErr == nil && arm.SameBoot(rec, d.State == "Booted", d.LastBooted)
	if sameBoot && m == arm.Launchd && !o.reboot {
		// launchd ignores DYLD_INSERT_LIBRARIES once a boot environment set it (setenv returns 0, getenv reads "")
		return usagef("%s is armed in boot mode since this boot, and launchd mode can't be added to it; it is already armed "+
			"(`stuntcam up` re-points it, --reboot re-arms it)", d.Name)
	}
	if sameBoot && !o.reboot && (m == "" || m == arm.Boot) {
		return rePointBootArmed(d, set, rec, o)
	}
	m = upMode(m, o.reboot, d)
	if m != arm.Boot && m != arm.Launchd {
		return fmt.Errorf("--mode must be boot or launchd")
	}
	wcID := ""
	if wcOn { // the camera is resolved and its helper started (and allowed) before the simulator is touched
		we := newWebcamEnv()
		if wcID, err = startWebcam(we, set, wcName, "", ""); err != nil {
			return err
		}
		srcSpec = "webcam:" + wcID
	}
	if m, err = armSim(d, set, m, srcSpec, o.logs); err != nil {
		return err
	}
	freshLiveState(d, srcSpec, wcID)
	grantAll(d.UDID, o.grants)
	if m == arm.Boot {
		noteDisplacedInserts(d)
	}
	fmt.Printf("armed (%s mode, build %s)", m, set.BuildID())
	if m == arm.Boot {
		fmt.Println(" until this simulator shuts down: run `stuntcam up` again after a Mac restart, an Erase or quitting " +
			"the simulators.")
	} else {
		fmt.Println(": apps launched from now on get the camera, except Xcode Run and Xcode tests (they set their own " +
			"libraries):")
		fmt.Println("  for those, `stuntcam up --reboot`.")
	}
	return nil
}

// refuseUnchangeableArm refuses, before the canary runs, an `up` that can't take effect: a sim boot-armed with another
// home's or an exec run's directory (unless this `up` reboots it into boot mode), or a boot-mode sim of this boot asked
// for launchd mode.
func refuseUnchangeableArm(d simctl.Device, mode arm.Mode, reboot bool) error {
	if dir := foreignBootArm(d); dir != "" && !willRebootBoot(mode, reboot) {
		// its boot environment names that directory: a launchd-mode arm can't override it and `current` here is never read,
		// so "armed" would be untrue
		return usagef("%s", foreignBootArmMsg(d, dir))
	}
	if r, err := arm.ReadRecord(d.UDID); err == nil && r.Mode == arm.Boot && mode == arm.Launchd &&
		arm.SameBoot(r, d.State == "Booted", d.LastBooted) {
		// its boot environment already has the stub, and launchctl setenv can't override what was set at boot (on iOS 27.0
		// the boot values never read back, so every attempt failed as "did not stick")
		return usagef("%s is booted armed in boot mode, which launchd mode can't change: `stuntcam up` keeps boot mode; "+
			"for launchd mode, `xcrun simctl shutdown %s` and then `stuntcam up --mode launchd`", d.Name, d.UDID)
	}
	return nil
}

// canaryVerdict gets and prints the canary's verdict for the sim's runtime: verdict first (design §4.5 step 4), the
// canary runs on a private device before the user's sim is touched. A failed canary refuses the arm unless
// allowUnverified; an inconclusive one is a warning.
func canaryVerdict(set artifacts.Set, d simctl.Device, allowUnverified bool) error {
	v, err := verify.Run(set, verify.Options{Runtime: d.RuntimeID, Progress: func(s string) { fmt.Println(s) }})
	if err != nil {
		return usagef("canary: %w (or --skip-verify)", err)
	}
	src := "ran the canary"
	if v.Shipped {
		src = "shipped with this release, " + v.ShippedSource()
	} else if v.Cached {
		src = "cached"
	}
	fmt.Printf("canary verdict for iOS %s, build %s: %s (%s)\n", v.RuntimeVer, v.EngineBuild, v.Verdict, src)
	if v.RetriedPass() {
		fmt.Println(retriedLine(v))
	}
	switch v.State {
	case verify.Fail:
		if !allowUnverified {
			return fmt.Errorf("not arming: the canary failed on this runtime, so the app would see a broken camera. " +
				"`stuntcam verify --fresh` re-runs it; --allow-unverified arms anyway")
		}
	case verify.Inconclusive:
		fmt.Println("warning: the canary could not run to a verdict (busy Mac, or the simulator would not boot); arming " +
			"anyway. The next `stuntcam up` retries it.")
	}
	return nil
}

// rePointBootArmed handles `up` on a sim already booted armed (boot mode, this boot): its boot environment has the
// stub, so pointing `current` at this build is all it takes (no reboot); the Xcode record too (the loader reads it at
// each launch).
func rePointBootArmed(d simctl.Device, set artifacts.Set, rec arm.Record, o upOptions) error {
	if o.source != "" || o.logs {
		return usagef("%s is already booted armed: --source/--log are part of its boot environment; add --reboot to "+
			"change them", d.Name)
	}
	if err := arm.Prepare(d.UDID, set); err != nil {
		return err
	}
	recordXcode(d.UDID, set.Rules, "up", true, false)
	// At stays the armed boot's time: SameBoot compares the sim's boot time with it, so moving it to now would make a
	// reboot within the next 5 s count as this boot (status, the service and source would take an unarmed reboot for
	// armed)
	rec.Build = set.BuildID()
	if err := arm.WriteRecord(d.UDID, rec); err != nil {
		return err
	}
	// an explicit arm undoes `down` here too: left in place, the down marker would make the login service skip this
	// sim's boots
	if err := os.Remove(arm.DownMarker(d.UDID)); err != nil && !os.IsNotExist(err) {
		return err
	}
	fmt.Printf("already armed in boot mode since this boot; now on build %s from the next app launch\n", set.BuildID())
	grantAll(d.UDID, o.grants)
	return nil
}

// upMode is the mode `up` arms in: --mode, else boot with --reboot or for a shut-down sim, launchd for a booted one.
func upMode(m arm.Mode, reboot bool, d simctl.Device) arm.Mode {
	switch {
	case m == "" && reboot:
		return arm.Boot
	case m == "" && d.State == "Booted":
		return arm.Launchd
	case m == "":
		return arm.Boot
	}
	return m
}

// armSim records the Xcode and arms the sim in mode m (booting or rebooting it for boot mode, booting it unarmed
// first for launchd mode if it is shut down) and returns the mode it ended up in: an armed boot whose SpringBoard
// doesn't come up falls back to launchd mode.
func armSim(d simctl.Device, set artifacts.Set, m arm.Mode, srcSpec string, logs bool) (arm.Mode, error) {
	o := arm.Options{StubLog: logs, Extra: map[string]string{}}
	if srcSpec != "" {
		o.Extra["SIMCAM_SOURCE"] = srcSpec
	}
	vars := arm.Vars(d.UDID, set, o)
	// before the boot: the first launch reads it
	recordXcode(d.UDID, set.Rules, "up", true, m == arm.Boot || d.State != "Booted")
	var err error
	switch m {
	case arm.Boot:
		if d.State == "Booted" {
			fmt.Println("rebooting it armed (apps running in it will quit)")
		} else {
			fmt.Println("booting it armed")
		}
		err = arm.ArmBoot(ctl, d.UDID, set, vars)
		if err == nil && !springboardUp(d.UDID, springboardWait) {
			// design B-MF6: an armed boot that doesn't come up healthy falls back to an unarmed boot + launchd mode
			fmt.Printf("the armed boot did not bring SpringBoard up in %d s: booting it unarmed and arming in launchd mode "+
				"instead\n", int(springboardWait/time.Second))
			_ = ctl.Shutdown(d.UDID)
			if err = arm.BootUnarmed(ctl, d.UDID, set.BuildID()); err == nil {
				m = arm.Launchd
				err = arm.ArmLaunchd(ctl, d.UDID, set, vars)
			}
		}
	case arm.Launchd:
		if d.State != "Booted" {
			if err := arm.BootUnarmed(ctl, d.UDID, set.BuildID()); err != nil {
				return m, err
			}
		}
		err = arm.ArmLaunchd(ctl, d.UDID, set, vars)
	}
	return m, err
}

// freshLiveState starts a fresh arm from a fresh control file: an earlier `stuntcam source` / `orient` would otherwise
// override --source (the engine applies the file's source= at its first poll, right after reading SIMCAM_SOURCE). It
// also sets the sim's webcam session: the camera now selected, or none (a fresh arm starts from the scene).
func freshLiveState(d simctl.Device, srcSpec, wcID string) {
	_ = os.Remove(home.ControlFile(d.UDID))
	if srcSpec != "" {
		if err := setControl(d, "source", srcSpec); err != nil {
			fmt.Fprintf(os.Stderr, "warning: %v\n", err)
		}
	}
	var wcIDs []string
	if wcID != "" {
		wcIDs = []string{wcID}
	}
	if we := newWebcamEnv(); wcID != "" || len(webcam.ReadSession(we.Home, d.UDID)) > 0 {
		if err := we.Switch(d.UDID, wcIDs, webcamInUse()); err != nil {
			fmt.Fprintf(os.Stderr, "warning: %v\n", err)
		}
	}
}

// noteDisplacedInserts names, for a project in the working directory, each scheme that inserts libraries a boot-mode
// launch displaces.
func noteDisplacedInserts(d simctl.Device) {
	if wd, err := os.Getwd(); err == nil && isXcodeProjectDir(wd) { // never walk ~ (slow, and privacy prompts)
		for _, in := range scheme.Scan(wd, 6) {
			fmt.Printf("note: %s inserts %s; boot mode displaces it. To keep it: stuntcam chain add --udid %s <path>\n",
				in.Scheme, strings.Join(in.Libs, ", "), d.UDID)
		}
	}
}

// grantAll grants camera and microphone access to each bundle; a failed grant is a warning, not an error.
func grantAll(udid string, bundles []string) {
	for _, b := range bundles {
		for _, svc := range []string{"camera", "microphone"} {
			if err := ctl.Grant(udid, svc, b); err != nil {
				fmt.Fprintf(os.Stderr, "warning: grant %s to %s: %v\n", svc, b, err)
			}
		}
	}
}

// willRebootBoot: this `up` reboots the sim into boot mode, which replaces a boot environment from another home (an
// explicit --mode boot reboots a booted sim; --reboot without --mode means boot). Only then is a sim armed from another
// home fixable here.
func willRebootBoot(m arm.Mode, reboot bool) bool {
	return m == arm.Boot || (reboot && m != arm.Launchd)
}

// foreignBootArm: the sim dir a booted sim's boot environment arms it with, when that is not this home's sims/<udid>
// (another SIMCAM_HOME, or a `stuntcam exec` run directory); "" otherwise.
func foreignBootArm(d simctl.Device) string {
	if d.State != "Booted" {
		return ""
	}
	env, err := bootEnv(d.UDID)
	if err != nil {
		return ""
	}
	if ok, dir := procenv.Armed(env, artifacts.StubName); ok && dir != "" && filepath.Clean(dir) != home.Sim(d.UDID) {
		return dir
	}
	return ""
}

func foreignBootArmMsg(d simctl.Device, dir string) string {
	whose := "a `stuntcam exec` run's directory"
	if filepath.Base(filepath.Clean(dir)) == d.UDID {
		whose = "another stuntcam home (SIMCAM_HOME differs from " + home.Root() + ")"
	}
	return fmt.Sprintf("%s is already boot-armed from %s, %s: its boot environment names that directory, so arming it "+
		"here would not take effect. `stuntcam up --reboot --udid %s` re-arms it under this home (apps running in it "+
		"quit); or run stuntcam with the SIMCAM_HOME that armed it", d.Name, whose, dir, d.UDID)
}

// isXcodeProjectDir: an .xcodeproj or .xcworkspace in dir or one level below (an app repo's root or its ios/ folder).
func isXcodeProjectDir(dir string) bool {
	for _, g := range []string{"*.xcodeproj", "*.xcworkspace", "*/*.xcodeproj", "*/*.xcworkspace"} {
		if m, _ := filepath.Glob(filepath.Join(dir, g)); len(m) > 0 {
			return true
		}
	}
	return false
}
