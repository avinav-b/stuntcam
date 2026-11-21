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
	fs := flag.NewFlagSet("doctor", flag.ContinueOnError)
	asJSON := fs.Bool("json", false, "JSON output (for bug reports)")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	r := &doctorReport{asJSON: *asJSON}
	act, aerr := xcode.Active()
	r.checkXcode(act, aerr)
	r.checkRuntimes()
	r.checkHostDir()
	set, setErr := r.checkBuild()
	webcamDoctor(set, setErr, r)
	sims := r.checkSims(act, aerr)
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
		}{1, version, commit, runtime.GOOS + "/" + runtime.GOARCH, act, r.checks, r.failed, sims}
		b, _ := json.MarshalIndent(out, "", "  ")
		if _, err := fmt.Println(string(b)); err != nil {
			return err
		}
	}
	if r.failed > 0 {
		return fmt.Errorf("%d check(s) failed", r.failed)
	}
	return nil
}

// checkXcode: the Xcode in use, the warnings about other Xcodes, and whether this build's restore rules cover it.
func (r *doctorReport) checkXcode(act xcode.Info, aerr error) {
	r.check(aerr == nil, fmt.Sprintf("Xcode in use: %s (%s) at %s%s", act.Version, act.Build, act.DeveloperDir,
		errText(aerr)))
	for _, n := range xcodeWarnings(act, aerr, installedXcodes(), os.Getenv("DEVELOPER_DIR"), runningXcodes()) {
		if n.Warn {
			r.warn(n.Msg)
		} else {
			r.info(n.Msg)
		}
	}
	if aerr != nil {
		return
	}
	rulesDir := sourceRulesDir() // the rules `stuntcam up` would stage: this build's restore-rules/
	f, ok := xcode.Rules(rulesDir, act.Build)
	if ok {
		r.ok("restore rules for " + act.Build + ": " + f)
		return
	}
	guess := f
	if f == "" {
		guess = "the built-in 27A266a rules"
	}
	r.warn(xcode.UnverifiedLine(act.Build) + " (no usable " + act.Build + ".json in " + rulesDir + "; best guess " +
		guess + ")")
}

// checkRuntimes: the available iOS simulator runtimes (none is a failure).
func (r *doctorReport) checkRuntimes() {
	rs, err := ctl.Runtimes()
	if err != nil {
		r.fail("simctl: " + err.Error())
		return
	}
	var v []string
	for _, rt := range rs {
		if rt.Available && rt.Platform == "iOS" {
			v = append(v, rt.Version+" ("+rt.Build+")")
		}
	}
	r.check(len(v) > 0, "iOS simulator runtimes: "+strings.Join(v, ", "))
}

// checkHostDir: the host directory is readable by simulator processes and writable, and the working directory isn't a
// privacy-protected one.
func (r *doctorReport) checkHostDir() {
	root := home.Root()
	r.check(!home.Protected(root), "host directory "+root+" (simulator processes must be able to read it)")
	if err := os.MkdirAll(root, 0o755); err == nil {
		f, err := os.CreateTemp(root, ".doctor")
		if err == nil {
			f.Close()
			os.Remove(f.Name())
		}
		r.check(err == nil, "host directory writable"+errText(err))
	}
	if wd, err := os.Getwd(); err == nil && home.Protected(wd) {
		r.warn("this directory (" + wd + ") is privacy-protected: fine for your project, but never pass paths inside it " +
			"to the simulator (stuntcam stages its files under " + root + ")")
	}
}

// checkBuild: the build stuntcam would use (for an installed package, its manifest check) and each dylib's ad-hoc
// linker signature. It returns the build and why it couldn't be found.
func (r *doctorReport) checkBuild() (artifacts.Set, error) {
	set, err := artifacts.Find("")
	where := set.From
	if set.Installed && err == nil {
		anchor := "manifest anchored to this binary"
		if ok, _ := artifacts.Anchored(); !ok {
			anchor = "manifest not anchored: a dev build of the CLI"
		}
		where += fmt.Sprintf(" (installed package %s, build %s, %d shipped verdict(s); every file matches its manifest, %s; "+
			"payload unpacked under %s)", set.Version, set.BuildID(), len(set.ShippedVerdicts()), anchor, home.Root())
	}
	r.check(err == nil, "build: "+where+errText(err))
	if err != nil {
		return set, err
	}
	ps := []string{set.Stub, set.Loader, set.Engine}
	if set.Hook != "" {
		ps = append(ps, set.Hook)
	}
	for _, p := range ps {
		o, _ := exec.Command("codesign", "-dv", p).CombinedOutput()
		s := string(o)
		signed := strings.Contains(s, "adhoc") && strings.Contains(s, "linker-signed")
		r.check(signed, filepath.Base(p)+" ad-hoc linker-signed")
	}
	return set, nil
}

// checkSims: each booted simulator's arming (anything but boot or launchd mode is a warning), its boot environment, its
// recorded Xcode and its latched apps. It returns their statuses for --json (empty, not nil, without any).
func (r *doctorReport) checkSims(act xcode.Info, aerr error) []simStatus {
	sims := []simStatus{}
	ds, err := ctl.Devices()
	if err != nil {
		r.fail("simctl: " + err.Error())
		return sims
	}
	for _, d := range ds {
		if d.State != "Booted" {
			continue
		}
		s := statusOf(d)
		sims = append(sims, s)
		armed := s.Mode == modeBoot || s.Mode == modeLaunchd
		r.okOrWarn(armed, fmt.Sprintf("%s (%s): %s%s", d.Name, runtimeName(d.RuntimeID), s.Mode, prefix(" — ", s.Note)))
		if s.BootEnv != "" {
			r.detail("boot environment (launchd_sim): "+s.BootEnv, d.Name+" boot environment (launchd_sim): "+s.BootEnv)
		}
		if armed {
			if w := recordWarning(d, s, act, aerr); w != "" {
				r.warn(w)
			}
		}
		for _, b := range sortedKeys(s.Latched) {
			r.warn(fmt.Sprintf("%s on %s latched (%s): stuntcam sentinel ls --udid %s", b, d.Name, s.Latched[b], d.UDID))
		}
	}
	return sims
}

// xcodeNote is one doctor line about Xcode: a warning, or information only.
type xcodeNote struct {
	Warn bool
	Msg  string
}

// xcodeWarnings: DEVELOPER_DIR naming another Xcode than xcode-select, a running Xcode.app that isn't the one in use,
// and several Xcodes installed (restores follow the one recorded at `up`, and ⌘R from another isn't noticed
// in-process). Several Xcodes alone is information; a warning only with one of the mismatches. Xcodes on /Volumes (disk
// images, backups) are left out unless one is the Xcode in use. A DEVELOPER_DIR naming the .app counts as its
// Contents/Developer.
func xcodeWarnings(act xcode.Info, aerr error, installed []xcode.Info, devDir string, running []string) []xcodeNote {
	var w []xcodeNote
	if devDir = xcode.NormalizeDeveloperDir(devDir); devDir != "" {
		if sel, err := selectedXcode(); err == nil && !xcode.Same(devDir, sel) {
			w = append(w, xcodeNote{true, "DEVELOPER_DIR=" + devDir + " differs from xcode-select -p (" + sel + "): " +
				"`stuntcam up` here records DEVELOPER_DIR's Xcode, while Xcode.app's ⌘R is whichever Xcode.app you opened"})
		}
	}
	if aerr == nil {
		for _, app := range running {
			if a := xcode.App(act.DeveloperDir); a != "" && !xcode.Same(app, a) {
				w = append(w, xcodeNote{true, "running Xcode " + app + " is not the Xcode in use here (" + a + "): its ⌘R " +
					"gets the restore rules of the Xcode recorded at `stuntcam up`"})
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
		n := xcodeNote{len(w) > 0, "several Xcodes installed: " + strings.Join(l, ", ") + ". Restore rules follow the one " +
			"active at `stuntcam up` (DEVELOPER_DIR, else xcode-select); after ⌘R from another, run `stuntcam up` from a " +
			"shell where it is active"}
		w = append([]xcodeNote{n}, w...)
	}
	return w
}

// recordWarning: an armed sim whose recorded Xcode (config.json) is not the one in use now.
func recordWarning(d simctl.Device, s simStatus, act xcode.Info, aerr error) string {
	c, err := arm.ReadConfig(d.UDID)
	// armed with another directory: its record is there
	if s.SimDir != "" && filepath.Clean(s.SimDir) != home.Sim(d.UDID) {
		var b []byte
		if b, err = os.ReadFile(filepath.Join(s.SimDir, "config.json")); err == nil {
			c = arm.Config{}
			err = json.Unmarshal(b, &c)
		}
	}
	switch {
	case err != nil:
		return fmt.Sprintf("%s: no Xcode recorded (armed by an older stuntcam or the scripts?): apps say \"restore rules "+
			"unverified\"; `stuntcam up --udid %s` records it", d.Name, d.UDID)
	case aerr != nil:
		return ""
	case c.Build != act.Build ||
		!xcode.Same(xcode.NormalizeDeveloperDir(c.DeveloperDir), xcode.NormalizeDeveloperDir(act.DeveloperDir)):
		return fmt.Sprintf("%s was armed for Xcode %s (%s) but the Xcode in use is %s (%s): `stuntcam up --udid %s` "+
			"re-records it (no reboot)", d.Name, c.Build, c.DeveloperDir, act.Build, act.DeveloperDir, d.UDID)
	case !simRulesVerified(c) && sourceRulesVerified(c.Build):
		// the loader reads the rules staged when the sim was armed, not this build's
		return fmt.Sprintf("%s applies the restore rules staged when it was armed (%s), which have none for Xcode %s; this "+
			"build has them: `stuntcam up --udid %s` or `stuntcam swap --udid %s` stages them (no reboot)",
			d.Name, rulesDirName(c.RulesDir), c.Build, d.UDID, d.UDID)
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
