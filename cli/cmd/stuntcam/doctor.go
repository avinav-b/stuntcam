// SPDX-License-Identifier: Apache-2.0

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// doctorCheck is one line of `stuntcam doctor` (--json: one entry per line, in order).
type doctorCheck struct {
	Level string `json:"level"` // ok, warn, fail, info
	Msg   string `json:"msg"`
}

// doctorReport collects doctor's lines: printed as they come, or kept for --json. failed counts the failures.
type doctorReport struct {
	asJSON bool
	checks []doctorCheck
	failed int
}

func (r *doctorReport) emit(level, msg string) {
	r.checks = append(r.checks, doctorCheck{level, msg})
	if !r.asJSON {
		tag := map[string]string{"ok": "ok  ", "warn": "warn", "fail": "FAIL", "info": "info"}[level]
		fmt.Printf("%s %s\n", tag, msg)
	}
}

func (r *doctorReport) ok(msg string)   { r.emit("ok", msg) }
func (r *doctorReport) warn(msg string) { r.emit("warn", msg) }
func (r *doctorReport) info(msg string) { r.emit("info", msg) }

func (r *doctorReport) fail(msg string) {
	r.failed++
	r.emit("fail", msg)
}

// check is an ok line when passed holds, else a failure.
func (r *doctorReport) check(passed bool, msg string) {
	if passed {
		r.ok(msg)
	} else {
		r.fail(msg)
	}
}

// okOrWarn is an ok line when good holds, else a warning.
func (r *doctorReport) okOrWarn(good bool, msg string) {
	if good {
		r.ok(msg)
	} else {
		r.warn(msg)
	}
}

// detail is information shown as an indented line under the last check (text) or as an info entry (JSON).
func (r *doctorReport) detail(text, jsonMsg string) {
	r.checks = append(r.checks, doctorCheck{"info", jsonMsg})
	if !r.asJSON {
		fmt.Printf("     %s\n", text)
	}
}

func cmdDoctor(args []string) error {
	flags := flag.NewFlagSet("doctor", flag.ContinueOnError)
	asJSON := flags.Bool("json", false, "JSON output (for bug reports)")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	report := &doctorReport{asJSON: *asJSON}
	active, activeErr := xcode.Active()
	report.checkXcode(active, activeErr)
	report.checkRuntimes()
	report.checkHostDir()
	set, setErr := report.checkBuild()
	webcamDoctor(set, setErr, report)
	sims := report.checkSims(active, activeErr)

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
		}{1, version, commit, runtime.GOOS + "/" + runtime.GOARCH, active, report.checks, report.failed, sims}
		data, _ := json.MarshalIndent(out, "", "  ") // can't fail: strings, numbers and slices
		if _, err := fmt.Println(string(data)); err != nil {
			return err
		}
	}

	if report.failed > 0 {
		return fmt.Errorf("%d check(s) failed", report.failed)
	}

	return nil
}

// checkXcode reports the Xcode in use, the warnings about other Xcodes, and whether this build's restore rules cover
// it.
func (r *doctorReport) checkXcode(active xcode.Info, activeErr error) {
	r.check(activeErr == nil, fmt.Sprintf("Xcode in use: %s (%s) at %s%s", active.Version, active.Build,
		active.DeveloperDir, errText(activeErr)))
	for _, n := range xcodeWarnings(active, activeErr, installedXcodes(), os.Getenv("DEVELOPER_DIR"), runningXcodes()) {
		if n.Warn {
			r.warn(n.Msg)
		} else {
			r.info(n.Msg)
		}
	}

	if activeErr != nil {
		return
	}

	rulesDir := sourceRulesDir() // the rules `stuntcam up` would stage: this build's restore-rules/
	rulesFile, ok := xcode.Rules(rulesDir, active.Build)
	if ok {
		r.ok("restore rules for " + active.Build + ": " + rulesFile)
		return
	}

	guess := rulesFile
	if rulesFile == "" {
		guess = "the built-in 27A266a rules"
	}

	r.warn(xcode.UnverifiedLine(active.Build) + " (no usable " + active.Build + ".json in " + rulesDir +
		"; best guess " + guess + ")")
}

// checkRuntimes reports the available iOS simulator runtimes; having none is a failure.
func (r *doctorReport) checkRuntimes() {
	runtimes, err := ctl.Runtimes()
	if err != nil {
		r.fail("simctl: " + err.Error())
		return
	}

	var versions []string
	for _, simRuntime := range runtimes {
		if simRuntime.Available && simRuntime.Platform == "iOS" {
			versions = append(versions, simRuntime.Version+" ("+simRuntime.Build+")")
		}
	}

	r.check(len(versions) > 0, "iOS simulator runtimes: "+strings.Join(versions, ", "))
}

// checkHostDir checks that the host directory is readable by simulator processes and writable, and that the working
// directory isn't a privacy-protected one.
func (r *doctorReport) checkHostDir() {
	root := home.Root()
	r.check(!home.Protected(root), "host directory "+root+" (simulator processes must be able to read it)")
	if err := os.MkdirAll(root, 0o755); err == nil {
		f, err := os.CreateTemp(root, ".doctor")
		if err == nil {
			// the probe only checks that root is writable
			f.Close()
			os.Remove(f.Name())
		}

		r.check(err == nil, "host directory writable"+errText(err))
	}

	if wd, err := os.Getwd(); err == nil && home.Protected(wd) {
		r.warn("this directory (" + wd + ") is privacy-protected: fine for your project, but never pass paths inside " +
			"it to the simulator (stuntcam stages its files under " + root + ")")
	}
}

// checkBuild reports the build stuntcam would use (for an installed package, the result of its manifest check) and
// checks each dylib's ad-hoc linker signature. It returns the build, or why it couldn't be found.
func (r *doctorReport) checkBuild() (artifacts.Set, error) {
	set, err := artifacts.Find("")
	where := set.From
	if set.Installed && err == nil {
		anchor := "manifest anchored to this binary"
		if ok, _ := artifacts.Anchored(); !ok {
			anchor = "manifest not anchored: a dev build of the CLI"
		}

		where += fmt.Sprintf(" (installed package %s, build %s, %d shipped verdict(s); every file matches its "+
			"manifest, %s; payload unpacked under %s)", set.Version, set.BuildID(), len(set.ShippedVerdicts()), anchor,
			home.Root())
	}

	r.check(err == nil, "build: "+where+errText(err))
	if err != nil {
		return set, err
	}

	binaries := []string{set.Stub, set.Loader, set.Engine}
	if set.Hook != "" {
		binaries = append(binaries, set.Hook)
	}

	for _, binary := range binaries {
		// it exits non-zero for an unsigned binary, and its output says so
		out, _ := exec.Command("codesign", "-dv", binary).CombinedOutput()
		signature := string(out)
		signed := strings.Contains(signature, "adhoc") && strings.Contains(signature, "linker-signed")
		r.check(signed, filepath.Base(binary)+" ad-hoc linker-signed")
	}

	return set, nil
}

// checkSims reports each booted simulator's arming (anything but boot or launchd mode is a warning), its boot
// environment, its recorded Xcode and its latched apps. It returns their statuses for --json (empty, not nil, when
// none is booted).
func (r *doctorReport) checkSims(active xcode.Info, activeErr error) []simStatus {
	sims := []simStatus{}
	devices, err := ctl.Devices()
	if err != nil {
		r.fail("simctl: " + err.Error())
		return sims
	}

	for _, device := range devices {
		if device.State != simctl.Booted {
			continue
		}

		status := statusOf(device)
		sims = append(sims, status)
		armed := status.Mode == modeBoot || status.Mode == modeLaunchd
		r.okOrWarn(armed, fmt.Sprintf("%s (%s): %s%s", device.Name, runtimeName(device.RuntimeID), status.Mode,
			prefix(" — ", status.Note)))
		if status.BootEnv != "" {
			r.detail("boot environment (launchd_sim): "+status.BootEnv,
				device.Name+" boot environment (launchd_sim): "+status.BootEnv)
		}

		if armed {
			if warning := recordWarning(device, status, active, activeErr); warning != "" {
				r.warn(warning)
			}
		}

		for _, b := range sortedKeys(status.Latched) {
			r.warn(fmt.Sprintf("%s on %s latched (%s): stuntcam sentinel ls --udid %s", b, device.Name,
				status.Latched[b], device.UDID))
		}
	}

	return sims
}

// xcodeNote is one doctor line about Xcode: a warning, or information only.
type xcodeNote struct {
	Warn bool
	Msg  string
}

// xcodeWarnings lists what can make apps restore the wrong Xcode's libraries:
//   - DEVELOPER_DIR names another Xcode than xcode-select does;
//   - a running Xcode.app isn't the one in use;
//   - several Xcodes are installed. Restores follow the one recorded at `up`, and ⌘R from another isn't noticed
//     in-process.
//
// Several Xcodes alone is only information. It is a warning only together with one of the mismatches.
// Xcodes on /Volumes, such as disk images and backups, are left out unless one is the Xcode in use.
// A DEVELOPER_DIR that names the .app counts as its Contents/Developer.
func xcodeWarnings(active xcode.Info, activeErr error, installed []xcode.Info, devDir string,
	running []string) []xcodeNote {
	var notes []xcodeNote
	if devDir = xcode.NormalizeDeveloperDir(devDir); devDir != "" {
		if sel, err := selectedXcode(); err == nil && !xcode.Same(devDir, sel) {
			notes = append(notes, xcodeNote{true, "DEVELOPER_DIR=" + devDir + " differs from xcode-select -p (" +
				sel + "): `stuntcam up` here records DEVELOPER_DIR's Xcode, while Xcode.app's ⌘R is whichever " +
				"Xcode.app you opened"})
		}
	}
	if activeErr == nil {
		for _, app := range running {
			if a := xcode.App(active.DeveloperDir); a != "" && !xcode.Same(app, a) {
				notes = append(notes, xcodeNote{true, "running Xcode " + app + " is not the Xcode in use here (" +
					a + "): its ⌘R gets the restore rules of the Xcode recorded at `stuntcam up`"})
			}
		}
	}
	var apps []string
	for _, info := range installed {
		if strings.HasPrefix(info.DeveloperDir, "/Volumes/") &&
			!(activeErr == nil && xcode.Same(info.DeveloperDir, active.DeveloperDir)) {
			continue
		}
		apps = append(apps, fmt.Sprintf("%s (%s %s)", xcode.App(info.DeveloperDir), info.Version, info.Build))
	}
	if len(apps) > 1 {
		note := xcodeNote{len(notes) > 0, "several Xcodes installed: " + strings.Join(apps, ", ") + ". Restore rules " +
			"follow the one active at `stuntcam up` (DEVELOPER_DIR, else xcode-select); after ⌘R from another, run " +
			"`stuntcam up` from a shell where it is active"}
		notes = append([]xcodeNote{note}, notes...)
	}
	return notes
}

// recordWarning returns a warning for an armed sim whose Xcode record (config.json) is missing or names another Xcode
// than the one in use now, or whose staged restore rules lack that Xcode while this build has them; "" otherwise.
func recordWarning(device simctl.Device, status simStatus, active xcode.Info, activeErr error) string {
	config, err := arm.ReadConfig(device.UDID)
	// armed with another directory: its record is there
	if status.SimDir != "" && filepath.Clean(status.SimDir) != home.Sim(device.UDID) {
		var data []byte
		if data, err = os.ReadFile(filepath.Join(status.SimDir, "config.json")); err == nil {
			config = arm.Config{}
			err = json.Unmarshal(data, &config)
		}
	}

	switch {
	case err != nil:
		return fmt.Sprintf("%s: no Xcode recorded (armed by an older stuntcam or the scripts?): apps say \"restore "+
			"rules unverified\"; `stuntcam up --udid %s` records it", device.Name, device.UDID)
	case activeErr != nil:
		return ""
	case config.Build != active.Build ||
		!xcode.Same(xcode.NormalizeDeveloperDir(config.DeveloperDir), xcode.NormalizeDeveloperDir(active.DeveloperDir)):
		return fmt.Sprintf("%s was armed for Xcode %s (%s) but the Xcode in use is %s (%s): `stuntcam up --udid %s` "+
			"re-records it (no reboot)", device.Name, config.Build, config.DeveloperDir, active.Build,
			active.DeveloperDir, device.UDID)
	case !simRulesVerified(config) && sourceRulesVerified(config.Build):
		// the loader reads the rules staged when the sim was armed, not this build's
		return fmt.Sprintf("%s applies the restore rules staged when it was armed (%s), which have none for Xcode "+
			"%s; this build has them: `stuntcam up --udid %s` or `stuntcam swap --udid %s` stages them (no reboot)",
			device.Name, rulesDirName(config.RulesDir), config.Build, device.UDID, device.UDID)
	}

	return ""
}

// simRulesVerified reports whether the loader would find verified rules for the recorded build in the sim's own rules
// dir (a config without one gets the built-in rules, which are 27A266a's).
func simRulesVerified(c arm.Config) bool {
	if c.RulesDir == "" {
		return c.Build == builtinRulesBuild
	}
	_, ok := xcode.Rules(c.RulesDir, c.Build)
	return ok
}

// builtinRulesBuild is the Xcode build the loader's built-in rules were measured for (loader.c dflt).
const builtinRulesBuild = "27A266a"

// realSourceRulesDir is the rules `stuntcam up` would stage now: this build's restore-rules/. Called through the
// sourceRulesDir seam.
func realSourceRulesDir() string {
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

// runningXcodes lists the Xcode.app bundles of running Xcode processes (…/Xcode*.app/Contents/MacOS/Xcode).
func runningXcodes() []string {
	out, err := exec.Command("ps", "-axww", "-o", "args=").Output()
	if err != nil {
		return nil
	}
	return parseRunningXcodes(string(out))
}

// xcodeExecutable is the end of an Xcode bundle's name (".app") plus the path of its main executable inside the bundle,
// as it appears in the command line of a running Xcode.
const xcodeExecutable = ".app/Contents/MacOS/Xcode"

// parseRunningXcodes lists the Xcode bundles whose main executable runs in ps output (one command line per line), each
// once, in order. The executable's path must end the line or be followed by a space (its arguments), so that
// XcodeHelper and the other executables next to it don't count.
func parseRunningXcodes(psOutput string) []string {
	var apps []string
	seen := map[string]bool{}
	for _, line := range strings.Split(psOutput, "\n") {
		bundleDir, args, found := strings.Cut(line, xcodeExecutable)
		if !found || (args != "" && args[0] != ' ') {
			continue
		}

		app := strings.TrimSpace(bundleDir + ".app")
		if !seen[app] {
			seen[app] = true
			apps = append(apps, app)
		}
	}

	return apps
}
