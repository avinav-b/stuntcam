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
	udid            string
	mode            string
	source          string
	build           string
	reboot          bool
	logs            bool
	skipVerify      bool
	allowUnverified bool
	grants          multi
}

func parseUpArgs(args []string) (upOptions, error) {
	var opts upOptions
	flags := flag.NewFlagSet("up", flag.ContinueOnError)
	flags.StringVar(&opts.udid, "udid", "", "simulator UDID")
	flags.StringVar(&opts.mode, "mode", "",
		"boot | launchd (default: boot if the sim is shut down, launchd if it is booted)")
	flags.BoolVar(&opts.reboot, "reboot", false,
		"reboot a booted simulator into boot mode (Xcode Run and tests need it)")
	flags.StringVar(&opts.source, "source", "",
		"scene | image:PATH | video:PATH | qr:TEXT | webcam[:NAME] (default scene)")
	flags.BoolVar(&opts.logs, "log", false, "log stub and engine decisions under the host directory's logs/<udid>/")
	flags.StringVar(&opts.build, "build", "", "use this build dir (loader + engine pair) instead of the default")
	flags.BoolVar(&opts.skipVerify, "skip-verify", false, "arm without a canary verdict")
	flags.BoolVar(&opts.allowUnverified, "allow-unverified", false, "arm even if the canary failed on this runtime")
	flags.Var(&opts.grants, "grant",
		"grant camera + microphone to this bundle id (repeatable; terminates it if running)")
	err := parse(flags, args, 0)
	return opts, err
}

// cmdUp arms a simulator: it stages the build, picks the simulator, refuses an arm that couldn't take effect, gets the
// canary's verdict, and then re-points a sim already boot-armed this boot, or arms it in boot or launchd mode with a
// fresh control file and webcam session.
func cmdUp(args []string) error {
	opts, err := parseUpArgs(args)
	if err != nil {
		return err
	}

	// A bad --source is reported before the canary, which can take a minute. The webcam source is resolved later.
	sourceSpec := ""
	webcamName, useWebcam := webcamSpec(opts.source)
	if opts.source != "" && !useWebcam {
		if sourceSpec, err = stageSource(opts.source); err != nil {
			return usageError{err}
		}
	}

	set, err := stageUpBuild(opts.build, useWebcam)
	if err != nil {
		return err
	}

	device, reason, err := pickSim(opts.udid)
	if err != nil {
		return err
	}

	fmt.Printf("simulator: %s (%s, %s), %s\n", device.Name, device.UDID, runtimeName(device.RuntimeID), reason)

	requestedMode := arm.Mode(opts.mode)
	if err := refuseUnchangeableArm(device, requestedMode, opts.reboot); err != nil {
		return err
	}

	if !opts.skipVerify {
		if err := canaryVerdict(set, device, opts.allowUnverified); err != nil {
			return err
		}
	}

	// A sim another stuntcam is booting is left alone, and one boot-armed since this boot is re-pointed (or, for
	// launchd mode, refused) instead of rebooted, unless --reboot.
	record, recordErr := arm.ReadRecord(device.UDID)
	if recordErr == nil && arm.InProgress(record) && !opts.reboot {
		return fmt.Errorf("%s is being booted armed by another stuntcam (pid %d) right now; try again in a minute, or "+
			"--reboot", device.Name, record.Pid)
	}

	bootArmedThisBoot := recordErr == nil && arm.SameBoot(record, device.State == simctl.Booted, device.LastBooted)
	if bootArmedThisBoot && !opts.reboot {
		switch requestedMode {
		case arm.Launchd:
			// launchd ignores DYLD_INSERT_LIBRARIES once a boot environment set it (setenv returns 0, getenv reads "")
			return usagef("%s is armed in boot mode since this boot, and launchd mode can't be added to it; it is "+
				"already armed (`stuntcam up` re-points it, --reboot re-arms it)", device.Name)
		case "", arm.Boot:
			return rePointBootArmed(device, set, record, opts)
		}
	}

	mode := upMode(requestedMode, opts.reboot, device)
	if mode != arm.Boot && mode != arm.Launchd {
		return fmt.Errorf("--mode must be boot or launchd")
	}

	// The camera is resolved and its helper started (and allowed) before the simulator is touched.
	webcamID := ""
	if useWebcam {
		if webcamID, err = startWebcam(newWebcamEnv(), set, webcamName, "", ""); err != nil {
			return err
		}

		sourceSpec = "webcam:" + webcamID
	}

	if mode, err = armSim(device, set, mode, sourceSpec, opts.logs); err != nil {
		return err
	}

	freshLiveState(device, sourceSpec, webcamID)
	grantAll(device.UDID, opts.grants)
	if mode == arm.Boot {
		noteDisplacedInserts(device)
	}

	printArmed(mode, set.BuildID())
	return nil
}

// stageUpBuild finds the build `up` arms with (--build, else the default) and stages it under the host directory. The
// webcam source needs the build's camera helper.
func stageUpBuild(buildDir string, useWebcam bool) (artifacts.Set, error) {
	set, err := artifacts.Find(buildDir)
	if err != nil {
		return set, err
	}

	if set, err = artifacts.Stage(set); err != nil {
		return set, fmt.Errorf("staging the build under %s: %w", home.Root(), err)
	}

	if useWebcam && set.Helper == "" {
		return set, usagef("--source webcam: this build has no camera helper (in a checkout: helper/build.sh)")
	}

	return set, nil
}

// printArmed says how the sim is now armed and what that reaches.
func printArmed(mode arm.Mode, buildID string) {
	fmt.Printf("armed (%s mode, build %s)", mode, buildID)
	if mode == arm.Boot {
		fmt.Println(" until this simulator shuts down: run `stuntcam up` again after a Mac restart, an Erase or " +
			"quitting the simulators.")
		return
	}
	fmt.Println(": apps launched from now on get the camera, except Xcode Run and Xcode tests (they set their " +
		"own libraries):")
	fmt.Println("  for those, `stuntcam up --reboot`.")
}

// refuseUnchangeableArm refuses, before the canary runs, an `up` that can't take effect: a sim boot-armed with another
// home's or an exec run's directory (unless this `up` reboots it into boot mode), or a boot-mode sim of this boot asked
// for launchd mode.
func refuseUnchangeableArm(device simctl.Device, mode arm.Mode, reboot bool) error {
	if dir := foreignBootArm(device); dir != "" && !willRebootBoot(mode, reboot) {
		// its boot environment names that directory: a launchd-mode arm can't override it and `current` here is never
		// read, so "armed" would be untrue
		return usagef("%s", foreignBootArmMsg(device, dir))
	}

	record, err := arm.ReadRecord(device.UDID)
	bootArmedThisBoot := err == nil && record.Mode == arm.Boot &&
		arm.SameBoot(record, device.State == simctl.Booted, device.LastBooted)
	if bootArmedThisBoot && mode == arm.Launchd {
		// its boot environment already has the stub, and launchctl setenv can't override what was set at boot (on iOS
		// 27.0 the boot values never read back, so every attempt failed as "did not stick")
		return usagef("%s is booted armed in boot mode, which launchd mode can't change: `stuntcam up` keeps boot "+
			"mode; for launchd mode, `xcrun simctl shutdown %s` and then `stuntcam up --mode launchd`", device.Name,
			device.UDID)
	}

	return nil
}

// canaryVerdict gets and prints the canary's verdict for the sim's runtime. The verdict comes first: the canary runs
// on a private device before the user's sim is touched. A failed canary refuses the arm unless allowUnverified; an
// inconclusive one is a warning.
func canaryVerdict(set artifacts.Set, device simctl.Device, allowUnverified bool) error {
	printProgress := func(line string) { fmt.Println(line) }
	result, err := verify.Run(set, verify.Options{Runtime: device.RuntimeID, Progress: printProgress})
	if err != nil {
		return usagef("canary: %w (or --skip-verify)", err)
	}

	origin := "ran the canary"
	if result.Shipped {
		origin = "shipped with this release, " + result.ShippedSource()
	} else if result.Cached {
		origin = "cached"
	}

	fmt.Printf("canary verdict for iOS %s, build %s: %s (%s)\n", result.RuntimeVer, result.EngineBuild, result.Verdict,
		origin)
	if result.RetriedPass() {
		fmt.Println(retriedLine(result))
	}

	switch result.State {
	case verify.Fail:
		if !allowUnverified {
			return fmt.Errorf("not arming: the canary failed on this runtime, so the app would see a broken camera. " +
				"`stuntcam verify --fresh` re-runs it; --allow-unverified arms anyway")
		}
	case verify.Inconclusive:
		fmt.Println("warning: the canary could not run to a verdict (busy Mac, or the simulator would not boot); " +
			"arming anyway. The next `stuntcam up` retries it.")
	}

	return nil
}

// rePointBootArmed handles `up` on a sim already booted armed in boot mode, this boot. Its boot environment has the
// stub, so it needs no reboot. Pointing `current` at this build is all it takes, along with the Xcode record, which
// the loader reads at each launch.
func rePointBootArmed(device simctl.Device, set artifacts.Set, record arm.Record, opts upOptions) error {
	if opts.source != "" || opts.logs {
		return usagef("%s is already booted armed: --source/--log are part of its boot environment; add --reboot to "+
			"change them", device.Name)
	}

	if err := arm.Prepare(device.UDID, set); err != nil {
		return err
	}

	recordXcode(device.UDID, set.Rules, "up", printXcodeSummary, signalsFromLaunchdSim)

	// At stays the armed boot's time: SameBoot compares the sim's boot time with it, so moving it to now would make a
	// reboot within the next 5 s count as this boot (status, the service and source would take an unarmed reboot for
	// armed)
	record.Build = set.BuildID()
	if err := arm.WriteRecord(device.UDID, record); err != nil {
		return err
	}

	// an explicit arm undoes `down` here too: left in place, the down marker would make the login service skip this
	// sim's boots
	if err := os.Remove(arm.DownMarker(device.UDID)); err != nil && !os.IsNotExist(err) {
		return err
	}

	fmt.Printf("already armed in boot mode since this boot; now on build %s from the next app launch\n", set.BuildID())
	grantAll(device.UDID, opts.grants)
	return nil
}

// upMode is the mode `up` arms in: --mode, else boot with --reboot or for a shut-down sim, launchd for a booted one.
func upMode(requested arm.Mode, reboot bool, device simctl.Device) arm.Mode {
	switch {
	case requested != "":
		return requested
	case reboot:
		return arm.Boot
	case device.State == simctl.Booted:
		return arm.Launchd
	default:
		return arm.Boot
	}
}

// armSim records the Xcode and arms the sim in mode (booting or rebooting it for boot mode, booting it unarmed first
// for launchd mode if it is shut down) and returns the mode it ended up in: an armed boot whose SpringBoard doesn't
// come up falls back to launchd mode.
func armSim(device simctl.Device, set artifacts.Set, mode arm.Mode, sourceSpec string, logs bool) (arm.Mode, error) {
	options := arm.Options{StubLog: logs, Extra: map[string]string{}}
	if sourceSpec != "" {
		options.Extra["SIMCAM_SOURCE"] = sourceSpec
	}

	vars := arm.Vars(device.UDID, set, options)

	// before the boot: the first launch reads it
	signals := signalsFromLaunchdSim
	if mode == arm.Boot || device.State != simctl.Booted { // this process boots the sim
		signals = signalsFromThisProcess
	}

	recordXcode(device.UDID, set.Rules, "up", printXcodeSummary, signals)

	switch mode {
	case arm.Boot:
		return armBoot(device, set, vars)
	case arm.Launchd:
		if device.State != simctl.Booted {
			if err := arm.BootUnarmed(ctl, device.UDID, set.BuildID()); err != nil {
				return mode, err
			}
		}

		return mode, arm.ArmLaunchd(ctl, device.UDID, set, vars)
	}

	return mode, nil
}

// armBoot boots (or reboots) the sim armed. An armed boot that doesn't bring SpringBoard up healthy falls back to an
// unarmed boot and launchd mode, and armBoot returns the mode it ended up in.
func armBoot(device simctl.Device, set artifacts.Set, vars []string) (arm.Mode, error) {
	if device.State == simctl.Booted {
		fmt.Println("rebooting it armed (apps running in it will quit)")
	} else {
		fmt.Println("booting it armed")
	}

	if err := arm.ArmBoot(ctl, device.UDID, set, vars); err != nil {
		return arm.Boot, err
	}

	if springboardUp(device.UDID, springboardWait) {
		return arm.Boot, nil
	}

	fmt.Printf("the armed boot did not bring SpringBoard up in %d s: booting it unarmed and arming in "+
		"launchd mode instead\n", int(springboardWait/time.Second))
	_ = ctl.Shutdown(device.UDID) // fails if already shut down; BootUnarmed reports a sim it can't boot
	if err := arm.BootUnarmed(ctl, device.UDID, set.BuildID()); err != nil {
		return arm.Boot, err
	}

	return arm.Launchd, arm.ArmLaunchd(ctl, device.UDID, set, vars)
}

// freshLiveState starts a fresh arm from a fresh control file. Otherwise an earlier `stuntcam source` or `orient`
// would override --source, because the engine applies the file's source= at its first poll, right after reading
// SIMCAM_SOURCE.
// It also sets the sim's webcam session to the camera now selected. With none selected there is no session, and a
// fresh arm starts from the scene.
func freshLiveState(device simctl.Device, sourceSpec, webcamID string) {
	_ = os.Remove(home.ControlFile(device.UDID)) // usually there is none
	if sourceSpec != "" {
		if err := setControl(device, "source", sourceSpec); err != nil {
			fmt.Fprintf(os.Stderr, "warning: %v\n", err)
		}
	}

	var webcamIDs []string
	if webcamID != "" {
		webcamIDs = []string{webcamID}
	}

	webcamEnv := newWebcamEnv()
	if webcamID == "" && len(webcam.ReadSession(webcamEnv.Home, device.UDID)) == 0 {
		return // no camera before or now: no session to change
	}

	if err := webcamEnv.Switch(device.UDID, webcamIDs, webcamInUse()); err != nil {
		fmt.Fprintf(os.Stderr, "warning: %v\n", err)
	}
}

// displacedInsertScanDepth is how many directory levels below the working directory noteDisplacedInserts looks for
// schemes.
const displacedInsertScanDepth = 6

// noteDisplacedInserts names, for a project in the working directory, each scheme that inserts libraries a boot-mode
// launch displaces.
func noteDisplacedInserts(device simctl.Device) {
	wd, err := os.Getwd()
	if err != nil || !isXcodeProjectDir(wd) { // never walk ~ (slow, and privacy prompts)
		return
	}
	for _, insert := range scheme.Scan(wd, displacedInsertScanDepth) {
		fmt.Printf("note: %s inserts %s; boot mode displaces it. To keep it: stuntcam chain add --udid %s <path>\n",
			insert.Scheme, strings.Join(insert.Libs, ", "), device.UDID)
	}
}

// grantAll grants camera and microphone access to each bundle; a failed grant is a warning, not an error.
func grantAll(udid string, bundles []string) {
	for _, bundle := range bundles {
		for _, service := range []string{"camera", "microphone"} {
			if err := ctl.Grant(udid, service, bundle); err != nil {
				fmt.Fprintf(os.Stderr, "warning: grant %s to %s: %v\n", service, bundle, err)
			}
		}
	}
}

// willRebootBoot reports whether this `up` reboots the sim into boot mode, which replaces a boot environment from
// another home (an explicit --mode boot reboots a booted sim; --reboot without --mode means boot). Only then is a sim
// armed from another home fixable here.
func willRebootBoot(mode arm.Mode, reboot bool) bool {
	return mode == arm.Boot || (reboot && mode != arm.Launchd)
}

// foreignBootArm is the sim dir a booted sim's boot environment arms it with, when that is not this home's
// sims/<udid> (another SIMCAM_HOME, or a `stuntcam exec` run directory); "" otherwise.
func foreignBootArm(device simctl.Device) string {
	if device.State != simctl.Booted {
		return ""
	}

	env, err := bootEnv(device.UDID)
	if err != nil {
		return ""
	}

	hasStub, dir := procenv.Armed(env, artifacts.StubName)
	if hasStub && dir != "" && filepath.Clean(dir) != home.Sim(device.UDID) {
		return dir
	}

	return ""
}

func foreignBootArmMsg(device simctl.Device, dir string) string {
	whose := "a `stuntcam exec` run's directory"
	if filepath.Base(filepath.Clean(dir)) == device.UDID {
		whose = "another stuntcam home (SIMCAM_HOME differs from " + home.Root() + ")"
	}
	return fmt.Sprintf("%s is already boot-armed from %s, %s: its boot environment names that directory, so arming it "+
		"here would not take effect. `stuntcam up --reboot --udid %s` re-arms it under this home (apps running in it "+
		"quit); or run stuntcam with the SIMCAM_HOME that armed it", device.Name, whose, dir, device.UDID)
}

// isXcodeProjectDir reports an .xcodeproj or .xcworkspace in dir or one level below (an app repo's root or its ios/
// folder).
func isXcodeProjectDir(dir string) bool {
	for _, pattern := range []string{"*.xcodeproj", "*.xcworkspace", "*/*.xcodeproj", "*/*.xcworkspace"} {
		if matches, _ := filepath.Glob(filepath.Join(dir, pattern)); len(matches) > 0 {
			return true
		}
	}
	return false
}
