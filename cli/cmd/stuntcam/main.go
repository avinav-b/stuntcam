// SPDX-License-Identifier: Apache-2.0

// Command stuntcam gives the iOS Simulator a camera for your own app's camera code (design §4.5).
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/procenv"
	"github.com/avinav-b/stuntcam/cli/internal/scheme"
	"github.com/avinav-b/stuntcam/cli/internal/service"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/state"
	"github.com/avinav-b/stuntcam/cli/internal/trace"
	"github.com/avinav-b/stuntcam/cli/internal/verify"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// version and commit are set by cli/build.sh: -ldflags "-X main.version=<VERSION file> -X main.commit=<git sha>"
// (scripts/package.sh builds the packaged binary the same way, so `stuntcam version` names the package version)
var (
	version = "0.0.0-dev"
	commit  = ""
)

func cmdVersion(args []string) error {
	fs := flag.NewFlagSet("version", flag.ContinueOnError)
	check := fs.Bool("check", false, "verify every installed file against the package manifest and unpack the payload (dylibs, canary) into the host directory")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	fmt.Println(versionLine())
	if !*check {
		return nil
	}
	// the binary's own package, whatever the working directory (a checkout), --build or STUNTCAM_BUILD say
	set, err := artifacts.InstalledHere()
	if err != nil {
		return err
	}
	fmt.Printf("package %s: every file matches %s\n", set.Version, filepath.Join(set.From, artifacts.ManifestName))
	if ok, h := artifacts.Anchored(); ok {
		fmt.Printf("manifest.txt and payload.tar are the ones this binary was packaged with (manifest sha256 %.12s)\n", h)
	} else {
		fmt.Println("this binary carries no package hashes (a dev build): manifest.txt is checked for consistency only")
	}
	for _, p := range []string{set.Engine, set.Loader, set.Stub, set.Hook, set.Canary} {
		if p != "" {
			fmt.Println("unpacked " + p)
		}
	}
	return nil
}

func versionLine() string {
	if commit == "" {
		return "stuntcam " + version
	}
	return "stuntcam " + version + " (commit " + commit + ")"
}

type command struct {
	name, args, help string
	run              func(args []string) error
}

var commands []command

func init() {
	commands = []command{
		{"up", "[--udid U] [--mode boot|launchd] [--reboot] [--grant BUNDLE]... [--source S] [--log] [--build DIR] [--skip-verify] [--allow-unverified]", "arm a simulator (boots it armed if it is shut down)", cmdUp},
		{"verify", "[--runtime V] [--fresh] [--keep] [--json] [--build DIR]", "canary verdict for this build on a runtime (private device set, cached)", cmdVerify},
		{"exec", "[--build DIR] -- COMMAND [ARGS…]", "run a command (xcodebuild test, a CI script) so simulators it boots come up armed", cmdExec},
		{"service", "install [--preboot U] [--only U]…|uninstall|status|run [--only U]…", "notify-only helper: arms simulators Xcode/Simulator boot (launchd mode) and says when Xcode Run needs `up`", cmdService},
		{"gc", "[--dry-run]", "remove private canary devices for removed runtimes, unused builds and unpacked canaries, and state of deleted simulators", cmdGC},
		{"down", "[--udid U]", "stop arming new launches (no reboot needed)", cmdDown},
		{"status", "[--json]", "every simulator stuntcam knows about: mode, build, latches", cmdStatus},
		{"swap", "[--udid U] [LOADER]", "point an armed simulator at another build, effective at the next launch", cmdSwap},
		{"chain", "add|rm|ls [--udid U] [LIB]", "extra libraries every app launch loads (e.g. another tool's loader)", cmdChain},
		{"sentinel", "ls|clear [--udid U] [--bundle B]", "crash/hang records and latches per app", cmdSentinel},
		{"trace", "[--json] [--udid U] [--pid N] BUNDLE", "an app's last 64 camera ops with host-clock times (shutter -> slot 12 -> still complete -> timestamp)", cmdTrace},
		{"env", "[--udid U]", "print SIMCTL_CHILD_* exports for per-launch arming (Detox, scripts)", cmdEnv},
		{"source", "[--udid U] scene|image:PATH|video:PATH", "switch the picture live, in running apps and the next ones", cmdSource},
		{"orient", "[--udid U] portrait|upsideDown|landscapeLeft|landscapeRight|auto", "set the virtual device orientation, live", cmdOrient},
		{"doctor", "[--json]", "check Xcode, runtimes, the host directory, the build and each simulator", cmdDoctor},
		{"version", "[--check]", "print the version and the commit it was built from (--check: verify the installed files and unpack the payload)", cmdVersion},
	}
}

func usage() {
	fmt.Fprint(os.Stderr, "usage: stuntcam <command> [options]\n\n")
	for _, c := range commands {
		fmt.Fprintf(os.Stderr, "  %-9s %s\n            %s\n", c.name, c.help, c.args)
	}
}

// usageError exits 64 (EX_USAGE), like the scripts: wrong arguments or a setup problem, never a verdict.
type usageError struct{ error }

func (u usageError) Unwrap() error { return u.error } // so errors.Is(err, flag.ErrHelp) sees through it (`stuntcam up -h`)

func usagef(format string, a ...any) error { return usageError{fmt.Errorf(format, a...)} }

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(64)
	}
	if os.Args[1] == "-h" || os.Args[1] == "--help" || os.Args[1] == "help" {
		usage()
		return
	}
	if os.Args[1] == verify.ShutdownCommand { // not listed: the detached child a verify hands its device's shutdown to
		os.Exit(cmdPrivateShutdown(os.Args[2:]))
	}
	for _, c := range commands {
		if c.name == os.Args[1] {
			if err := c.run(os.Args[2:]); err != nil {
				if errors.Is(err, flag.ErrHelp) {
					return
				}
				fmt.Fprintln(os.Stderr, "stuntcam:", err)
				var u usageError
				if errors.Is(err, flag.ErrHelp) { // `stuntcam up -h`: the flag package printed the options
					return
				}
				if errors.As(err, &u) {
					os.Exit(64)
				}
				os.Exit(1)
			}
			return
		}
	}
	fmt.Fprintf(os.Stderr, "stuntcam: unknown command %q\n", os.Args[1])
	usage()
	os.Exit(64)
}

// parse parses flags and allows at most maxPos positional arguments, all after the flags: Go's flag package stops at the first
// non-flag, so `stuntcam swap LOADER --udid X` would silently drop --udid and act on another simulator.
func parse(fs *flag.FlagSet, args []string, maxPos int) error {
	if err := fs.Parse(args); err != nil {
		return usageError{err}
	}
	for _, a := range fs.Args() {
		if strings.HasPrefix(a, "-") {
			return usagef("%s: options go before the arguments (%q)", fs.Name(), a)
		}
	}
	if fs.NArg() > maxPos {
		return usagef("%s: unexpected argument %q", fs.Name(), fs.Arg(maxPos))
	}
	return nil
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

var ctl = simctl.Ctl{}

// pickSim resolves the target: --udid, else the only booted simulator, else the most recently booted available one.
func pickSim(udid string) (simctl.Device, string, error) {
	ds, err := ctl.Devices()
	if err != nil {
		return simctl.Device{}, "", err
	}
	if udid != "" {
		for _, d := range ds {
			if d.UDID == udid {
				return d, "given with --udid", nil
			}
		}
		return simctl.Device{}, "", fmt.Errorf("no simulator %s (xcrun simctl list devices)", udid)
	}
	var booted []simctl.Device
	for _, d := range ds {
		if d.State == "Booted" && strings.Contains(d.RuntimeID, "iOS") {
			booted = append(booted, d)
		}
	}
	switch {
	case len(booted) == 1:
		return booted[0], "the only booted iOS simulator", nil
	case len(booted) > 1:
		var n []string
		for _, d := range booted {
			n = append(n, fmt.Sprintf("%s (%s)", d.Name, d.UDID))
		}
		return simctl.Device{}, "", fmt.Errorf("%d simulators are booted; pick one with --udid: %s", len(booted), strings.Join(n, ", "))
	}
	var best simctl.Device
	for _, d := range ds {
		if d.Available && strings.Contains(d.RuntimeID, "iOS") && d.LastBooted.After(best.LastBooted) {
			best = d
		}
	}
	if best.UDID == "" {
		return best, "", errors.New("no booted simulator and none booted before; pass --udid (xcrun simctl list devices)")
	}
	return best, "the most recently booted iOS simulator", nil
}

func runtimeName(id string) string {
	s := strings.TrimPrefix(id, "com.apple.CoreSimulator.SimRuntime.")
	return strings.Replace(strings.Replace(s, "-", " ", 1), "-", ".", -1)
}

func cmdUp(args []string) error {
	fs := flag.NewFlagSet("up", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	mode := fs.String("mode", "", "boot | launchd (default: boot if the sim is shut down, launchd if it is booted)")
	reboot := fs.Bool("reboot", false, "reboot a booted simulator into boot mode (Xcode Run and tests need it)")
	source := fs.String("source", "", "scene | image:PATH | video:PATH (default scene)")
	logs := fs.Bool("log", false, "log stub and engine decisions under the host directory's logs/<udid>/")
	build := fs.String("build", "", "use this build dir (loader + engine pair) instead of the default")
	skipVerify := fs.Bool("skip-verify", false, "arm without a canary verdict")
	allowUnverified := fs.Bool("allow-unverified", false, "arm even if the canary failed on this runtime")
	var grants multi
	fs.Var(&grants, "grant", "grant camera + microphone to this bundle id (repeatable; terminates it if running)")
	if err := parse(fs, args, 0); err != nil {
		return err
	}

	srcSpec := ""
	if *source != "" { // checked before the self-test, which can take a minute
		var err error
		if srcSpec, err = stageSource(*source); err != nil {
			return usageError{err}
		}
	}
	set, err := artifacts.Find(*build)
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return fmt.Errorf("staging the build under %s: %w", home.Root(), err)
	}
	d, why, err := pickSim(*udid)
	if err != nil {
		return err
	}
	fmt.Printf("simulator: %s (%s, %s), %s\n", d.Name, d.UDID, runtimeName(d.RuntimeID), why)
	if dir := foreignBootArm(d); dir != "" && !willRebootBoot(arm.Mode(*mode), *reboot) {
		// its boot environment names that directory: a launchd-mode arm can't override it and `current` here is never read,
		// so "armed" would be untrue
		return usagef("%s", foreignBootArmMsg(d, dir))
	}
	if r, err := arm.ReadRecord(d.UDID); err == nil && r.Mode == arm.Boot && arm.Mode(*mode) == arm.Launchd &&
		arm.SameBoot(r, d.State == "Booted", d.LastBooted) {
		// refused before the canary runs: its boot environment already has the stub, and launchctl setenv can't override what
		// was set at boot (on iOS 27.0 the boot values never read back, so every attempt failed as "did not stick")
		return usagef("%s is booted armed in boot mode, which launchd mode can't change: `stuntcam up` keeps boot mode; for launchd mode, `xcrun simctl shutdown %s` and then `stuntcam up --mode launchd`", d.Name, d.UDID)
	}
	if !*skipVerify {
		// verdict first (design §4.5 step 4): the canary runs on a private device, before the user's sim is touched
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
			if !*allowUnverified {
				return fmt.Errorf("not arming: the canary failed on this runtime, so the app would see a broken camera. `stuntcam verify --fresh` re-runs it; --allow-unverified arms anyway")
			}
		case verify.Inconclusive:
			fmt.Println("warning: the canary could not run to a verdict (busy Mac, or the simulator would not boot); arming anyway. The next `stuntcam up` retries it.")
		}
	}
	m := arm.Mode(*mode)
	rec, recErr := arm.ReadRecord(d.UDID)
	if recErr == nil && arm.InProgress(rec) && !*reboot {
		return fmt.Errorf("%s is being booted armed by another stuntcam (pid %d) right now; try again in a minute, or --reboot", d.Name, rec.Pid)
	}
	sameBoot := recErr == nil && arm.SameBoot(rec, d.State == "Booted", d.LastBooted)
	if sameBoot && m == arm.Launchd && !*reboot {
		// launchd ignores DYLD_INSERT_LIBRARIES once a boot environment set it (setenv returns 0, getenv reads "")
		return usagef("%s is armed in boot mode since this boot, and launchd mode can't be added to it; it is already armed (`stuntcam up` re-points it, --reboot re-arms it)", d.Name)
	}
	if sameBoot && !*reboot && (m == "" || m == arm.Boot) {
		if *source != "" || *logs {
			return usagef("%s is already booted armed: --source/--log are part of its boot environment; add --reboot to change them", d.Name)
		}
		// already booted armed (boot mode, this boot): its boot environment has the stub, so pointing `current` at this
		// build is all it takes (no reboot); the Xcode record too (the loader reads it at each launch)
		if err := arm.Prepare(d.UDID, set); err != nil {
			return err
		}
		recordXcode(d.UDID, set.Rules, "up", true, false)
		// At stays the armed boot's time: SameBoot compares the sim's boot time with it, and moving it to now made a reboot
		// within the next 5 s count as this boot: status, the service and source then took an unarmed reboot for armed
		rec.Build = set.BuildID()
		if err := arm.WriteRecord(d.UDID, rec); err != nil {
			return err
		}
		// an explicit arm undoes `down` here too: left in place, the login service skipped this sim's later boots
		if err := os.Remove(arm.DownMarker(d.UDID)); err != nil && !os.IsNotExist(err) {
			return err
		}
		fmt.Printf("already armed in boot mode since this boot; now on build %s from the next app launch\n", set.BuildID())
		return grantAll(d.UDID, grants)
	}
	switch {
	case m == "" && *reboot:
		m = arm.Boot
	case m == "" && d.State == "Booted":
		m = arm.Launchd
	case m == "":
		m = arm.Boot
	}
	if m != arm.Boot && m != arm.Launchd {
		return fmt.Errorf("--mode must be boot or launchd")
	}
	o := arm.Options{StubLog: *logs, Extra: map[string]string{}}
	if srcSpec != "" {
		o.Extra["SIMCAM_SOURCE"] = srcSpec
	}
	vars := arm.Vars(d.UDID, set, o)
	recordXcode(d.UDID, set.Rules, "up", true, m == arm.Boot || d.State != "Booted") // before the boot: the first launch reads it
	switch m {
	case arm.Boot:
		if d.State == "Booted" {
			fmt.Println("rebooting it armed (apps running in it will quit)")
		} else {
			fmt.Println("booting it armed")
		}
		err = arm.ArmBoot(ctl, d.UDID, set, vars)
		if err == nil && !springboardUp(d.UDID, 60*time.Second) {
			// design B-MF6: an armed boot that doesn't come up healthy falls back to an unarmed boot + launchd mode
			fmt.Println("the armed boot did not bring SpringBoard up in 60 s: booting it unarmed and arming in launchd mode instead")
			_ = ctl.Shutdown(d.UDID)
			if err = arm.BootUnarmed(ctl, d.UDID, set.BuildID()); err == nil {
				m = arm.Launchd
				err = arm.ArmLaunchd(ctl, d.UDID, set, vars)
			}
		}
	case arm.Launchd:
		if d.State != "Booted" {
			if err := arm.BootUnarmed(ctl, d.UDID, set.BuildID()); err != nil {
				return err
			}
		}
		err = arm.ArmLaunchd(ctl, d.UDID, set, vars)
	}
	if err != nil {
		return err
	}
	// a fresh arm starts from a fresh control file: an earlier `stuntcam source` / `orient` would otherwise override
	// --source (the engine applies the file's source= at its first poll, right after reading SIMCAM_SOURCE)
	_ = os.Remove(home.ControlFile(d.UDID))
	if srcSpec != "" {
		if err := setControl(d, "source", srcSpec); err != nil {
			fmt.Fprintf(os.Stderr, "warning: %v\n", err)
		}
	}
	if err := grantAll(d.UDID, grants); err != nil {
		return err
	}
	if m == arm.Boot {
		if wd, err := os.Getwd(); err == nil && isXcodeProjectDir(wd) { // never walk ~ (slow, and privacy prompts)
			for _, in := range scheme.Scan(wd, 6) {
				fmt.Printf("note: %s inserts %s; boot mode displaces it. To keep it: stuntcam chain add --udid %s <path>\n", in.Scheme, strings.Join(in.Libs, ", "), d.UDID)
			}
		}
	}
	fmt.Printf("armed (%s mode, build %s)", m, set.BuildID())
	if m == arm.Boot {
		fmt.Println(" until this simulator shuts down: run `stuntcam up` again after a Mac restart, an Erase or quitting the simulators.")
	} else {
		fmt.Println(": apps launched from now on get the camera, except Xcode Run and Xcode tests (they set their own libraries):")
		fmt.Println("  for those, `stuntcam up --reboot`.")
	}
	return nil
}

func grantAll(udid string, bundles []string) error {
	for _, b := range bundles {
		for _, svc := range []string{"camera", "microphone"} {
			if err := ctl.Grant(udid, svc, b); err != nil {
				fmt.Fprintf(os.Stderr, "warning: grant %s to %s: %v\n", svc, b, err)
			}
		}
	}
	return nil
}

// willRebootBoot: this `up` reboots the sim into boot mode, which replaces a boot environment from another home (an explicit
// --mode boot reboots a booted sim; --reboot without --mode means boot). Only then is a sim armed from another home fixable
// here.
func willRebootBoot(m arm.Mode, reboot bool) bool {
	return m == arm.Boot || (reboot && m != arm.Launchd)
}

// recordXcode writes sims/<udid>/config.json: the Xcode active now (DEVELOPER_DIR, else xcode-select), whose restore
// rules the loader applies (design §4.1 "Which Xcode's libs"), from rulesDir (the per-content dir artifacts.Stage made).
// Under ⌘R nothing in the app names the launching Xcode, so this record is the loader's only source. If the Xcode can't be
// identified, an existing record is kept. booting: stuntcam boots the sim with this process's environment, so
// its SIMCTL_CHILD_ signal variables become boot signals; otherwise they are read from the booted sim's launchd_sim. With
// say, prints which Xcode and whether its rules are verified.
func recordXcode(udid, rulesDir, source string, say, booting bool) xcode.Info {
	x, err := xcodeActive()
	if err != nil {
		if old, rerr := arm.ReadConfig(udid); rerr == nil && old.Build != "" {
			// keep only the Xcode: the rules dir and boot signals are this arm's
			fmt.Fprintf(os.Stderr, "warning: could not identify the Xcode in use (%v); keeping the Xcode recorded earlier, %s (%s)\n", err, old.Build, old.DeveloperDir)
			x = old.Info
		} else {
			fmt.Fprintf(os.Stderr, "warning: could not identify the Xcode in use (%v); apps will use the newest restore rules and say so\n", err)
		}
	}
	if err := arm.WriteConfig(udid, x, rulesDir, source, bootSignals(udid, booting)); err != nil {
		fmt.Fprintf(os.Stderr, "warning: recording the Xcode for %s: %v\n", udid, err)
	}
	if say {
		f, ok := "", false
		if rulesDir != "" {
			f, ok = xcode.Rules(rulesDir, x.Build)
		}
		switch {
		case ok:
			fmt.Printf("Xcode %s (%s, %s): restore rules verified\n", x.Version, x.Build, x.DeveloperDir)
		case f != "":
			fmt.Printf("Xcode %s (%s, %s): %s (best guess: %s)\n", x.Version, x.Build, x.DeveloperDir, xcode.UnverifiedLine(x.Build), strings.TrimSuffix(filepath.Base(f), ".json"))
		default:
			fmt.Printf("Xcode %s (%s): %s (no usable rules staged%s: the loader's built-in 27A266a rules)\n", x.Version, x.Build, xcode.UnverifiedLine(x.Build), prefix(" in ", rulesDir))
		}
	}
	return x
}

// bootSignals: the displaced-insert signal variables in a sim's boot environment (config.json boot_signals). booting: the
// SIMCTL_CHILD_ ones of this process (simctl boot passes them on); else those of its launchd_sim, if booted.
func bootSignals(udid string, booting bool) []string {
	if booting {
		return arm.ChildSignals(os.Environ())
	}
	env, err := bootEnv(udid)
	if err != nil {
		return nil
	}
	var l []string
	for k, v := range env {
		l = append(l, k+"="+v)
	}
	return arm.Signals(l)
}

// foreignBootArm: the sim dir a booted sim's boot environment arms it with, when that is not this home's sims/<udid> (another
// SIMCAM_HOME, or a `stuntcam exec` run directory); "" otherwise.
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
	return fmt.Sprintf("%s is already boot-armed from %s, %s: its boot environment names that directory, so arming it here would not take effect. "+
		"`stuntcam up --reboot --udid %s` re-arms it under this home (apps running in it quit); or run stuntcam with the SIMCAM_HOME that armed it", d.Name, whose, dir, d.UDID)
}

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
	r, err := verify.Run(set, verify.Options{Runtime: *rt, Fresh: *fresh, Keep: *keep, Progress: func(s string) { fmt.Fprintln(os.Stderr, s) }})
	if err != nil {
		return usageError{err} // could not start: not a verdict (exit 64, as scripts/verify.sh)
	}
	if *asJSON {
		e := json.NewEncoder(os.Stdout)
		e.SetIndent("", " ")
		e.Encode(r)
	} else {
		src := fmt.Sprintf("device %s: create %ds, boot %ds, canary %ds", r.Device, r.CreateSeconds, r.BootSeconds, r.CanarySeconds)
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
		os.Exit(1)
	case verify.Inconclusive:
		os.Exit(2)
	}
	return nil
}

// retriedLine is the one line `up` and `verify` print for a retried pass.
func retriedLine(r verify.Result) string {
	what := "the canary's retry of a photo's frame-gap check (" + r.Retried + ")"
	switch {
	case r.FailedFirst != "": // one launch failed on content, the launch run to confirm it passed
		what = "a second canary launch after the first FAILED (" + r.FailedFirst + "); an intermittent engine bug looks like this too"
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
		return 64
	}
	if err := verify.FinishShutdown(*udid, *lock, *parent); err != nil {
		fmt.Println("error:", err)
		return 1
	}
	return 0
}

func cmdDown(args []string) error {
	fs := flag.NewFlagSet("down", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if err := arm.Disarm(ctl, d.UDID, d.State == "Booted"); err != nil {
		return err
	}
	fmt.Printf("%s (%s): new launches no longer load stuntcam; apps already running keep it until they quit\n", d.Name, d.UDID)
	return nil
}

type simStatus struct {
	UDID, Name, Runtime, State, Mode, Build string
	Xcode                                   string            `json:",omitempty"` // the build recorded at arm time (config.json)
	BootEnv                                 string            `json:",omitempty"` // what launchd_sim's environment says: "stub", "no stub", or why it couldn't be read
	SimDir                                  string            `json:",omitempty"` // the SIMCAM_SIM_DIR the boot environment names (when it has the stub)
	ArmedAt                                 int64             `json:",omitempty"`
	Chain                                   []string          `json:",omitempty"`
	Latched                                 map[string]string `json:",omitempty"` // bundle -> features
	LatchEffect                             map[string]string `json:",omitempty"` // bundle -> what the latch does (motion only stocks CoreMotion)
	PreflightFail                           string            `json:",omitempty"` // a valid preflight-fail marker of the current build: CoreMotion stock
	Fallback                                map[string]string `json:",omitempty"` // bundle -> what its container-tmp fallback latches or stocks (engine: state dir not openable)
	FallbackDir                             map[string]string `json:",omitempty"` // bundle -> that fallback's directory
	FallbackEffect                          map[string]string `json:",omitempty"` // bundle -> what that fallback's markers do at the next launch ("" = nothing unless it falls back again)
	FallbackUnchecked                       map[string]string `json:",omitempty"` // bundle -> why its container tmp wasn't checked (not booted, not installed)
	Note                                    string            `json:",omitempty"`
}

func statusOf(d simctl.Device) simStatus {
	s := simStatus{UDID: d.UDID, Name: d.Name, Runtime: runtimeName(d.RuntimeID), State: d.State, Mode: "unarmed"}
	_, loader, cerr := home.ReadCurrent(d.UDID)
	if cerr == nil {
		s.Build = filepath.Base(filepath.Dir(loader))
	}
	r, rerr := arm.ReadRecord(d.UDID)
	if c, err := arm.ReadConfig(d.UDID); err == nil {
		s.Xcode = c.Build
	}
	// boot mode is visible only in launchd_sim's own environment (`launchctl getenv` inside the sim reads it as empty)
	envArmed, envDir, envErr := false, "", errors.New("not booted")
	if d.State == "Booted" {
		var env map[string]string
		if env, envErr = bootEnv(d.UDID); envErr == nil {
			envArmed, envDir = procenv.Armed(env, artifacts.StubName)
			s.SimDir = envDir
			s.BootEnv = map[bool]string{true: "stub", false: "no stub"}[envArmed]
		} else {
			s.BootEnv = "unreadable: " + envErr.Error()
		}
	}
	switch {
	case d.State != "Booted":
		if rerr == nil && r.Mode == arm.Boot {
			s.Note = "boot-mode arming ends at shutdown; `stuntcam up` boots it armed"
		}
	case rerr == nil && arm.InProgress(r):
		s.Mode, s.Note = "booting", "being booted armed by `stuntcam up` or `exec` right now"
	case envErr == nil && envArmed && envDir != "" && filepath.Clean(envDir) != home.Sim(d.UDID):
		s.Mode, s.Note = "boot", "boot-armed with another directory, "+envDir+" (a simulator `stuntcam exec`'s command booted?)"
		if filepath.Base(envDir) == d.UDID {
			s.Note = "boot-armed under another stuntcam home: " + envDir + " (SIMCAM_HOME differs from " + home.Root() + ")"
		}
		if b, err := os.ReadFile(filepath.Join(envDir, "current")); err == nil {
			if _, l, err := home.ParseCurrent(string(b)); err == nil {
				s.Build = filepath.Base(filepath.Dir(l))
			}
		}
		if b, err := os.ReadFile(filepath.Join(envDir, "config.json")); err == nil {
			var c arm.Config
			if json.Unmarshal(b, &c) == nil {
				s.Xcode = c.Build
			}
		}
	case envErr == nil && envArmed && cerr != nil:
		s.Mode, s.Note = "booted-unarmed", "`stuntcam down` since this boot: its boot environment still has the stub, so `stuntcam up` re-arms it without a reboot"
	case envErr == nil && envArmed:
		s.Mode = "boot"
		if rerr == nil && r.Mode == arm.Boot {
			s.ArmedAt = r.At
		} else {
			s.Note = "boot-armed outside the stuntcam command (scripts/boot.sh?)"
		}
	case envErr == nil && rerr == nil && r.Mode == arm.Boot:
		s.Mode = "booted-unarmed"
		s.Note = "rebooted since `stuntcam up` (by Xcode, Simulator or a restart): run `stuntcam up --reboot` for Xcode Run"
	case rerr != nil && cerr == nil:
		s.Mode, s.Note = "armed", "armed outside the stuntcam command (scripts/boot.sh or launchd-arm.sh?): mode unknown"
	case cerr == nil && r.Mode == arm.Env:
		s.Mode, s.Note = "env", "per-launch arming only (`stuntcam env`): launches without its exports get no camera"
	case rerr != nil || cerr != nil:
		s.Mode = "booted-unarmed"
		if rerr == nil && arm.SameBoot(r, true, d.LastBooted) {
			s.Note = "`stuntcam down` since this boot: its boot environment still has the stub, so `stuntcam up` re-arms it without a reboot"
		}
	case r.Mode == arm.Boot && !arm.SameBoot(r, true, d.LastBooted):
		s.Mode = "booted-unarmed"
		s.Note = "rebooted since `stuntcam up` (by Xcode, Simulator or a restart): run `stuntcam up --reboot` for Xcode Run"
	default:
		s.Mode, s.ArmedAt = string(r.Mode), r.At
		if r.Mode == arm.Launchd {
			if v, _ := ctl.Getenv(d.UDID, "DYLD_INSERT_LIBRARIES"); !strings.Contains(v, artifacts.StubName) {
				s.Mode, s.Note = "booted-unarmed", "the stub is no longer in the simulator's launchd environment"
			}
		}
	}
	if b, err := os.ReadFile(home.ChainFile(d.UDID)); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if l = strings.TrimSpace(l); l != "" && !strings.HasPrefix(l, "#") {
				s.Chain = append(s.Chain, l)
			}
		}
	}
	for _, b := range state.Bundles(d.UDID) {
		var feats []string
		if a, err := state.Load(d.UDID, b, s.Build); err == nil { // latches count for the build `current` names (the engine's rule)
			if f := a.LatchedFeatures(); len(f) > 0 {
				if s.Latched == nil {
					s.Latched = map[string]string{}
				}
				s.Latched[b] = strings.Join(f, ",")
				feats = append(feats, f...)
			}
		}
		for _, m := range state.Markers(d.UDID, b) { // a valid marker for the current build: stock without activating
			if m.Build == s.Build && m.Valid(time.Now()) {
				if s.Latched == nil {
					s.Latched = map[string]string{}
				}
				s.Latched[b] = strings.TrimPrefix(s.Latched[b]+fmt.Sprintf(" (marker: %s until %s)", strings.ReplaceAll(m.Features, " ", ","), m.Until.Format("2006-01-02 15:04")), " ")
				feats = append(feats, strings.Fields(m.Features)...)
			}
		}
		if len(feats) > 0 {
			if s.LatchEffect == nil {
				s.LatchEffect = map[string]string{}
			}
			s.LatchEffect[b] = state.Effect(feats)
		}
	}
	for _, p := range state.PreflightFails(d.UDID) { // a real preflight failure stocks CoreMotion for 24 h
		if p.Build == s.Build && p.Valid(time.Now()) {
			s.PreflightFail = fmt.Sprintf("%s until %s", p.What, p.Until.Format("2006-01-02 15:04"))
		}
	}
	return s
}

// statusWithFallback adds what the engine's container-tmp fallback holds (the app couldn't open its state dir, e.g. on 17.5):
// markers there keep the app stock just the same. Only `stuntcam status` asks: it costs one simctl call
// per known bundle, which the service, exec and doctor (they need only Mode) must not pay.
func statusWithFallback(d simctl.Device, s simStatus) simStatus {
	for _, b := range state.Bundles(d.UDID) {
		f, why := fallbackOf(d, b)
		if why != "" {
			if s.FallbackUnchecked == nil {
				s.FallbackUnchecked = map[string]string{}
			}
			s.FallbackUnchecked[b] = why
			continue
		}
		desc, ff := f.Read(b, s.Build).Latches(s.Build, time.Now())
		if desc == "" {
			continue
		}
		if s.Fallback == nil {
			s.Fallback, s.FallbackDir = map[string]string{}, map[string]string{}
		}
		s.Fallback[b], s.FallbackDir[b] = desc, f.Dir
		if len(ff) > 0 { // per location: a state-dir latch's line never borrows the fallback's effect
			if s.FallbackEffect == nil {
				s.FallbackEffect = map[string]string{}
			}
			s.FallbackEffect[b] = state.Effect(ff)
		}
	}
	return s
}

// fallbackOf finds an app's container-tmp fallback on a sim through `simctl get_app_container <udid> <bundle> data`. why != ""
// says why it couldn't be checked: the sim isn't booted (simctl answers only for a booted device), the app isn't installed,
// or simctl's error.
func fallbackOf(d simctl.Device, bundle string) (state.Fallback, string) {
	if d.State != "Booted" {
		return state.Fallback{}, "simulator not booted"
	}
	if why := fallbackSkip[d.UDID]; why != "" { // the sim already timed out or turned out not booted: skip its other bundles
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

// stateDirBlocked: the state dir is missing, not a directory or not writable. The engine can't open it either, so it keeps
// its state in the fallback, which the clear reached: not a reason to exit 1 (e.g. a state dir blocked with mode 555).
func stateDirBlocked(err error) bool {
	return errors.Is(err, os.ErrNotExist) || errors.Is(err, syscall.ENOTDIR) || errors.Is(err, os.ErrPermission)
}

// fallbackTimeout bounds each get_app_container call; fallbackSkip remembers, per sim, why its remaining bundles are skipped.
var (
	fallbackTimeout = 10 * time.Second
	fallbackSkip    = map[string]string{}
)

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return strings.TrimSpace(s[:i])
	}
	return s
}

var bootEnv = procenv.BootEnv // replaced in tests

var xcodeActive, selectedXcode = xcode.Active, xcode.Selected // replaced in tests

func cmdStatus(args []string) error {
	fs := flag.NewFlagSet("status", flag.ContinueOnError)
	asJSON := fs.Bool("json", false, "JSON output")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	ds, err := ctl.Devices()
	if err != nil {
		return err
	}
	var out []simStatus
	for _, d := range ds {
		_, known := os.Stat(home.Sim(d.UDID))
		if d.State == "Booted" || known == nil {
			out = append(out, statusWithFallback(d, statusOf(d)))
		}
	}
	if *asJSON {
		e := json.NewEncoder(os.Stdout)
		e.SetIndent("", " ")
		return e.Encode(out)
	}
	if len(out) == 0 {
		fmt.Println("no booted simulators and none armed before (stuntcam up)")
	}
	for _, s := range out {
		fmt.Printf("%s (%s, %s): %s, %s", s.Name, s.UDID, s.Runtime, s.State, s.Mode)
		if s.Build != "" {
			fmt.Printf(", build %s", s.Build)
		}
		if s.Xcode != "" {
			fmt.Printf(", Xcode %s", s.Xcode)
		}
		if s.ArmedAt > 0 {
			fmt.Printf(", armed %s ago", ago(time.Since(time.Unix(s.ArmedAt, 0))))
		}
		fmt.Println()
		if s.Note != "" {
			fmt.Println("  " + s.Note)
		}
		for _, c := range s.Chain {
			fmt.Println("  chain: " + c)
		}
		for b, f := range s.Latched {
			fmt.Printf("  latched: %s (%s): %s until `stuntcam sentinel clear --bundle %s`\n", b, f, s.LatchEffect[b], b)
		}
		for _, b := range sortedKeys(s.Fallback) {
			switch eff := s.FallbackEffect[b]; {
			case eff != "":
				fmt.Printf("  in the container-tmp fallback: %s (%s; %s): %s until `stuntcam sentinel clear --bundle %s`\n", b, s.Fallback[b], s.FallbackDir[b], eff, b)
			case strings.Contains(s.Fallback[b], "preflight-fail"):
				fmt.Printf("  in the container-tmp fallback: %s (%s; %s): stock CoreMotion until `stuntcam sentinel clear --bundle %s`\n", b, s.Fallback[b], s.FallbackDir[b], b)
			default: // records only: the engine reads them only when the app again can't open its state dir
				fmt.Printf("  in the container-tmp fallback: %s (%s; %s): latches its next launch if it again can't open its state dir; clear with `stuntcam sentinel clear --bundle %s`\n", b, s.Fallback[b], s.FallbackDir[b], b)
			}
		}
		if s.PreflightFail != "" {
			fmt.Printf("  preflight failed (%s): apps get stock CoreMotion; a passing preflight or `stuntcam sentinel clear` resets it\n", s.PreflightFail)
		}
		if len(s.FallbackUnchecked) > 0 {
			fmt.Printf("  container-tmp fallback not checked: %s\n", uncheckedText(s.FallbackUnchecked))
		}
	}
	return nil
}

func cmdSwap(args []string) error {
	fs := flag.NewFlagSet("swap", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if _, _, err := home.ReadCurrent(d.UDID); err != nil {
		return fmt.Errorf("%s isn't armed (no current): `stuntcam up` first; swap only changes the build of an armed simulator", d.Name)
	}
	var set artifacts.Set
	if fs.NArg() > 0 {
		set, err = artifacts.Find(filepath.Dir(fs.Arg(0)))
	} else {
		set, err = artifacts.Find("")
	}
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return err
	}
	if err := home.WriteCurrent(d.UDID, set.Loader); err != nil {
		return err
	}
	if _, err := arm.ReadConfig(d.UDID); err != nil { // as `env` does: a sim armed before config.json existed gets its record
		recordXcode(d.UDID, set.Rules, "swap", false, false)
	} else if set.Rules != "" { // the new build's restore rules apply from the next launch too
		if err := arm.SetRulesDir(d.UDID, set.Rules); err != nil {
			fmt.Fprintf(os.Stderr, "warning: pointing %s at the new build's restore rules: %v\n", d.Name, err)
		}
	}
	if r, err := arm.ReadRecord(d.UDID); err == nil {
		r.Build = set.BuildID()
		_ = arm.WriteRecord(d.UDID, r)
	}
	fmt.Printf("%s now uses build %s from the next app launch (running apps keep theirs)\n", d.Name, set.BuildID())
	return nil
}

func cmdChain(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return usagef("usage: stuntcam chain add|rm|ls [--udid U] [LIB] (the action comes first)")
	}
	if args[0] != "add" && args[0] != "rm" && args[0] != "ls" {
		return usagef("unknown chain action %q (add, rm, ls)", args[0])
	}
	fs := flag.NewFlagSet("chain", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	maxPos := 1
	if args[0] == "ls" {
		maxPos = 0
	}
	if err := parse(fs, args[1:], maxPos); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	p := home.ChainFile(d.UDID)
	b, _ := os.ReadFile(p)
	var cur []string
	for _, l := range strings.Split(string(b), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			cur = append(cur, l)
		}
	}
	switch args[0] {
	case "ls":
		for _, l := range cur {
			fmt.Println(l)
		}
		return nil
	case "add", "rm":
		if fs.NArg() != 1 {
			return usagef("usage: stuntcam chain %s [--udid U] LIB", args[0])
		}
		lib, _ := filepath.Abs(fs.Arg(0))
		var next []string
		for _, l := range cur {
			if l != lib {
				next = append(next, l)
			}
		}
		if args[0] == "add" {
			if strings.HasPrefix(filepath.Base(lib), "libsimcam") {
				return errors.New("stuntcam's own libraries are never chained (the loader skips them)")
			}
			if fi, err := os.Stat(lib); err != nil || fi.IsDir() {
				return fmt.Errorf("%s: no such library", lib)
			}
			if home.Protected(lib) {
				return fmt.Errorf("%s is in a privacy-protected folder the simulator can't read; copy it under %s", lib, home.Root())
			}
			next = append(next, lib)
		}
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		data := strings.Join(next, "\n")
		if data != "" {
			data += "\n"
		}
		if err := home.WriteAtomic(p, []byte(data)); err != nil {
			return err
		}
		fmt.Printf("chain for %s: %d librar%s (loaded from the next app launch)\n", d.Name, len(next), map[bool]string{true: "y", false: "ies"}[len(next) == 1])
		return nil
	}
	return fmt.Errorf("unknown chain action %q", args[0])
}

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
		unchecked := map[string]string{}
		for _, b := range bundles {
			a, err := state.Load(d.UDID, b, cur) // "" (not armed): the last activation's build
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
				unchecked[b] = why
				continue
			}
			fs := f.Read(b, cur)
			if fs.Empty() {
				continue
			}
			if err != nil {
				fmt.Printf("%s: no records in the state dir\n", b)
			}
			fmt.Printf("  container-tmp fallback %s:", f.Dir)
			if fs.Events {
				fmt.Printf(" %d activations since the last clear", fs.App.Activations)
				if lf := fs.App.LatchedFeatures(); len(lf) > 0 { // conditional, as in status
					fmt.Printf(", LATCHED %s if the app again can't open its state dir (%s)", strings.Join(lf, ","), state.Effect(lf))
				}
			} else {
				fmt.Printf(" no events.log")
			}
			fmt.Println()
			printSentinel(fs.App, fs.Markers)
			for _, p := range fs.PreflightFails {
				fmt.Printf("  %s\n", preflightLine(p))
			}
		}
		for _, p := range state.PreflightFails(d.UDID) { // per sim, not per bundle
			fmt.Printf("sim: %s\n", preflightLine(p))
		}
		if len(unchecked) > 0 {
			fmt.Printf("container-tmp fallback not checked: %s\n", uncheckedText(unchecked))
		}
		return nil
	case "clear":
		unchecked := map[string]string{}
		var failed, incomplete []string
		for _, b := range bundles {
			serr := state.Clear(d.UDID, b, "stuntcam sentinel clear")
			f, why := fallbackOf(d, b)
			var done []string
			var ferr error
			if why == "" {
				done, ferr = f.Clear("stuntcam sentinel clear")
			} else {
				unchecked[b] = why
			}
			var msg []string
			if serr == nil {
				msg = append(msg, "cleared (state dir)")
			} else {
				msg = append(msg, "state dir not cleared: "+serr.Error())
			}
			if len(done) > 0 {
				msg = append(msg, fmt.Sprintf("container-tmp fallback %s: %s", f.Dir, strings.Join(done, ", ")))
			}
			if ferr != nil {
				msg = append(msg, fmt.Sprintf("container-tmp fallback %s not fully cleared: %v", f.Dir, ferr))
			}
			ok := serr == nil || len(done) > 0
			if !ok {
				failed = append(failed, b)
			}
			// a location the engine reads was reached and left latched, or couldn't be checked for a reason other than "not
			// booted"/"not installed": scripts must not read success
			if ferr != nil || (why != "" && why != "app not installed" && why != "simulator not booted") ||
				(serr != nil && !stateDirBlocked(serr)) {
				incomplete = append(incomplete, b)
			}
			// the camera claim holds when every location the engine reads was reached (no container: nothing there)
			if serr == nil && ferr == nil && (why == "" || why == "app not installed") {
				msg = append(msg, "its next launch gets the camera again")
			}
			fmt.Printf("%s: %s\n", b, strings.Join(msg, "; "))
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
	return fmt.Errorf("unknown sentinel action %q", args[0])
}

// cmdTrace prints an app's op ring (design §4.5; engine/src/core/trace.inc): the newest trace-<pid>.txt in its state dir or its
// container-tmp fallback (found as `sentinel` finds it), or the one of --pid. The app may be running (the file is rewritten
// at most every 150 ms after an op) or gone (its last copy). No trace: exit 1.
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

// printSentinel prints the counted events, the debug-end note and the markers of one location.
func printSentinel(a state.App, ms []state.Marker) {
	for _, e := range a.Counted {
		fmt.Printf("  %s %s %s (activation %d, build %s) %s\n", time.Unix(e.At, 0).Format("2006-01-02 15:04:05"), e.Kind, e.Feature, e.Activation, e.Build, e.Detail)
	}
	if a.UnknownDebugEnd != "" {
		fmt.Printf("  last debug run ended inside a %s op (not counted; if stuntcam crashed there, report it)\n", a.UnknownDebugEnd)
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
	return fmt.Sprintf("marker preflight-fail-%s: %s until %s (%s)", p.Build, p.What, p.Until.Format("2006-01-02 15:04:05"), v)
}

func cmdEnv(args []string) error {
	fs := flag.NewFlagSet("env", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	build := fs.String("build", "", "build dir")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	set, err := artifacts.Find(*build)
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return err
	}
	if err := envArm(d, set, "env"); err != nil {
		return err
	}
	fmt.Printf("# stuntcam for launches on %s (%s); a launch that sets its own DYLD_INSERT_LIBRARIES replaces this one\n", d.Name, d.UDID)
	for _, kv := range arm.Vars(d.UDID, set, arm.Options{}) {
		k, v, _ := strings.Cut(kv, "=")
		fmt.Printf("export SIMCTL_CHILD_%s=%s\n", k, shellQuote(v))
	}
	return nil
}

// doctorCheck is one line of `stuntcam doctor` (--json: one entry per line, in order).
type doctorCheck struct {
	Level string `json:"level"` // ok, warn, fail, info
	Msg   string `json:"msg"`
}

func cmdDoctor(args []string) error {
	fs := flag.NewFlagSet("doctor", flag.ContinueOnError)
	asJSON := fs.Bool("json", false, "JSON output (for bug reports)")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	bad := 0
	var checks []doctorCheck
	sims := []simStatus{}
	emit := func(level, msg string) {
		checks = append(checks, doctorCheck{level, msg})
		if !*asJSON {
			tag := map[string]string{"ok": "ok  ", "warn": "warn", "fail": "FAIL", "info": "info"}[level]
			fmt.Printf("%s %s\n", tag, msg)
		}
	}
	say := func(ok bool, warn bool, msg string) {
		level := "ok"
		if !ok {
			level = "fail"
			if warn {
				level = "warn"
			} else {
				bad++
			}
		}
		emit(level, msg)
	}
	act, aerr := xcode.Active()
	say(aerr == nil, false, fmt.Sprintf("Xcode in use: %s (%s) at %s%s", act.Version, act.Build, act.DeveloperDir, errText(aerr)))
	for _, n := range xcodeWarnings(act, aerr, xcode.Installed(), os.Getenv("DEVELOPER_DIR"), runningXcodes()) {
		if n.Warn {
			say(false, true, n.Msg)
		} else {
			emit("info", n.Msg)
		}
	}
	if aerr == nil {
		rulesDir := sourceRulesDir() // the rules `stuntcam up` would stage: this build's restore-rules/
		f, ok := xcode.Rules(rulesDir, act.Build)
		if ok {
			say(true, false, "restore rules for "+act.Build+": "+f)
		} else {
			say(false, true, xcode.UnverifiedLine(act.Build)+" (no usable "+act.Build+".json in "+rulesDir+"; best guess "+map[bool]string{true: f, false: "the built-in 27A266a rules"}[f != ""]+")")
		}
	}
	if rs, err := ctl.Runtimes(); err == nil {
		var v []string
		for _, r := range rs {
			if r.Available && r.Platform == "iOS" {
				v = append(v, r.Version+" ("+r.Build+")")
			}
		}
		say(len(v) > 0, false, "iOS simulator runtimes: "+strings.Join(v, ", "))
	} else {
		say(false, false, "simctl: "+err.Error())
	}
	root := home.Root()
	say(!home.Protected(root), false, "host directory "+root+" (simulator processes must be able to read it)")
	if err := os.MkdirAll(root, 0o755); err == nil {
		f, err := os.CreateTemp(root, ".doctor")
		if err == nil {
			f.Close()
			os.Remove(f.Name())
		}
		say(err == nil, false, "host directory writable"+errText(err))
	}
	if wd, err := os.Getwd(); err == nil && home.Protected(wd) {
		say(false, true, "this directory ("+wd+") is privacy-protected: fine for your project, but never pass paths inside it to the simulator (stuntcam stages its files under "+root+")")
	}
	set, err := artifacts.Find("")
	where := set.From
	if set.Installed && err == nil {
		anchor := "manifest anchored to this binary"
		if ok, _ := artifacts.Anchored(); !ok {
			anchor = "manifest not anchored: a dev build of the CLI"
		}
		where += fmt.Sprintf(" (installed package %s, build %s, %d shipped verdict(s); every file matches its manifest, %s; payload unpacked under %s)", set.Version, set.BuildID(), len(set.ShippedVerdicts()), anchor, home.Root())
	}
	say(err == nil, false, "build: "+where+errText(err))
	if err == nil {
		ps := []string{set.Stub, set.Loader, set.Engine}
		if set.Hook != "" {
			ps = append(ps, set.Hook)
		}
		for _, p := range ps {
			o, _ := exec.Command("codesign", "-dv", p).CombinedOutput()
			s := string(o)
			say(strings.Contains(s, "adhoc") && strings.Contains(s, "linker-signed"), false, filepath.Base(p)+" ad-hoc linker-signed")
		}
	}
	ds, err := ctl.Devices()
	if err == nil {
		for _, d := range ds {
			if d.State != "Booted" {
				continue
			}
			s := statusOf(d)
			sims = append(sims, s)
			say(s.Mode == "boot" || s.Mode == "launchd", true, fmt.Sprintf("%s (%s): %s%s", d.Name, runtimeName(d.RuntimeID), s.Mode, prefix(" — ", s.Note)))
			if s.BootEnv != "" { // text unchanged from before --json; JSON gets it as an info entry
				checks = append(checks, doctorCheck{"info", d.Name + " boot environment (launchd_sim): " + s.BootEnv})
				if !*asJSON {
					fmt.Printf("     boot environment (launchd_sim): %s\n", s.BootEnv)
				}
			}
			if s.Mode == "boot" || s.Mode == "launchd" {
				if w := recordWarning(d, s, act, aerr); w != "" {
					say(false, true, w)
				}
			}
			for _, b := range sortedKeys(s.Latched) {
				say(false, true, fmt.Sprintf("%s on %s latched (%s): stuntcam sentinel ls --udid %s", b, d.Name, s.Latched[b], d.UDID))
			}
		}
	} else {
		say(false, false, "simctl: "+err.Error())
	}
	if *asJSON {
		out := struct {
			Schema  int           `json:"schema"`
			Version string        `json:"version"`
			Commit  string        `json:"commit,omitempty"`
			Host    string        `json:"host"`
			Xcode   xcode.Info    `json:"xcode"`
			Checks  []doctorCheck `json:"checks"`
			Failed  int           `json:"failed"`
			Booted  []simStatus   `json:"booted_sims"`
		}{1, version, commit, runtime.GOOS + "/" + runtime.GOARCH, act, checks, bad, sims}
		b, _ := json.MarshalIndent(out, "", "  ")
		fmt.Println(string(b))
	}
	if bad > 0 {
		return fmt.Errorf("%d check(s) failed", bad)
	}
	return nil
}

// xcodeNote is one doctor line about Xcode: a warning, or information only.
type xcodeNote struct {
	Warn bool
	Msg  string
}

// xcodeWarnings: DEVELOPER_DIR naming another Xcode than xcode-select, a running Xcode.app that isn't the one in use, and
// several Xcodes installed (restores follow the one recorded at `up`, and ⌘R from another isn't noticed in-process). Several
// Xcodes alone is information; a warning only with one of the mismatches. Xcodes on /Volumes (disk images,
// backups) are left out unless one is the Xcode in use. A DEVELOPER_DIR naming the .app counts as its Contents/Developer.
func xcodeWarnings(act xcode.Info, aerr error, installed []xcode.Info, devDir string, running []string) []xcodeNote {
	var w []xcodeNote
	if devDir = xcode.NormalizeDeveloperDir(devDir); devDir != "" {
		if sel, err := selectedXcode(); err == nil && !xcode.Same(devDir, sel) {
			w = append(w, xcodeNote{true, "DEVELOPER_DIR=" + devDir + " differs from xcode-select -p (" + sel + "): `stuntcam up` here records DEVELOPER_DIR's Xcode, while Xcode.app's ⌘R is whichever Xcode.app you opened"})
		}
	}
	if aerr == nil {
		for _, app := range running {
			if a := xcode.App(act.DeveloperDir); a != "" && !xcode.Same(app, a) {
				w = append(w, xcodeNote{true, "running Xcode " + app + " is not the Xcode in use here (" + a + "): its ⌘R gets the restore rules of the Xcode recorded at `stuntcam up`"})
			}
		}
	}
	var l []string
	for _, i := range installed {
		if strings.HasPrefix(i.DeveloperDir, "/Volumes/") && !(aerr == nil && xcode.Same(i.DeveloperDir, act.DeveloperDir)) {
			continue
		}
		l = append(l, fmt.Sprintf("%s (%s %s)", xcode.App(i.DeveloperDir), i.Version, i.Build))
	}
	if len(l) > 1 {
		n := xcodeNote{len(w) > 0, "several Xcodes installed: " + strings.Join(l, ", ") + ". Restore rules follow the one active at `stuntcam up` (DEVELOPER_DIR, else xcode-select); after ⌘R from another, run `stuntcam up` from a shell where it is active"}
		w = append([]xcodeNote{n}, w...)
	}
	return w
}

// recordWarning: an armed sim whose recorded Xcode (config.json) is not the one in use now.
func recordWarning(d simctl.Device, s simStatus, act xcode.Info, aerr error) string {
	c, err := arm.ReadConfig(d.UDID)
	if s.SimDir != "" && filepath.Clean(s.SimDir) != home.Sim(d.UDID) { // armed with another directory: its record is there
		var b []byte
		if b, err = os.ReadFile(filepath.Join(s.SimDir, "config.json")); err == nil {
			c = arm.Config{}
			err = json.Unmarshal(b, &c)
		}
	}
	switch {
	case err != nil:
		return fmt.Sprintf("%s: no Xcode recorded (armed by an older stuntcam or the scripts?): apps say \"restore rules unverified\"; `stuntcam up --udid %s` records it", d.Name, d.UDID)
	case aerr != nil:
		return ""
	case c.Build != act.Build || !xcode.Same(xcode.NormalizeDeveloperDir(c.DeveloperDir), xcode.NormalizeDeveloperDir(act.DeveloperDir)):
		return fmt.Sprintf("%s was armed for Xcode %s (%s) but the Xcode in use is %s (%s): `stuntcam up --udid %s` re-records it (no reboot)", d.Name, c.Build, c.DeveloperDir, act.Build, act.DeveloperDir, d.UDID)
	case !simRulesVerified(c) && sourceRulesVerified(c.Build):
		// the loader reads the rules staged when the sim was armed, not this build's
		return fmt.Sprintf("%s applies the restore rules staged when it was armed (%s), which have none for Xcode %s; this build has them: `stuntcam up --udid %s` or `stuntcam swap --udid %s` stages them (no reboot)", d.Name, rulesDirName(c.RulesDir), c.Build, d.UDID, d.UDID)
	}
	return ""
}

// simRulesVerified: the loader would find verified rules for the recorded build in the sim's own rules dir ("" = the
// built-in rules, which are 27A266a's).
func simRulesVerified(c arm.Config) bool {
	if c.RulesDir == "" {
		return c.Build == builtinRulesBuild
	}
	_, ok := xcode.Rules(c.RulesDir, c.Build)
	return ok
}

// builtinRulesBuild is the Xcode build the loader's built-in rules were measured for (loader.c dflt).
const builtinRulesBuild = "27A266a"

// sourceRulesDir is the rules `stuntcam up` would stage now: this build's restore-rules/ (replaced in tests).
var sourceRulesDir = func() string {
	if s, err := artifacts.Find(""); err == nil && s.Rules != "" {
		return s.Rules
	}
	return home.Rules()
}

func sourceRulesVerified(build string) bool {
	if build == "" {
		return false
	}
	_, ok := xcode.Rules(sourceRulesDir(), build)
	return ok
}

func rulesDirName(dir string) string {
	if dir == "" {
		return "none: the built-in rules"
	}
	return dir
}

// runningXcodes: the Xcode.app bundles of running Xcode processes (…/Xcode*.app/Contents/MacOS/Xcode).
func runningXcodes() []string {
	out, err := exec.Command("ps", "-axww", "-o", "args=").Output()
	if err != nil {
		return nil
	}
	return parseRunningXcodes(string(out))
}

func parseRunningXcodes(ps string) []string {
	var apps []string
	seen := map[string]bool{}
	for _, l := range strings.Split(ps, "\n") {
		i := strings.Index(l, ".app/Contents/MacOS/Xcode")
		if i < 0 || (len(l) > i+25 && l[i+25] != ' ') {
			continue
		}
		app := strings.TrimSpace(l[:i+4])
		if !seen[app] {
			seen[app] = true
			apps = append(apps, app)
		}
	}
	return apps
}

func errText(err error) string {
	if err != nil {
		return " (" + err.Error() + ")"
	}
	return ""
}

func prefix(p, s string) string {
	if s == "" {
		return ""
	}
	return p + s
}

func ago(d time.Duration) string {
	switch {
	case d < time.Minute:
		return fmt.Sprintf("%ds", int(d.Seconds()))
	case d < time.Hour:
		return fmt.Sprintf("%dm", int(d.Minutes()))
	case d < 48*time.Hour:
		return fmt.Sprintf("%dh%02dm", int(d.Hours()), int(d.Minutes())%60)
	}
	return fmt.Sprintf("%dd", int(d.Hours()/24))
}

// shellQuote single-quotes a value for sh ($, backticks and backslashes stay literal).
func shellQuote(v string) string { return "'" + strings.ReplaceAll(v, "'", `'\''`) + "'" }

// springboardUp waits until the sim's launchd lists a running SpringBoard.
func springboardUp(udid string, bound time.Duration) bool {
	c := ctl
	c.Timeout = 10 * time.Second // one slow spawn must not outlast the bound
	for end := time.Now().Add(bound); time.Now().Before(end); time.Sleep(time.Second) {
		out, err := c.Spawn(udid, "launchctl", "list")
		if err != nil {
			continue
		}
		for _, l := range strings.Split(out, "\n") {
			if f := strings.Fields(l); len(f) == 3 && f[2] == "com.apple.SpringBoard" && f[0] != "-" {
				return true
			}
		}
	}
	return false
}

// envArm prepares a booted sim for per-launch arming (`stuntcam env`, and exec's runner hook on an unarmed destination):
// its dirs, `current` if missing (an existing one is kept: a `stuntcam swap` would otherwise be undone), the Xcode
// record, and an "env" arm record unless one of another mode exists.
func envArm(d simctl.Device, set artifacts.Set, source string) error {
	if _, _, err := home.ReadCurrent(d.UDID); err == nil {
		set.Loader = "" // Prepare skips current when Loader is empty
	} else if rec, err := arm.ReadRecord(d.UDID); err == nil && arm.SameBoot(rec, d.State == "Booted", d.LastBooted) {
		return fmt.Errorf("%s was booted armed and then `stuntcam down`: writing current would re-arm every launch on it, not just yours; `stuntcam up` to re-arm, or reboot it unarmed first", d.Name)
	}
	wroteCurrent := set.Loader != ""
	if err := arm.Prepare(d.UDID, set); err != nil {
		return err
	}
	if _, err := arm.ReadConfig(d.UDID); err != nil || wroteCurrent { // keep the record of an `up` (it may be for another Xcode on purpose)
		recordXcode(d.UDID, set.Rules, source, false, false)
	}
	if old, err := arm.ReadRecord(d.UDID); wroteCurrent && (err != nil || old.Mode == arm.Env) { // say so, so status and the login service don't take this sim for one armed by the scripts (an existing record, e.g. launchd names a later `down` needs, stays)
		if err := arm.WriteRecord(d.UDID, arm.Record{Mode: arm.Env, Build: set.BuildID(), At: time.Now().Unix()}); err != nil {
			return err
		}
	}
	return nil
}

func cmdExec(args []string) error {
	fs := flag.NewFlagSet("exec", flag.ContinueOnError)
	build := fs.String("build", "", "build dir")
	if err := fs.Parse(args); err != nil { // the command after "--" keeps its own options
		return usageError{err}
	}
	cmdline := fs.Args()
	if len(cmdline) > 0 && cmdline[0] == "--" {
		cmdline = cmdline[1:]
	}
	if len(cmdline) == 0 {
		return usagef("usage: stuntcam exec [--build DIR] -- COMMAND [ARGS…]")
	}
	set, err := artifacts.Find(*build)
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return err
	}
	// sims the command boots share one run directory (its current, chain, state), since their UDIDs aren't known in advance;
	// unique per process, so parallel CI jobs don't share one
	run := fmt.Sprintf("exec-%d-%d", time.Now().UnixNano(), os.Getpid())
	if err := arm.Prepare(run, set); err != nil {
		return err
	}
	xi := recordXcode(run, set.Rules, "exec", false, true) // the Xcode the command's xcodebuild uses (same DEVELOPER_DIR)
	vars := arm.Vars(run, set, arm.Options{})
	env := os.Environ()
	for _, kv := range vars {
		k, v, _ := strings.Cut(kv, "=")
		if k == "DYLD_INSERT_LIBRARIES" { // keep another tool's insert the caller already exported
			v = arm.MergeInsert(os.Getenv("SIMCTL_CHILD_DYLD_INSERT_LIBRARIES"), v)
		}
		env = append(env, "SIMCTL_CHILD_"+k+"="+v)
	}
	// activations are counted in the run directory and in each -destination's own directory: one booted armed before this
	// command (by `up`, or an earlier exec's pre-boot) logs there, not in the run directory (otherwise a second exec on the
	// same destination reported no camera use)
	counted := append([]string{run}, destinationIDs(cmdline)...)
	xb := parseXcodebuild(cmdline)
	xtest := xb.OK && !xb.Cleared && xb.test()
	hookDir := filepath.Join(home.Sim(run), "hook")
	var hooked []simctl.Device
	type hookCand struct {
		d          simctl.Device
		mode, what string
	}
	var cands []hookCand // launchd-armed or unarmed iOS destinations, hooked only if planRunnerHook says so
	var dest destSet
	if ds, err := ctl.Devices(); err == nil {
		dest = destinations(cmdline, ds)
		// xcodebuild boots a shut-down -destination itself, and that boot does not get the SIMCTL_CHILD_ variables (seen on
		// Xcode 27.0: no stuntcam variable in the sim, no stub in the test host); so boot it armed before the command runs.
		// iOS simulators only: stuntcam arms nothing else
		pre, isDest := map[string]bool{}, map[string]bool{}
		for _, u := range destinationIDs(cmdline) {
			isDest[u] = true
			for _, d := range ds {
				if d.UDID != u {
					continue
				}
				if rec, err := arm.ReadRecord(u); err == nil && arm.InProgress(rec) {
					fmt.Fprintf(os.Stderr, "stuntcam: %s is being booted armed by another stuntcam (pid %d); not booting it again\n", d.Name, rec.Pid)
					continue
				}
				if d.State == "Shutdown" && !isIOSRuntime(d.RuntimeID) {
					fmt.Fprintf(os.Stderr, "stuntcam: %s is a %s simulator: not booting it armed (stuntcam arms iOS simulators only)\n", d.Name, runtimeName(d.RuntimeID))
					continue
				}
				if d.State == "Shutdown" {
					fmt.Fprintf(os.Stderr, "stuntcam: booting %s (%s) armed for the command (xcodebuild's own boot would not be)\n", d.Name, d.UDID)
					// armed with its own directory (as `up` does), so `down`, `swap`, `status` and `sentinel` see it
					recordXcode(d.UDID, set.Rules, "exec", false, true)
					if err := arm.ArmBoot(ctl, d.UDID, set, arm.Vars(d.UDID, set, arm.Options{})); err != nil {
						return fmt.Errorf("booting %s armed: %w", d.Name, err)
					}
					if !springboardUp(d.UDID, 60*time.Second) {
						fmt.Fprintf(os.Stderr, "stuntcam: %s has no SpringBoard after 60 s; running the command anyway\n", d.Name)
					}
					pre[d.UDID] = true
				}
			}
		}
		// already-booted iOS destinations (only the command's): the runner hook for launchd-armed and unarmed ones
		for _, d := range ds {
			if !isDest[d.UDID] || d.State != "Booted" || !isIOSRuntime(d.RuntimeID) {
				continue
			}
			m := "boot"
			if !pre[d.UDID] {
				m = hookMode(statusOf(d).Mode)
			}
			what := map[string]string{"launchd": "launchd-armed", "unarmed": "booted unarmed"}[m]
			switch {
			case m == "" || (m == "boot" && !xtest):
			case m == "boot":
				if err := writeHookEnv(hookDir, d.UDID, "boot", nil); err != nil {
					fmt.Fprintf(os.Stderr, "stuntcam: %v\n", err)
				}
			case !xtest && m == "launchd" && !xb.OK: // launchd arming already covers simctl launch, RN, Flutter, Detox
			case !xtest:
				fmt.Fprintf(os.Stderr, "stuntcam: %s is %s; Xcode test launches there get no camera unless the command is `xcodebuild test` itself (the runner hook), and Xcode Run never does (stuntcam up --udid %s --reboot first)\n", d.Name, what, d.UDID)
			case set.Hook == "":
				fmt.Fprintf(os.Stderr, "stuntcam: %s is %s and this build has no runner hook (%s; rebuild with ./build.sh): the command's test launches there get no camera (stuntcam up --udid %s --reboot first)\n", d.Name, what, artifacts.HookName, d.UDID)
			default:
				cands = append(cands, hookCand{d, m, what})
			}
		}
	}
	if xtest && set.Hook != "" && (len(cands) > 0 || len(dest.Named) > 0) {
		wd, _ := os.Getwd()
		rules := ""
		if set.Rules != "" && xi.Build != "" {
			rules = filepath.Join(set.Rules, xi.Build+".json")
		}
		var names []string
		for _, c := range cands {
			names = append(names, c.d.Name)
		}
		plan := planRunnerHook(hookInputs{Args: xb.Args, WrapEnv: xb.Env, WD: wd, Dest: dest, Hooked: names, Hook: set.Hook,
			Dir: hookDir, Stub: set.Stub, Rules: rules, RulesOK: testRunnerRule(rules), XcodeBuild: xi.Build,
			DeveloperDir: xi.DeveloperDir, Environ: os.Environ()})
		for _, n := range plan.Notes {
			fmt.Fprintln(os.Stderr, n)
		}
		if plan.Export {
			for _, c := range cands {
				d := c.d
				var hv []string
				if c.mode == "launchd" { // launchd gives the app every other variable
					cur, _ := ctl.Getenv(d.UDID, "DYLD_INSERT_LIBRARIES")
					st := stubIn(cur, artifacts.StubName)
					if st == "" {
						st = set.Stub
					}
					hv = []string{"DYLD_INSERT_LIBRARIES=" + st}
				} else {
					// stays env-armed afterwards, as after `stuntcam env` (cli/README.md, exec)
					if err := envArm(d, set, "exec"); err != nil {
						fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook there: its test launches get no camera\n", err)
						continue
					}
					hv = arm.Vars(d.UDID, set, arm.Options{})
				}
				if err := writeHookEnv(hookDir, d.UDID, c.mode, hv); err != nil {
					fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook on %s\n", err, d.Name)
					continue
				}
				hooked = append(hooked, d)
				fmt.Fprintf(os.Stderr, "stuntcam: %s is %s: UI tests' apps get the camera through the runner hook; a hosted test's host app doesn't (stuntcam up --udid %s --reboot arms that too)\n", d.Name, c.what, d.UDID)
			}
			if err := os.MkdirAll(hookDir, 0o755); err != nil {
				fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook\n", err)
			} else {
				env = append(env, plan.Env...)
			}
		}
	}
	c := exec.Command(cmdline[0], cmdline[1:]...)
	c.Env, c.Stdin, c.Stdout, c.Stderr = env, os.Stdin, os.Stdout, os.Stderr
	// stuntcam waits for the command and still reports. Signals that started out ignored (nohup, a background job) stay
	// ignored. SIGTERM/HUP sent to stuntcam (some CI runners) are passed on. A terminal Ctrl-C reaches the command by itself
	// when stuntcam is the terminal's foreground job, so the first SIGINT is passed on only otherwise (a `kill -INT` from a
	// script, no terminal); a second one always is.
	var watch []os.Signal
	for _, sg := range []os.Signal{os.Interrupt, syscall.SIGTERM, syscall.SIGHUP} {
		if !signal.Ignored(sg) {
			watch = append(watch, sg)
		}
	}
	sigs := make(chan os.Signal, 2)
	if len(watch) > 0 {
		signal.Notify(sigs, watch...)
	}
	defer signal.Stop(sigs)
	before := activations(counted) // a reused sim's directory has earlier activations
	if err := c.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v\n", err)
		if errors.Is(err, os.ErrPermission) {
			os.Exit(126)
		}
		os.Exit(127)
	}
	tty := inForegroundOfTerminal() // then a terminal Ctrl-C reaches the child too
	go func() {
		ints := 0
		for sg := range sigs {
			if sg == os.Interrupt {
				if ints++; tty && ints == 1 {
					continue
				}
			}
			if c.Process != nil {
				_ = c.Process.Signal(sg)
			}
		}
	}()
	runErr := c.Wait()
	var apps []string
	for k, n := range activations(counted) {
		if d := n - before[k]; d > 0 {
			apps = append(apps, fmt.Sprintf("%s (%d activation%s)", k, d, map[bool]string{true: "", false: "s"}[d == 1]))
		}
	}
	sort.Strings(apps)
	names := map[string]string{}
	for _, d := range hooked {
		names[d.UDID] = d.Name
	}
	hr := hookReport(hookDir)
	for _, u := range sortedHookUDIDs(hr) {
		c := hr[u]
		n := names[u]
		if n == "" {
			n = u
		}
		if c.Armed > 0 || names[u] != "" {
			fmt.Fprintf(os.Stderr, "stuntcam: runner hook on %s: %d app launch%s armed\n", n, c.Armed, map[bool]string{true: "", false: "es"}[c.Armed == 1])
		}
		for _, f := range c.Failures {
			fmt.Fprintf(os.Stderr, "stuntcam: runner hook on %s %s\n", n, f)
		}
	}
	if len(apps) > 0 {
		fmt.Fprintf(os.Stderr, "stuntcam: the camera was used by %s\n", strings.Join(apps, ", "))
	} else {
		fmt.Fprintln(os.Stderr, "stuntcam: no app used the camera in the command's simulators")
	}
	if ee, ok := runErr.(*exec.ExitError); ok {
		if ws, ok := ee.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
			os.Exit(128 + int(ws.Signal())) // as a shell reports a child killed by a signal
		}
		os.Exit(ee.ExitCode())
	}
	return runErr
}

func cmdService(args []string) error {
	if len(args) == 0 {
		return usagef("usage: stuntcam service install|run (--only U … | --all) [--preboot U] | uninstall | status")
	}
	fs := flag.NewFlagSet("service", flag.ContinueOnError)
	preboot := fs.String("preboot", "", "boot this simulator armed when the service starts (at login)")
	var only multi
	fs.Var(&only, "only", "arm only this simulator (repeatable)")
	all := fs.Bool("all", false, "arm every iOS simulator that boots unarmed (on a shared Mac this includes other people's)")
	if err := parse(fs, args[1:], 0); err != nil {
		return err
	}
	if (args[0] == "install" || args[0] == "run") && len(only) == 0 && !*all {
		return usagef("service %s needs --only <udid> (repeatable) or --all: it arms simulators that boot without stuntcam, and other tools' or people's simulators may be booted on this Mac", args[0])
	}
	if len(only) > 0 && *all {
		return usagef("--only and --all exclude each other")
	}
	for i, u := range only {
		only[i] = strings.ToUpper(u)
	}
	uid := fmt.Sprint(os.Getuid())
	switch args[0] {
	case "install":
		exe, err := os.Executable()
		if err != nil {
			return err
		}
		set, err := artifacts.Find("")
		if err != nil {
			return err
		}
		if set, err = artifacts.Stage(set); err != nil {
			return err
		}
		env := map[string]string{"STUNTCAM_BUILD": filepath.Dir(set.Loader), "STUNTCAM_STUB": set.Stub, "STUNTCAM_PROFILE": set.Profile}
		if set.Image != "" {
			env["STUNTCAM_IMAGE"] = set.Image
		}
		if ds, err := ctl.Devices(); err == nil {
			for _, u := range only {
				found := false
				for _, d := range ds {
					found = found || d.UDID == u
				}
				if !found {
					fmt.Fprintf(os.Stderr, "warning: --only %s is not a simulator on this Mac (xcrun simctl list devices)\n", u)
				}
			}
		}
		if *preboot != "" && len(only) > 0 && !containsUpper(only, *preboot) {
			return usagef("--preboot %s is not one of the --only simulators", *preboot)
		}
		for _, k := range []string{"SIMCAM_HOME", "DEVELOPER_DIR"} { // the same Xcode and host directory as this shell
			if v := os.Getenv(k); v != "" {
				env[k] = v
			}
		}
		if home.Protected(exe) {
			return fmt.Errorf("the stuntcam binary is in a privacy-protected folder (%s); a login agent can't run it from there", exe)
		}
		for _, d := range []string{filepath.Dir(service.PlistPath()), home.Logs()} { // launchd won't start a job whose log dir is missing
			if err := os.MkdirAll(d, 0o755); err != nil {
				return err
			}
		}
		_ = exec.Command("launchctl", "bootout", "gui/"+uid+"/"+service.Label).Run()
		if err := home.WriteAtomic(service.PlistPath(), []byte(service.Plist(exe, env, *preboot, only))); err != nil {
			return err
		}
		var out []byte
		for try := 0; try < 5; try++ { // right after a bootout, bootstrap can fail with "5: Input/output error" for a moment
			if out, err = exec.Command("launchctl", "bootstrap", "gui/"+uid, service.PlistPath()).CombinedOutput(); err == nil {
				break
			}
			time.Sleep(time.Second)
		}
		if err != nil {
			return fmt.Errorf("launchctl bootstrap: %v: %s", err, out)
		}
		time.Sleep(time.Second)
		if st, err := exec.Command("launchctl", "print", "gui/"+uid+"/"+service.Label).CombinedOutput(); err != nil || !strings.Contains(string(st), "state = running") {
			return fmt.Errorf("installed, but %s is not running; see %s", service.Label, filepath.Join(home.Logs(), "service.log"))
		}
		fmt.Printf("installed %s (%s): it arms simulators that boot without stuntcam and tells you when Xcode Run needs `stuntcam up`\n", service.Label, service.PlistPath())
		return nil
	case "uninstall":
		_ = exec.Command("launchctl", "bootout", "gui/"+uid+"/"+service.Label).Run()
		if err := os.Remove(service.PlistPath()); err != nil && !os.IsNotExist(err) {
			return err
		}
		fmt.Println("uninstalled", service.Label, "(simulators it armed in launchd mode stay armed until `stuntcam down` or a shutdown; its log stays in "+home.Logs()+")")
		return nil
	case "status":
		out, err := exec.Command("launchctl", "print", "gui/"+uid+"/"+service.Label).CombinedOutput()
		if err != nil {
			fmt.Println("not running")
			return nil
		}
		for _, l := range strings.Split(string(out), "\n") {
			if l = strings.TrimSpace(l); strings.HasPrefix(l, "state =") || strings.HasPrefix(l, "pid =") || strings.HasPrefix(l, "program =") || strings.HasPrefix(l, "last exit code =") {
				fmt.Println(l)
			}
		}
		return nil
	case "run":
		return serviceRun(*preboot, only)
	}
	return usagef("unknown service action %q (install, uninstall, status, run)", args[0])
}

func serviceRun(preboot string, only []string) error {
	set, err := artifacts.Find("")
	if err != nil {
		return err
	}
	if set, err = artifacts.Stage(set); err != nil {
		return err
	}
	log := func(f string, a ...any) {
		fmt.Printf("%s %s\n", time.Now().Format("2006-01-02 15:04:05"), fmt.Sprintf(f, a...))
	}
	preboot = strings.ToUpper(preboot)
	if preboot != "" && len(only) > 0 && !containsUpper(only, preboot) {
		log("preboot %s skipped: not one of --only", preboot)
	} else if preboot != "" {
		if _, err := os.Stat(arm.DownMarker(strings.ToUpper(preboot))); err == nil {
			log("preboot %s skipped: `stuntcam down` was run on it", preboot)
		} else if d, _, err := pickSim(preboot); err != nil {
			log("preboot %s: %v", preboot, err)
		} else if d.State != "Booted" {
			recordXcode(d.UDID, set.Rules, "service", false, true)
			err = arm.ArmBoot(ctl, d.UDID, set, arm.Vars(d.UDID, set, arm.Options{}))
			log("preboot %s: %v", d.Name, errText(err))
		}
	}
	handled := map[string]time.Time{}
	// leave alone: armed by stuntcam (boot or launchd), armed by the scripts ("armed"), being armed right now (a boot record
	// is written before the boot), and sims the user ran `stuntcam down` on (until their next `up`)
	armed := func(d simctl.Device) service.State {
		if _, err := os.Stat(arm.DownMarker(d.UDID)); err == nil {
			return service.IsArmed
		}
		switch statusOf(d).Mode {
		case "boot", "launchd", "armed":
			return service.IsArmed
		case "booting": // `up` or `exec` is arming it: look again next time, so a boot that fails is still covered
			return service.Pending
		}
		return service.Unarmed
	}
	log("watching simulators (build %s)", set.BuildID())
	for {
		ds, err := ctl.Devices()
		if err == nil {
			for _, d := range service.ToArm(ds, armed, handled, only...) {
				handled[d.UDID] = d.LastBooted
				recordXcode(d.UDID, set.Rules, "service", false, false)
				err := arm.ArmLaunchd(ctl, d.UDID, set, arm.Vars(d.UDID, set, arm.Options{}))
				log("%s (%s) booted unarmed: launchd mode %s", d.Name, d.UDID, map[bool]string{true: "armed", false: "failed: " + errText(err)}[err == nil])
				if err == nil {
					title, body := service.Notice(d)
					_ = exec.Command("osascript", "-e", fmt.Sprintf("display notification %q with title %q", body, title)).Run()
				}
			}
		}
		time.Sleep(2 * time.Second)
	}
}

func cmdGC(args []string) error {
	fs := flag.NewFlagSet("gc", flag.ContinueOnError)
	dry := fs.Bool("dry-run", false, "list what would be removed")
	keep := fs.Int("keep-builds", 3, "newest unreferenced builds to keep")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	rm := func(what, p string) {
		if *dry {
			fmt.Println("would remove", what, p)
			return
		}
		if err := os.RemoveAll(p); err == nil {
			fmt.Println("removed", what, p)
		}
	}
	priv := simctl.Ctl{Set: home.Devices()}
	// Per-simulator state and private devices are removed only when every listing succeeded and isn't empty: an empty list
	// from a timed-out or out-of-date simctl would make every simulator look deleted. Builds and old exec runs don't
	// depend on those lists and are cleaned anyway.
	rs, rerr := ctl.Runtimes()
	pubDevs, derr := ctl.Devices()
	privDevs, privErr := priv.Devices()
	if privErr != nil {
		if _, statErr := os.Stat(home.Devices()); statErr != nil {
			privDevs, privErr = nil, nil // no private set yet
		}
	}
	listsOK := rerr == nil && derr == nil && privErr == nil && len(rs) > 0 && len(pubDevs) > 0
	if !listsOK {
		why := "no simulators or runtimes listed"
		for _, e := range []error{rerr, derr, privErr} {
			if e != nil {
				why = e.Error()
			}
		}
		fmt.Printf("keeping every simulator's state and the private devices (%s)\n", why)
	}
	avail := map[string]bool{}
	for _, r := range rs {
		if r.Available {
			avail[r.Identifier] = true
		}
	}
	if listsOK {
		for _, d := range privDevs {
			if !avail[d.RuntimeID] {
				if *dry {
					fmt.Println("would delete private canary device", d.Name, d.UDID)
				} else if _, err := priv.Run(nil, "delete", d.UDID); err == nil {
					fmt.Println("deleted private canary device", d.Name, d.UDID)
				}
			}
		}
	}
	// per-sim state for simulators that no longer exist (default or private set)
	exists := map[string]bool{}
	for _, d := range append(append([]simctl.Device{}, pubDevs...), privDevs...) {
		exists[d.UDID] = true
	}
	sims, _ := os.ReadDir(filepath.Join(home.Root(), "sims"))
	refs := map[string]bool{}
	for _, s := range sims {
		if strings.HasPrefix(s.Name(), "exec-") {
			// an exec run's directory: simulators it booted read its current until they shut down; after 7 days, gone
			if fi, err := s.Info(); err == nil && time.Since(fi.ModTime()) > 7*24*time.Hour {
				rm("finished exec run", filepath.Join(home.Root(), "sims", s.Name()))
				continue
			}
		} else if listsOK && !exists[s.Name()] {
			rm("state of deleted simulator", filepath.Join(home.Root(), "sims", s.Name()))
			continue
		}
		if _, l, err := home.ReadCurrent(s.Name()); err == nil {
			refs[filepath.Base(filepath.Dir(l))] = true
		}
		if r, err := arm.ReadRecord(s.Name()); err == nil {
			refs[r.Build] = true
		}
	}
	currentCanary := ""
	if set, err := artifacts.Find(""); err == nil {
		refs[filepath.Base(filepath.Dir(set.Loader))], refs[filepath.Base(filepath.Dir(set.Stub))] = true, true
		if set.Hook != "" {
			refs[filepath.Base(filepath.Dir(set.Hook))] = true
		}
		if id := filepath.Dir(set.Canary); set.Canary != "" && filepath.Dir(id) == home.Canary() {
			currentCanary = filepath.Base(id)
		}
	}
	if env := service.InstalledEnv(); env != nil { // the login service's build: deleting it would leave the agent unable to start
		if b := env["STUNTCAM_BUILD"]; b != "" {
			refs[filepath.Base(b)] = true
		}
		if st := env["STUNTCAM_STUB"]; st != "" {
			refs[filepath.Base(filepath.Dir(st))] = true
		}
	}
	// builds: keep referenced ones, the stub dirs in use, and the newest few others
	bs, _ := os.ReadDir(home.Builds())
	mtime := func(e os.DirEntry) time.Time {
		if fi, err := e.Info(); err == nil {
			return fi.ModTime()
		}
		return time.Time{}
	}
	sort.Slice(bs, func(i, j int) bool { return mtime(bs[i]).After(mtime(bs[j])) })
	kept := 0
	for _, b := range bs {
		p := filepath.Join(home.Builds(), b.Name())
		if refs[b.Name()] {
			continue
		}
		if alwaysKeptBuild(p) {
			continue
		}
		if kept < *keep {
			kept++
			continue
		}
		rm("unused build", p)
	}
	gcCanaries(home.Canary(), currentCanary, 2, time.Now(), *dry, rm)
	gcStale(home.Rules(), time.Now(), rm) // StageRules' temp dirs
	return nil
}

// staleAge: a .stage-*/.old-* dir older than this is left over from a stuntcam that died mid-unpack (an unpack takes
// well under a second).
const staleAge = time.Hour

// gcCanaries removes the canaries unpacked under dir (<id>/m10canary.app, from installed packages) but the current one
// and the keep newest others (mtime: unpackTree touches the dir at each use), and stale temp dirs. It holds the unpack
// lock, so it never races an unpack. A removed canary comes back from its package's payload at that package's next use.
func gcCanaries(dir, current string, keep int, now time.Time, dry bool, rm func(what, p string)) {
	if _, err := os.Stat(dir); err != nil {
		return
	}
	if !dry {
		unlock, err := artifacts.LockDir(dir)
		if err != nil {
			fmt.Println("keeping the unpacked canaries:", err)
			return
		}
		defer unlock()
	}
	gcStale(dir, now, rm)
	es, _ := os.ReadDir(dir)
	type ent struct {
		name string
		mt   time.Time
	}
	var ids []ent
	for _, e := range es {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") || e.Name() == current {
			continue
		}
		if fi, err := e.Info(); err == nil {
			ids = append(ids, ent{e.Name(), fi.ModTime()})
		}
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i].mt.After(ids[j].mt) })
	for i, e := range ids {
		if i >= keep && now.Sub(e.mt) > 24*time.Hour { // untouched for a day: never one a concurrent verify just picked
			rm("old unpacked canary", filepath.Join(dir, e.name))
		}
	}
}

// gcStale removes .stage-* and .old-* entries in dir older than staleAge (younger ones may be an unpack in progress).
func gcStale(dir string, now time.Time, rm func(what, p string)) {
	es, _ := os.ReadDir(dir)
	for _, e := range es {
		if !strings.HasPrefix(e.Name(), ".stage-") && !strings.HasPrefix(e.Name(), ".old-") {
			continue
		}
		if fi, err := e.Info(); err == nil && now.Sub(fi.ModTime()) > staleAge {
			rm("stale temp dir", filepath.Join(dir, e.Name()))
		}
	}
}

// alwaysKeptBuild: a build dir `gc` never removes: a stub's (tiny, and a boot env may name any of them until that sim shuts
// down) or the runner hook's (tiny, and a running exec's test runners may name it).
func alwaysKeptBuild(dir string) bool {
	for _, n := range []string{artifacts.StubName, artifacts.HookName} {
		if _, err := os.Stat(filepath.Join(dir, n)); err == nil {
			return true
		}
	}
	return false
}

// destinationIDs returns the simulator UDIDs named by `-destination id=…` (or `…,id=…`) arguments.
func destinationIDs(args []string) []string {
	var out []string
	seen := map[string]bool{} // a destination named twice is booted and counted once
	for i := 0; i+1 < len(args); i++ {
		if args[i] != "-destination" {
			continue
		}
		for _, kv := range strings.Split(args[i+1], ",") {
			if k, v, ok := strings.Cut(strings.TrimSpace(kv), "="); ok && k == "id" && v != "" {
				if u := strings.ToUpper(v); !seen[u] { // simctl lists UDIDs in upper case; xcodebuild takes either
					seen[u] = true
					out = append(out, u)
				}
			}
		}
	}
	return out
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

func containsUpper(xs []string, x string) bool {
	for _, v := range xs {
		if strings.EqualFold(v, x) {
			return true
		}
	}
	return false
}

// activations counts camera activations per app bundle across sim directories.
func activations(dirs []string) map[string]int {
	out := map[string]int{}
	for _, dir := range dirs {
		for _, b := range state.Bundles(dir) {
			if a, err := state.Load(dir, b, ""); err == nil {
				out[b] += a.Activations
			}
		}
	}
	return out
}

// inForegroundOfTerminal reports whether this process's group is the foreground group of its controlling terminal: then a
// Ctrl-C there is delivered to the whole group (the child included) and must not be forwarded again. No terminal, or a
// background job: false.
func inForegroundOfTerminal() bool {
	t, err := os.Open("/dev/tty")
	if err != nil {
		return false
	}
	defer t.Close()
	var pg int32
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, t.Fd(), uintptr(syscall.TIOCGPGRP), uintptr(unsafe.Pointer(&pg))); e != 0 {
		return false
	}
	return int(pg) == syscall.Getpgrp()
}

// stageSource validates a source spec and copies an image or video where simulator processes can read it (not a
// privacy-protected folder), returning the spec to use.
func stageSource(spec string) (string, error) {
	if spec == "scene" {
		return spec, nil
	}
	kind, p, ok := strings.Cut(spec, ":")
	if !ok || (kind != "image" && kind != "video") || p == "" {
		return "", fmt.Errorf("source must be scene, image:PATH or video:PATH (got %q)", spec)
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", err
	}
	if fi, err := os.Stat(abs); err != nil || fi.IsDir() {
		return "", fmt.Errorf("%s: no such file", abs)
	}
	staged, err := artifacts.StageData(abs)
	if err != nil {
		return "", fmt.Errorf("staging %s: %w", abs, err)
	}
	return kind + ":" + staged, nil
}

// controllable says why live control (source/orient) would reach no app on the sim, or nil.
func controllable(d simctl.Device, key string) error {
	if _, _, err := home.ReadCurrent(d.UDID); err != nil {
		return fmt.Errorf("%s isn't armed: `stuntcam up` first", d.Name)
	}
	r, err := arm.ReadRecord(d.UDID)
	switch {
	case err != nil:
		return fmt.Errorf("%s wasn't armed by the stuntcam command (the scripts don't set up live control): `stuntcam up`", d.Name)
	case d.State != "Booted" && key == "source":
		return fmt.Errorf("%s isn't running; `stuntcam up --source …` sets the picture when it boots", d.Name)
	case d.State != "Booted":
		return fmt.Errorf("%s isn't running: `stuntcam up` boots it armed, then `stuntcam %s`", d.Name, map[string]string{"orientation": "orient"}[key])
	case r.Mode == arm.Boot && !arm.SameBoot(r, true, d.LastBooted):
		return fmt.Errorf("%s was rebooted without stuntcam since `up`: `stuntcam up --reboot` first", d.Name)
	case r.Mode != arm.Env && !containsStr(r.Vars, "SIMCAM_CONTROL"): // env's exports come from this binary
		return fmt.Errorf("%s was armed before live control existed: run `stuntcam up` again (add --reboot for a boot-mode simulator)", d.Name)
	case r.Mode == arm.Launchd:
		// the record keeps every name any arm set (so `down` unsets them all); an older stuntcam's `up` since then may have
		// unset this one, so ask the sim (otherwise source reported success while no app polled the file)
		v, err := ctl.Getenv(d.UDID, "SIMCAM_CONTROL")
		if err != nil {
			return fmt.Errorf("reading %s's launchd environment failed: %w", d.Name, err)
		}
		if v != home.ControlFile(d.UDID) {
			return fmt.Errorf("%s's launchd environment has no stuntcam control file (re-armed by an older stuntcam?): run `stuntcam up` again", d.Name)
		}
	}
	return nil
}

// setControl sets key=value in a simulator's control file (other lines kept), atomically. The sim must have been armed
// with the control file (stuntcam 0.1 arms without it: a re-arm is needed).
func setControl(d simctl.Device, key, value string) error {
	if err := controllable(d, key); err != nil {
		return err
	}
	return writeControl(d, key, value)
}

// writeControl is setControl without the checks, for a caller that has just run them (each launchd check is a
// `simctl spawn`, ~0.3 s).
func writeControl(d simctl.Device, key, value string) error {
	p := home.ControlFile(d.UDID)
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	// read-modify-write under a lock, so a concurrent source and orient don't lose one change
	lk, err := os.OpenFile(p+".lock", os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer lk.Close()
	if err := syscall.Flock(int(lk.Fd()), syscall.LOCK_EX); err != nil {
		return err
	}
	defer syscall.Flock(int(lk.Fd()), syscall.LOCK_UN)
	b, _ := os.ReadFile(p)
	var lines []string
	for _, l := range strings.Split(string(b), "\n") {
		if k, _, ok := strings.Cut(strings.TrimSpace(l), "="); ok && k != key {
			lines = append(lines, strings.TrimSpace(l))
		}
	}
	lines = append(lines, key+"="+value)
	return home.WriteAtomic(p, []byte(strings.Join(lines, "\n")+"\n"))
}

// uncheckedText groups bundle -> reason by reason: "simulator not booted" (it applies to every app), else "dev.a, dev.b: app
// not installed; …".
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

func sortedKeys(m map[string]string) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func containsStr(xs []string, x string) bool {
	for _, v := range xs {
		if v == x {
			return true
		}
	}
	return false
}

func cmdSource(args []string) error {
	fs := flag.NewFlagSet("source", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	if fs.NArg() != 1 {
		return usagef("usage: stuntcam source [--udid U] scene|image:PATH|video:PATH")
	}
	if a := fs.Arg(0); a != "scene" && !strings.HasPrefix(a, "image:") && !strings.HasPrefix(a, "video:") {
		return usagef("usage: stuntcam source [--udid U] scene|image:PATH|video:PATH")
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if err := controllable(d, "source"); err != nil { // before copying the file into data/
		return err
	}
	spec, err := stageSource(fs.Arg(0))
	if err != nil {
		return usageError{err}
	}
	if err := writeControl(d, "source", spec); err != nil {
		return err
	}
	fmt.Printf("%s: source %s (running apps switch within a second; apps launched later start with it)\n", d.Name, spec)
	return nil
}

func cmdOrient(args []string) error {
	fs := flag.NewFlagSet("orient", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	// UIDevice orientation names: landscapeLeft = the home button (or bottom edge) on the right
	valid := map[string]string{"portrait": "portrait", "upsidedown": "upsideDown", "portraitupsidedown": "upsideDown", "landscapeleft": "landscapeLeft", "landscaperight": "landscapeRight", "auto": "auto"}
	v, ok := "", false
	if fs.NArg() == 1 {
		v, ok = valid[strings.ToLower(fs.Arg(0))]
	}
	if !ok {
		return usagef("usage: stuntcam orient [--udid U] portrait|upsideDown|landscapeLeft|landscapeRight|auto")
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if err := setControl(d, "orientation", v); err != nil {
		return err
	}
	fmt.Printf("%s: orientation %s (apps follow within a second, if their UI allows it)\n", d.Name, v)
	return nil
}
