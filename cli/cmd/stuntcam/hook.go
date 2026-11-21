// SPDX-License-Identifier: Apache-2.0

package main

// exec's runner hook (design §4.2, §4.4, §4.5): on a destination that is launchd-armed or booted unarmed, Xcode's
// per-launch insert list replaces the stub's for a UI test's app under test, so `exec -- xcodebuild test` inserts
// libsimcamrunnerhook.dylib into the XCTest runner (TEST_RUNNER_DYLD_INSERT_LIBRARIES). The hook adds the stub and
// stuntcam's variables to every app the runner launches, and puts back what that variable displaced from the runner's
// own insert list (libXCTestBundleInject for hosted tests, which otherwise hang). What it adds per simulator comes from
// <run dir>/hook/<udid>.env, written here.

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/scheme"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// hookMode is what the runner hook does on a booted destination in this `status` mode: "boot" (inert: the boot env arms
// the app already), "launchd" (adds the stub; launchd gives the app the rest), "unarmed" (adds the stub and every
// variable), or "" (nothing to do: not booted, or being booted).
func hookMode(m statusMode) string {
	switch m {
	case modeBoot:
		return "boot"
	case modeLaunchd:
		return "launchd"
	case modeBootedUnarmed, modeEnv, modeScriptArmed:
		return "unarmed"
	}
	return ""
}

// hookEnvText is <udid>.env: "mode=<m>", then KEY=VALUE lines (the hook reads DYLD_INSERT_LIBRARIES as the stub to
// prepend).
func hookEnvText(mode string, vars []string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "mode=%s\n", mode)
	for _, kv := range vars {
		if !strings.ContainsAny(kv, "\n") {
			b.WriteString(kv + "\n")
		}
	}
	return b.String()
}

func writeHookEnv(dir, udid, mode string, vars []string) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, udid+".env"), []byte(hookEnvText(mode, vars)), 0o644)
}

// stubIn is the stub entry of an insert list ("" if none).
func stubIn(list, stubName string) string {
	for _, e := range strings.Split(list, ":") {
		if filepath.Base(e) == stubName {
			return e
		}
	}
	return ""
}

// runnerHookEnv is what exec exports for the hook: TEST_RUNNER_DYLD_INSERT_LIBRARIES (launch-only libraries first: the
// sanitizer runtimes in first unless the caller's own value already has that runtime, then the caller's; then the rest
// of the caller's value, then ours; no other stuntcam entry, no duplicate), the per-run hook dir, the stub for
// simulators exec couldn't name, the restore-rules file the hook reads its "test-runner" rule from ("" = its built-in
// copy), then extra.
func runnerHookEnv(callers, hook, dir, stub, rules string, first, extra []string) []string {
	var lead, rest []string
	for _, e := range strings.Split(callers, ":") {
		switch {
		case e == "" || strings.HasPrefix(filepath.Base(e), "libsimcam") || slices.Contains(lead, e) ||
			slices.Contains(rest, e):
		case launchOnly(e):
			lead = append(lead, e)
		default:
			rest = append(rest, e)
		}
	}
	var keep []string
	for _, f := range first {
		if !slices.Contains(keep, f) && !hasRuntime(lead, filepath.Base(f)) {
			keep = append(keep, f)
		}
	}
	keep = append(append(keep, lead...), rest...)
	env := []string{
		"TEST_RUNNER_DYLD_INSERT_LIBRARIES=" + strings.Join(append(keep, hook), ":"),
		"TEST_RUNNER_SIMCAM_HOOK_DIR=" + dir,
		"TEST_RUNNER_SIMCAM_HOOK_STUB=" + stub,
	}
	if rules != "" {
		env = append(env, "TEST_RUNNER_SIMCAM_HOOK_RULES="+rules)
	}
	return append(env, extra...)
}

// launchOnly: libraries that only work inserted at launch, ahead of the rest (as the hook and loader treat them).
func launchOnly(p string) bool {
	b := filepath.Base(p)
	return strings.HasPrefix(b, "libclang_rt.") || b == "libgmalloc.dylib"
}

// hasRuntime: list has a library of this sanitizer runtime's kind (libclang_rt.<san>_…, any platform suffix or copy).
func hasRuntime(list []string, runtime string) bool {
	san, _, _ := strings.Cut(strings.TrimPrefix(filepath.Base(runtime), "libclang_rt."), "_") // asan
	if san == "" {
		return false
	}
	for _, e := range list {
		if strings.HasPrefix(filepath.Base(e), "libclang_rt."+san+"_") {
			return true
		}
	}
	return false
}

// hookCount is what the runner hook logged on one simulator.
type hookCount struct {
	Armed    int
	Failures []string
}

// hookReport reads <dir>/<udid>.log for every simulator the hook ran on: app launches armed and failures (one line
// each).
func hookReport(dir string) map[string]hookCount {
	out := map[string]hookCount{}
	logs, _ := filepath.Glob(filepath.Join(dir, "*.log"))
	sort.Strings(logs)
	for _, l := range logs {
		b, err := os.ReadFile(l)
		if err != nil {
			continue
		}
		var c hookCount
		for _, line := range strings.Split(string(b), "\n") {
			_, msg, ok := strings.Cut(line, "] ")
			if !ok {
				continue
			}
			switch {
			case strings.HasPrefix(msg, "armed "):
				c.Armed++
			case strings.HasPrefix(msg, "failed "):
				c.Failures = append(c.Failures, msg)
			}
		}
		out[strings.TrimSuffix(filepath.Base(l), ".log")] = c
	}
	return out
}

// testConfigs is how the command's test runners run: from an -xctestrun file (each target of the selected test
// configurations), else the test action of the -scheme's .xcscheme under the -project/-workspace dir (or wd): its test
// plan (-testPlan, else the default; one entry per selected configuration) or, without plans, its own settings.
// -enableAddressSanitizer / -enableThreadSanitizer YES|NO override every entry. nil, "" with no error: nothing known
// (no scheme found). An error: a test plan or .xctestrun exists and can't be read.
func testConfigs(args []string, wd string) ([]scheme.TestConfig, string, error) {
	abs := func(p string) string {
		if p != "" && !filepath.IsAbs(p) {
			return filepath.Join(wd, p)
		}
		return p
	}
	only, skip := optValues(args, "-only-test-configuration"), optValues(args, "-skip-test-configuration")
	var cs []scheme.TestConfig
	src := ""
	xr := abs(lastOpt(args, "-xctestrun"))
	// test-without-building from an .xctestproducts
	if tp := abs(lastOpt(args, "-testProductsPath")); xr == "" && tp != "" {
		var m []string // Xcode 27 keeps it at <tp>/Tests/<n>/<scheme>.xctestrun; look a few levels down
		base := strings.Count(filepath.Clean(tp), string(filepath.Separator))
		filepath.WalkDir(tp, func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if d.IsDir() && strings.Count(p, string(filepath.Separator))-base > 3 {
				return filepath.SkipDir
			}
			if !d.IsDir() && strings.HasSuffix(p, ".xctestrun") {
				m = append(m, p)
			}
			return nil
		})
		if len(m) != 1 {
			return nil, tp, fmt.Errorf("%s: found %d .xctestrun files, can't tell which one runs", tp, len(m))
		}
		xr = m[0]
	}
	if xr != "" {
		var targets []string
		for _, t := range optValues(args, "-only-testing") {
			if n, _, _ := strings.Cut(t, "/"); !slices.Contains(targets, n) {
				targets = append(targets, n)
			}
		}
		var err error
		if cs, err = scheme.XCTestRun(xr, only, skip, targets); err != nil {
			return nil, xr, err
		}
		src = xr
	} else if name := lastOpt(args, "-scheme"); name != "" {
		p := scheme.Locate(wd, abs(lastOpt(args, "-workspace")), abs(lastOpt(args, "-project")), name)
		if p == "" { // unknown sanitizers: exporting the hook could drop a sanitizer runtime and hang the run
			return nil, "", fmt.Errorf("scheme %q not found where xcodebuild would look", name)
		}
		ta, err := scheme.ReadTestAction(p, lastOpt(args, "-testPlan"), only, skip)
		if err != nil {
			return nil, p, err
		}
		cs, src = ta.Configs, p
		if ta.Plan != "" {
			src = ta.Plan
		}
	}
	flags := map[string]string{"address": lastOpt(args, "-enableAddressSanitizer"),
		"thread": lastOpt(args, "-enableThreadSanitizer")}
	if len(cs) == 0 && (flags["address"] != "" || flags["thread"] != "") {
		cs = []scheme.TestConfig{{}}
	}
	for i := range cs {
		for _, sn := range []string{"address", "thread"} {
			switch v := flags[sn]; {
			case strings.EqualFold(v, "YES") && !slices.Contains(cs[i].Sanitizers, sn):
				cs[i].Sanitizers = append(cs[i].Sanitizers, sn)
			case strings.EqualFold(v, "NO"):
				cs[i].Sanitizers = without(cs[i].Sanitizers, sn)
			}
		}
		sort.Strings(cs[i].Sanitizers)
	}
	return cs, src, nil
}

func without(xs []string, x string) []string {
	var out []string
	for _, v := range xs {
		if v != x {
			out = append(out, v)
		}
	}
	return out
}

// hookInputs is everything planRunnerHook decides from; hookPlan is what it decided.
type hookInputs struct {
	Args         []string          // xcodebuild's own arguments
	WrapEnv      map[string]string // what the command's env/arch wrappers set
	WD           string
	Dest         destSet
	Hooked       []string // the destinations exec found launchd-armed or unarmed (names, for the messages)
	Hook         string   // the hook library ("" = this build has none)
	Dir, Stub    string
	Rules        string // <rules dir>/<xcode build>.json ("" = none)
	RulesOK      bool   // that file is usable and has a "test-runner" rule
	XcodeBuild   string
	DeveloperDir string
	Environ      []string // exec's own environment (the caller's)
}

type hookPlan struct {
	Export bool
	Env    []string // to add to the command's environment
	Notes  []string // stderr lines before the run
}

// planRunnerHook decides whether exec inserts its runner hook into the command's test runners and with what (design
// §4.5). Not when: no destination needs it; any destination isn't an iOS simulator; the Xcode in use has no verified
// test-runner restore rule (STUNTCAM_HOOK_FORCE=1 overrides); the command's own wrapper sets
// TEST_RUNNER_DYLD_INSERT_LIBRARIES; a test plan or .xctestrun can't be read; the runners would need different
// sanitizer runtimes (two sanitizers, or one only in some configurations: one variable can't give each runner its own);
// or the sanitizer runtime isn't in the Xcode in use. Each "not" is one warning naming what UI tests then miss.
func planRunnerHook(in hookInputs) hookPlan {
	if in.Hook == "" || (len(in.Hooked) == 0 && len(in.Dest.Named) == 0) {
		return hookPlan{}
	}
	pl := &hookPlanner{in: in}
	if why := pl.environmentProblem(); why != "" {
		return pl.decline(why)
	}
	cs, src, err := testConfigs(in.Args, in.WD)
	if err != nil {
		return pl.decline(fmt.Sprintf("can't read how the tests run (%v), so exec can't tell which sanitizer runtime the "+
			"runner needs", err))
	}
	sans, why := oneSanitizerSet(cs, src)
	if why != "" {
		return pl.decline(why)
	}
	callers, _ := pl.getenv("TEST_RUNNER_DYLD_INSERT_LIBRARIES")
	first, why := pl.sanitizerRuntimes(sans, callers)
	if why != "" {
		return pl.decline(why)
	}
	if len(first) > 0 {
		pl.notes = append(pl.notes, "stuntcam: sanitizer tests: the runner keeps "+strings.Join(first, ", "))
	}
	// what the scheme or plan inserts into the runner itself is replaced by the variable
	dropped, noMTC := runnerInserts(cs)
	if len(dropped) > 0 {
		pl.notes = append(pl.notes, fmt.Sprintf("stuntcam: %s inserts %s into the test runner; the runner hook's "+
			"TEST_RUNNER_DYLD_INSERT_LIBRARIES replaces that list, so the runner runs without it (the app under test keeps "+
			"its own)", src, strings.Join(dropped, ", ")))
	}
	extra := pl.runnerSettings(noMTC)
	rules := in.Rules
	if !in.RulesOK {
		rules = "" // forced: the hook's built-in list
	}
	env := runnerHookEnv(callers, in.Hook, in.Dir, in.Stub, rules, first, extra)
	p := hookPlan{Export: true, Env: env, Notes: pl.notes}
	for _, n := range in.Dest.Named {
		p.Notes = append(p.Notes, fmt.Sprintf("stuntcam: -destination %q names no simulator by id: if it is launchd-armed, "+
			"UI tests' apps there get the camera through the runner hook; booted unarmed, they don't (name it by id=, or "+
			"stuntcam up --udid <sim> first)", n))
	}
	return p
}

// hookPlanner is planRunnerHook's inputs and the notes it has made so far.
type hookPlanner struct {
	in    hookInputs
	notes []string
}

// getenv looks k up in exec's own environment.
func (pl *hookPlanner) getenv(k string) (string, bool) {
	for _, kv := range pl.in.Environ {
		if n, v, ok := strings.Cut(kv, "="); ok && n == k {
			return v, true
		}
	}
	return "", false
}

// decline is the plan without the hook: the notes so far and one warning naming why and the destinations it concerns.
func (pl *hookPlanner) decline(why string) hookPlan {
	where := strings.Join(append(append([]string{}, pl.in.Hooked...), pl.in.Dest.Named...), ", ")
	return hookPlan{Notes: append(pl.notes, fmt.Sprintf("stuntcam: %s: no runner hook, so UI tests' apps on %s get no "+
		"camera (stuntcam up --udid <sim> --reboot arms them at boot instead)", why, where))}
}

// environmentProblem is why the hook can't be used whatever the tests are ("" if none): a destination that isn't an
// iOS simulator, no verified test-runner rule for this Xcode (STUNTCAM_HOOK_FORCE=1 goes ahead, with a note), or a
// command wrapper that sets DEVELOPER_DIR to another Xcode or TEST_RUNNER_DYLD_INSERT_LIBRARIES itself.
func (pl *hookPlanner) environmentProblem() string {
	in := pl.in
	if len(in.Dest.Foreign) > 0 {
		return "the command also tests on " + strings.Join(in.Dest.Foreign, "; ") + ", and the runner hook is for iOS " +
			"simulators only"
	}
	if !in.RulesOK {
		if v, _ := pl.getenv("STUNTCAM_HOOK_FORCE"); v != "1" {
			what := "Xcode " + in.XcodeBuild
			if in.XcodeBuild == "" {
				what = "the Xcode in use (not identified)"
			}
			return fmt.Sprintf("%s has no verified test-runner restore rule (%s; STUNTCAM_HOOK_FORCE=1 uses the hook anyway, "+
				"with its built-in Xcode 27.0 list)", what, rulesDirName(in.Rules))
		}
		pl.notes = append(pl.notes, "stuntcam: STUNTCAM_HOOK_FORCE=1: runner hook used without a verified test-runner "+
			"restore rule for this Xcode")
	}
	dd, ok := in.WrapEnv["DEVELOPER_DIR"]
	if ok && xcode.NormalizeDeveloperDir(dd) != xcode.NormalizeDeveloperDir(in.DeveloperDir) {
		// the command runs another Xcode than the one exec identified: its rules and sanitizer runtimes are unknown
		return "the command sets DEVELOPER_DIR=" + dd + " (exec identified " + in.DeveloperDir + ")"
	}
	if _, ok := in.WrapEnv["TEST_RUNNER_DYLD_INSERT_LIBRARIES"]; ok {
		return "the command sets TEST_RUNNER_DYLD_INSERT_LIBRARIES itself (which would replace the hook's)"
	}
	return ""
}

// oneSanitizerSet is the sanitizers every test configuration runs with (sorted), or why there is no one set: one
// variable gives every runner the same insert list, so it can't give each runner its own runtime.
func oneSanitizerSet(cs []scheme.TestConfig, src string) (sans []string, why string) {
	same := true
	for i, c := range cs {
		for _, s := range c.Sanitizers {
			if !slices.Contains(sans, s) {
				sans = append(sans, s)
			}
		}
		if i > 0 && strings.Join(c.Sanitizers, ",") != strings.Join(cs[0].Sanitizers, ",") {
			same = false
		}
	}
	sort.Strings(sans)
	switch {
	case len(sans) > 1:
		return sans, fmt.Sprintf("the tests run with more than one sanitizer (%s; %s) and one "+
			"TEST_RUNNER_DYLD_INSERT_LIBRARIES can't give each runner its own runtime", configSanitizers(cs), src)
	case !same:
		return sans, fmt.Sprintf("only some test configurations run with the %s sanitizer (%s; %s) and one "+
			"TEST_RUNNER_DYLD_INSERT_LIBRARIES can't give only those runners its runtime", sans[0], configSanitizers(cs), src)
	}
	return sans, ""
}

// configSanitizers lists each test configuration's sanitizers: "ASan: address, TSan: thread, default: none".
func configSanitizers(cs []scheme.TestConfig) string {
	var ns []string
	for _, c := range cs {
		n, v := c.Name, strings.Join(c.Sanitizers, "+")
		if n == "" {
			n = "default"
		}
		if v == "" {
			v = "none"
		}
		ns = append(ns, n+": "+v)
	}
	return strings.Join(ns, ", ")
}

// sanitizerRuntimes is the Xcode's runtime for each sanitizer, to insert first, or why one is missing. A runtime the
// caller's own TEST_RUNNER_DYLD_INSERT_LIBRARIES (callers) already has needs none.
func (pl *hookPlanner) sanitizerRuntimes(sans []string, callers string) (first []string, why string) {
	for _, sn := range sans {
		r := sanitizerRuntime(pl.in.DeveloperDir, sn)
		if r == "" {
			kind := map[string]string{"address": "libclang_rt.asan_x", "thread": "libclang_rt.tsan_x"}[sn]
			if hasRuntime(strings.Split(callers, ":"), kind) {
				continue // the caller's own TEST_RUNNER_DYLD_INSERT_LIBRARIES has it
			}
			return nil, fmt.Sprintf("the tests run with the %s sanitizer and its runtime isn't in %s (the hook would drop "+
				"the runner's sanitizer runtime and hang the run)", sn, pl.in.DeveloperDir)
		}
		first = append(first, r)
	}
	return first, ""
}

// runnerInserts is what the test configurations insert into the runner itself (Guard Malloc, a test action's or plan's
// DYLD_INSERT_LIBRARIES), which the hook's variable replaces, and whether any turns the Main Thread Checker off.
func runnerInserts(cs []scheme.TestConfig) (dropped []string, noMTC bool) {
	for _, c := range cs {
		if c.GuardMalloc && !slices.Contains(dropped, "Guard Malloc (libgmalloc)") {
			dropped = append(dropped, "Guard Malloc (libgmalloc)")
		}
		for _, l := range c.Inserts {
			if !slices.Contains(dropped, l) {
				dropped = append(dropped, l)
			}
		}
		noMTC = noMTC || c.NoMTC
	}
	return dropped, noMTC
}

// runnerSettings is the TEST_RUNNER_ variables (sorted) that carry the caller's SIMCAM_* settings to the runner
// (xcodebuild passes on only TEST_RUNNER_ variables) unless the caller set the TEST_RUNNER_ one, and
// TEST_RUNNER_SIMCAM_MTC=0 when a scheme or plan turns the Main Thread Checker off (it stays off there).
func (pl *hookPlanner) runnerSettings(noMTC bool) []string {
	var extra []string
	set := map[string]bool{}
	for _, kv := range pl.in.Environ {
		k, v, _ := strings.Cut(kv, "=")
		if !strings.HasPrefix(k, "SIMCAM_") || set[k] {
			continue
		}
		switch k {
		case "SIMCAM_HOME", "SIMCAM_SIM_DIR", "SIMCAM_STATE_DIR", "SIMCAM_CHAIN", "SIMCAM_CONTROL", "SIMCAM_HOOK_DIR",
			"SIMCAM_HOOK_STUB", "SIMCAM_HOOK_RULES":
			continue // exec's own, or a sim's paths (the hook takes those from its .env file)
		}
		if _, ok := arm.LegacyNames[k]; ok {
			continue // a sim's camera settings (SIMCAM_PROFILE...): like its paths, from the .env file
		}
		if _, ok := pl.getenv("TEST_RUNNER_" + k); !ok {
			extra = append(extra, "TEST_RUNNER_"+k+"="+v)
			set[k] = true
		}
	}
	if _, ok := pl.getenv("TEST_RUNNER_SIMCAM_MTC"); noMTC && !ok && !set["SIMCAM_MTC"] {
		extra = append(extra, "TEST_RUNNER_SIMCAM_MTC=0")
	}
	sort.Strings(extra)
	return extra
}

// testRunnerRule reports whether rules file f is usable and has a "test-runner" rule (the list the hook restores).
func testRunnerRule(f string) bool {
	if f == "" || !xcode.Usable(f) {
		return false
	}
	b, err := os.ReadFile(f)
	if err != nil {
		return false
	}
	var r struct {
		Rules []struct {
			Name string          `json:"name"`
			Libs json.RawMessage `json:"libs"`
		} `json:"rules"`
	}
	if json.Unmarshal(b, &r) != nil {
		return false
	}
	for _, x := range r.Rules {
		libs := strings.TrimSpace(strings.Trim(strings.TrimSpace(string(x.Libs)), "[]"))
		if x.Name == "test-runner" && len(x.Libs) > 2 && libs != "" {
			return true
		}
	}
	return false
}

// sanitizerRuntime is the Xcode toolchain's simulator runtime for a sanitizer ("" if absent): the file Xcode copies
// into the test bundle. Inserted into the runner it does what Xcode's own insert of that copy did (checked with an ASan
// UI test on iOS 27.0).
func sanitizerRuntime(developerDir, s string) string {
	name := map[string]string{"address": "libclang_rt.asan_iossim_dynamic.dylib",
		"thread": "libclang_rt.tsan_iossim_dynamic.dylib"}[s]
	if name == "" || developerDir == "" {
		return ""
	}
	m, _ := filepath.Glob(filepath.Join(developerDir, "Toolchains", "XcodeDefault.xctoolchain", "usr", "lib", "clang", "*",
		"lib", "darwin", name))
	sort.Strings(m)
	for i := len(m) - 1; i >= 0; i-- { // the newest clang dir (one is usually a symlink to the other)
		if fi, err := os.Stat(m[i]); err == nil && fi.Mode().IsRegular() {
			return m[i]
		}
	}
	return ""
}
