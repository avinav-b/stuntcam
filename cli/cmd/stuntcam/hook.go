// SPDX-License-Identifier: Apache-2.0

package main

// exec's runner hook. On a destination that is launchd-armed or booted unarmed, Xcode's per-launch insert list replaces
// the stub's for a UI test's app under test, so `exec -- xcodebuild test` inserts libsimcamrunnerhook.dylib into the
// XCTest runner (TEST_RUNNER_DYLD_INSERT_LIBRARIES). The hook adds the stub and stuntcam's variables to every app the
// runner launches, and puts back what that variable displaced from the runner's own insert list (libXCTestBundleInject
// for hosted tests, which otherwise hang). What it adds per simulator comes from <run dir>/hook/<udid>.env, written
// here.

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

// runnerHookMode is what the runner hook does on a booted destination, as its hook file's mode= line says.
type runnerHookMode string

const (
	hookBoot    runnerHookMode = "boot"    // inert: the boot environment arms the app already
	hookLaunchd runnerHookMode = "launchd" // adds the stub; launchd gives the app the rest
	hookUnarmed runnerHookMode = "unarmed" // adds the stub and every variable
	hookNone    runnerHookMode = ""        // nothing to do: not booted, or being booted
)

// hookMode is the runner hook's mode on a booted destination in this `status` mode.
func hookMode(m statusMode) runnerHookMode {
	switch m {
	case modeBoot:
		return hookBoot
	case modeLaunchd:
		return hookLaunchd
	case modeBootedUnarmed, modeEnv, modeScriptArmed:
		return hookUnarmed
	}
	return hookNone
}

// hookEnvText is <udid>.env: "mode=<m>", then KEY=VALUE lines (the hook reads DYLD_INSERT_LIBRARIES as the stub to
// prepend).
func hookEnvText(mode runnerHookMode, vars []string) string {
	var text strings.Builder
	fmt.Fprintf(&text, "mode=%s\n", mode)
	for _, kv := range vars {
		if !strings.ContainsAny(kv, "\n") {
			text.WriteString(kv + "\n")
		}
	}
	return text.String()
}

func writeHookEnv(dir, udid string, mode runnerHookMode, vars []string) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, udid+".env"), []byte(hookEnvText(mode, vars)), 0o644)
}

// stubIn is the stub entry of an insert list ("" if none).
func stubIn(list, stubName string) string {
	for _, entry := range strings.Split(list, ":") {
		if filepath.Base(entry) == stubName {
			return entry
		}
	}
	return ""
}

// runnerHookEnv is what exec exports for the hook, in this order:
//   - TEST_RUNNER_DYLD_INSERT_LIBRARIES, described below;
//   - the per-run hook dir;
//   - the stub, for simulators exec couldn't name;
//   - the restore-rules file the hook reads its "test-runner" rule from. "" means its built-in copy;
//   - extra.
//
// TEST_RUNNER_DYLD_INSERT_LIBRARIES starts with the launch-only libraries: the sanitizer runtimes in first, unless the
// caller's own value already has that runtime. Then come the rest of the caller's value and then ours. It has no
// other stuntcam entry and no duplicate.
func runnerHookEnv(callers, hook, dir, stub, rules string, first, extra []string) []string {
	var lead, rest []string
	for _, entry := range strings.Split(callers, ":") {
		switch {
		case entry == "" || strings.HasPrefix(filepath.Base(entry), "libsimcam") || slices.Contains(lead, entry) ||
			slices.Contains(rest, entry):
		case launchOnly(entry):
			lead = append(lead, entry)
		default:
			rest = append(rest, entry)
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

// launchOnly reports whether a library only works inserted at launch, ahead of the rest (as the hook and the loader
// treat it).
func launchOnly(p string) bool {
	b := filepath.Base(p)
	return strings.HasPrefix(b, "libclang_rt.") || b == "libgmalloc.dylib"
}

// hasRuntime reports whether list has a library of this sanitizer runtime's kind (libclang_rt.<san>_…, with any
// platform suffix, or a copy).
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
	for _, logPath := range logs {
		data, err := os.ReadFile(logPath)
		if err != nil {
			continue
		}

		var count hookCount
		for _, line := range strings.Split(string(data), "\n") {
			_, msg, ok := strings.Cut(line, "] ")
			if !ok {
				continue
			}

			switch {
			case strings.HasPrefix(msg, "armed "):
				count.Armed++
			case strings.HasPrefix(msg, "failed "):
				count.Failures = append(count.Failures, msg)
			}
		}

		out[strings.TrimSuffix(filepath.Base(logPath), ".log")] = count
	}

	return out
}

// xctestrunSearchDepth is how many directory levels under -testProductsPath testConfigs looks for the .xctestrun.
const xctestrunSearchDepth = 3

// testConfigs is how the command's test runners run. It returns one entry per test configuration that runs, and the
// file it read them from.
//
// The configurations come from the first of these that applies:
//   - an -xctestrun file (or the one .xctestrun inside -testProductsPath): each target of the selected configurations;
//   - the test action of the -scheme's .xcscheme, found under the -project or -workspace dir, or under wd. That is
//     the selected configurations of its test plan (-testPlan, else the default plan), or its own settings when it
//     has no plans.
//
// -enableAddressSanitizer and -enableThreadSanitizer YES|NO then override every entry.
//
// With no scheme and no .xctestrun, it returns nil, "" and no error: nothing is known. A test plan or .xctestrun that
// exists but can't be read is an error, and so is a -scheme that can't be found.
func testConfigs(args []string, wd string) ([]scheme.TestConfig, string, error) {
	configs, src, err := selectedTestConfigs(args, wd)
	if err != nil {
		return nil, src, err
	}

	return applySanitizerOverrides(args, configs), src, nil
}

// absUnder resolves a relative path argument against wd, the directory xcodebuild runs in. "" stays "".
func absUnder(wd, p string) string {
	if p != "" && !filepath.IsAbs(p) {
		return filepath.Join(wd, p)
	}

	return p
}

// selectedTestConfigs is testConfigs before the sanitizer overrides.
func selectedTestConfigs(args []string, wd string) ([]scheme.TestConfig, string, error) {
	only := optValues(args, "-only-test-configuration")
	skip := optValues(args, "-skip-test-configuration")
	xctestrun := absUnder(wd, lastOpt(args, "-xctestrun"))
	productsPath := absUnder(wd, lastOpt(args, "-testProductsPath"))

	// test-without-building from an .xctestproducts
	if xctestrun == "" && productsPath != "" {
		found, err := findXCTestRun(productsPath)
		if err != nil {
			return nil, productsPath, err
		}

		xctestrun = found
	}

	if xctestrun != "" {
		configs, err := scheme.XCTestRun(xctestrun, only, skip, onlyTestingTargets(args))
		if err != nil {
			return nil, xctestrun, err
		}

		return configs, xctestrun, nil
	}

	name := lastOpt(args, "-scheme")
	if name == "" {
		return nil, "", nil
	}

	schemePath := scheme.Locate(wd, absUnder(wd, lastOpt(args, "-workspace")), absUnder(wd, lastOpt(args, "-project")),
		name)
	if schemePath == "" {
		// The sanitizers are unknown, and exporting the hook could drop a sanitizer runtime and hang the run.
		return nil, "", fmt.Errorf("scheme %q not found where xcodebuild would look", name)
	}

	action, err := scheme.ReadTestAction(schemePath, lastOpt(args, "-testPlan"), only, skip)
	if err != nil {
		return nil, schemePath, err
	}

	if action.Plan != "" {
		return action.Configs, action.Plan, nil
	}

	return action.Configs, schemePath, nil
}

// findXCTestRun returns the one .xctestrun file at most xctestrunSearchDepth levels under productsPath. Xcode 27 keeps
// it at <productsPath>/Tests/<n>/<scheme>.xctestrun. None, or more than one, is an error.
func findXCTestRun(productsPath string) (string, error) {
	var found []string
	base := strings.Count(filepath.Clean(productsPath), string(filepath.Separator))
	// WalkDir's result is dropped: the walk skips what it can't read and never fails
	filepath.WalkDir(productsPath, func(walked string, entry fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}

		if entry.IsDir() && strings.Count(walked, string(filepath.Separator))-base > xctestrunSearchDepth {
			return filepath.SkipDir
		}

		if !entry.IsDir() && strings.HasSuffix(walked, ".xctestrun") {
			found = append(found, walked)
		}

		return nil
	})

	if len(found) != 1 {
		return "", fmt.Errorf("%s: found %d .xctestrun files, can't tell which one runs", productsPath, len(found))
	}

	return found[0], nil
}

// onlyTestingTargets returns the targets that the -only-testing options name, each once, in order. An option is
// TARGET, TARGET/CLASS or TARGET/CLASS/METHOD.
func onlyTestingTargets(args []string) []string {
	var targets []string
	for _, testID := range optValues(args, "-only-testing") {
		if target, _, _ := strings.Cut(testID, "/"); !slices.Contains(targets, target) {
			targets = append(targets, target)
		}
	}

	return targets
}

// applySanitizerOverrides applies -enableAddressSanitizer and -enableThreadSanitizer YES|NO to every config. With no
// config known, an override still says how the run goes, so it gets one config of its own.
func applySanitizerOverrides(args []string, configs []scheme.TestConfig) []scheme.TestConfig {
	overrides := map[string]string{
		"address": lastOpt(args, "-enableAddressSanitizer"),
		"thread":  lastOpt(args, "-enableThreadSanitizer"),
	}
	if len(configs) == 0 && (overrides["address"] != "" || overrides["thread"] != "") {
		configs = []scheme.TestConfig{{}}
	}

	for i := range configs {
		for _, sanitizer := range []string{"address", "thread"} {
			switch v := overrides[sanitizer]; {
			case strings.EqualFold(v, "YES") && !slices.Contains(configs[i].Sanitizers, sanitizer):
				configs[i].Sanitizers = append(configs[i].Sanitizers, sanitizer)
			case strings.EqualFold(v, "NO"):
				configs[i].Sanitizers = without(configs[i].Sanitizers, sanitizer)
			}
		}

		sort.Strings(configs[i].Sanitizers)
	}

	return configs
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

// planRunnerHook decides whether exec inserts its runner hook into the command's test runners, and with what.
// It declines when:
//   - no destination needs it;
//   - a destination isn't an iOS simulator;
//   - the Xcode in use has no verified test-runner restore rule (STUNTCAM_HOOK_FORCE=1 overrides this);
//   - the command's own wrapper sets TEST_RUNNER_DYLD_INSERT_LIBRARIES;
//   - a test plan or .xctestrun can't be read;
//   - the runners would need different sanitizer runtimes: two sanitizers, or one in only some configurations. One
//     variable can't give each runner its own;
//   - the sanitizer runtime isn't in the Xcode in use.
//
// Each decline is one warning that names what UI tests then miss.
func planRunnerHook(inputs hookInputs) hookPlan {
	if inputs.Hook == "" || (len(inputs.Hooked) == 0 && len(inputs.Dest.Named) == 0) {
		return hookPlan{}
	}

	planner := &hookPlanner{inputs: inputs}
	if why := planner.environmentProblem(); why != "" {
		return planner.decline(why)
	}

	configs, src, err := testConfigs(inputs.Args, inputs.WD)
	if err != nil {
		return planner.decline(fmt.Sprintf("can't read how the tests run (%v), so exec can't tell which sanitizer "+
			"runtime the runner needs", err))
	}

	sans, problem := oneSanitizerSet(configs, src)
	if problem != "" {
		return planner.decline(problem)
	}

	callers, _ := planner.getenv("TEST_RUNNER_DYLD_INSERT_LIBRARIES")
	first, problem := planner.sanitizerRuntimes(sans, callers)
	if problem != "" {
		return planner.decline(problem)
	}

	if len(first) > 0 {
		planner.notes = append(planner.notes, "stuntcam: sanitizer tests: the runner keeps "+strings.Join(first, ", "))
	}

	// what the scheme or plan inserts into the runner itself is replaced by the variable
	dropped, noMTC := runnerInserts(configs)
	if len(dropped) > 0 {
		planner.notes = append(planner.notes, fmt.Sprintf("stuntcam: %s inserts %s into the test runner; the runner "+
			"hook's TEST_RUNNER_DYLD_INSERT_LIBRARIES replaces that list, so the runner runs without it (the app "+
			"under test keeps its own)", src, strings.Join(dropped, ", ")))
	}

	extra := planner.runnerSettings(noMTC)
	rules := inputs.Rules
	if !inputs.RulesOK {
		rules = "" // forced: the hook's built-in list
	}

	env := runnerHookEnv(callers, inputs.Hook, inputs.Dir, inputs.Stub, rules, first, extra)
	plan := hookPlan{Export: true, Env: env, Notes: planner.notes}
	for _, named := range inputs.Dest.Named {
		plan.Notes = append(plan.Notes, fmt.Sprintf("stuntcam: -destination %q names no simulator by id: if it is "+
			"launchd-armed, UI tests' apps there get the camera through the runner hook; booted unarmed, they don't "+
			"(name it by id=, or stuntcam up --udid <sim> first)", named))
	}

	return plan
}

// hookPlanner is planRunnerHook's inputs and the notes it has made so far.
type hookPlanner struct {
	inputs hookInputs
	notes  []string
}

// getenv looks key up in exec's own environment.
func (pl *hookPlanner) getenv(key string) (string, bool) {
	for _, kv := range pl.inputs.Environ {
		if name, value, ok := strings.Cut(kv, "="); ok && name == key {
			return value, true
		}
	}
	return "", false
}

// decline is the plan without the hook: the notes so far and one warning naming why and the destinations it concerns.
func (pl *hookPlanner) decline(why string) hookPlan {
	where := strings.Join(append(append([]string{}, pl.inputs.Hooked...), pl.inputs.Dest.Named...), ", ")
	return hookPlan{Notes: append(pl.notes, fmt.Sprintf("stuntcam: %s: no runner hook, so UI tests' apps on %s get no "+
		"camera (stuntcam up --udid <sim> --reboot arms them at boot instead)", why, where))}
}

// environmentProblem is why the hook can't be used whatever the tests are ("" if none): a destination that isn't an
// iOS simulator, no verified test-runner rule for this Xcode (STUNTCAM_HOOK_FORCE=1 goes ahead, with a note), or a
// command wrapper that sets DEVELOPER_DIR to another Xcode or TEST_RUNNER_DYLD_INSERT_LIBRARIES itself.
func (pl *hookPlanner) environmentProblem() string {
	inputs := pl.inputs
	if len(inputs.Dest.Foreign) > 0 {
		return "the command also tests on " + strings.Join(inputs.Dest.Foreign, "; ") + ", and the runner hook is " +
			"for iOS simulators only"
	}

	if !inputs.RulesOK {
		if v, _ := pl.getenv("STUNTCAM_HOOK_FORCE"); v != "1" {
			what := "Xcode " + inputs.XcodeBuild
			if inputs.XcodeBuild == "" {
				what = "the Xcode in use (not identified)"
			}

			return fmt.Sprintf("%s has no verified test-runner restore rule (%s; STUNTCAM_HOOK_FORCE=1 uses the hook "+
				"anyway, with its built-in Xcode 27.0 list)", what, rulesDirName(inputs.Rules))
		}

		pl.notes = append(pl.notes, "stuntcam: STUNTCAM_HOOK_FORCE=1: runner hook used without a verified test-runner "+
			"restore rule for this Xcode")
	}

	developerDir, ok := inputs.WrapEnv["DEVELOPER_DIR"]
	if ok && xcode.NormalizeDeveloperDir(developerDir) != xcode.NormalizeDeveloperDir(inputs.DeveloperDir) {
		// the command runs another Xcode than the one exec identified: its rules and sanitizer runtimes are unknown
		return "the command sets DEVELOPER_DIR=" + developerDir + " (exec identified " + inputs.DeveloperDir + ")"
	}

	if _, ok := inputs.WrapEnv["TEST_RUNNER_DYLD_INSERT_LIBRARIES"]; ok {
		return "the command sets TEST_RUNNER_DYLD_INSERT_LIBRARIES itself (which would replace the hook's)"
	}

	return ""
}

// oneSanitizerSet is the sanitizers every test configuration runs with (sorted), or why there is no one set: one
// variable gives every runner the same insert list, so it can't give each runner its own runtime.
func oneSanitizerSet(configs []scheme.TestConfig, src string) (sans []string, problem string) {
	same := true
	for i, config := range configs {
		for _, sanitizer := range config.Sanitizers {
			if !slices.Contains(sans, sanitizer) {
				sans = append(sans, sanitizer)
			}
		}

		if i > 0 && strings.Join(config.Sanitizers, ",") != strings.Join(configs[0].Sanitizers, ",") {
			same = false
		}
	}

	sort.Strings(sans)
	switch {
	case len(sans) > 1:
		return sans, fmt.Sprintf("the tests run with more than one sanitizer (%s; %s) and one "+
			"TEST_RUNNER_DYLD_INSERT_LIBRARIES can't give each runner its own runtime", configSanitizers(configs), src)
	case !same:
		return sans, fmt.Sprintf("only some test configurations run with the %s sanitizer (%s; %s) and one "+
			"TEST_RUNNER_DYLD_INSERT_LIBRARIES can't give only those runners its runtime", sans[0],
			configSanitizers(configs), src)
	}

	return sans, ""
}

// configSanitizers lists each test configuration's sanitizers: "ASan: address, TSan: thread, default: none".
func configSanitizers(configs []scheme.TestConfig) string {
	var entries []string
	for _, config := range configs {
		name, list := config.Name, strings.Join(config.Sanitizers, "+")
		if name == "" {
			name = "default"
		}

		if list == "" {
			list = "none"
		}

		entries = append(entries, name+": "+list)
	}

	return strings.Join(entries, ", ")
}

// sanitizerRuntimes is the Xcode's runtime for each sanitizer, to insert first, or why one is missing. A runtime the
// caller's own TEST_RUNNER_DYLD_INSERT_LIBRARIES (callers) already has needs none.
func (pl *hookPlanner) sanitizerRuntimes(sans []string, callers string) (first []string, problem string) {
	for _, sanitizer := range sans {
		runtime := sanitizerRuntime(pl.inputs.DeveloperDir, sanitizer)
		if runtime == "" {
			kind := map[string]string{"address": "libclang_rt.asan_x", "thread": "libclang_rt.tsan_x"}[sanitizer]
			if hasRuntime(strings.Split(callers, ":"), kind) {
				continue // the caller's own TEST_RUNNER_DYLD_INSERT_LIBRARIES has it
			}

			return nil, fmt.Sprintf("the tests run with the %s sanitizer and its runtime isn't in %s (the hook would "+
				"drop the runner's sanitizer runtime and hang the run)", sanitizer, pl.inputs.DeveloperDir)
		}

		first = append(first, runtime)
	}

	return first, ""
}

// runnerInserts is what the test configurations insert into the runner itself (Guard Malloc, a test action's or plan's
// DYLD_INSERT_LIBRARIES), which the hook's variable replaces, and whether any turns the Main Thread Checker off.
func runnerInserts(configs []scheme.TestConfig) (dropped []string, noMTC bool) {
	for _, config := range configs {
		if config.GuardMalloc && !slices.Contains(dropped, "Guard Malloc (libgmalloc)") {
			dropped = append(dropped, "Guard Malloc (libgmalloc)")
		}

		for _, insert := range config.Inserts {
			if !slices.Contains(dropped, insert) {
				dropped = append(dropped, insert)
			}
		}

		noMTC = noMTC || config.NoMTC
	}

	return dropped, noMTC
}

// runnerSettings returns sorted TEST_RUNNER_ variables for the runner. xcodebuild passes on only TEST_RUNNER_
// variables.
//   - Each of the caller's SIMCAM_* settings gets one, unless the caller set the TEST_RUNNER_ one itself.
//   - TEST_RUNNER_SIMCAM_MTC=0 is added when a scheme or plan turns the Main Thread Checker off, so it stays off.
func (pl *hookPlanner) runnerSettings(noMTC bool) []string {
	var extra []string
	set := map[string]bool{}
	for _, kv := range pl.inputs.Environ {
		key, value, _ := strings.Cut(kv, "=")
		if !strings.HasPrefix(key, "SIMCAM_") || set[key] {
			continue
		}

		switch key {
		case "SIMCAM_HOME", "SIMCAM_SIM_DIR", "SIMCAM_STATE_DIR", "SIMCAM_CHAIN", "SIMCAM_CONTROL", "SIMCAM_HOOK_DIR",
			"SIMCAM_HOOK_STUB", "SIMCAM_HOOK_RULES":
			continue // exec's own, or a sim's paths (the hook takes those from its .env file)
		}

		if _, ok := arm.LegacyNames[key]; ok {
			continue // a sim's camera settings (SIMCAM_PROFILE...): like its paths, from the .env file
		}

		if _, ok := pl.getenv("TEST_RUNNER_" + key); !ok {
			extra = append(extra, "TEST_RUNNER_"+key+"="+value)
			set[key] = true
		}
	}

	if _, ok := pl.getenv("TEST_RUNNER_SIMCAM_MTC"); noMTC && !ok && !set["SIMCAM_MTC"] {
		extra = append(extra, "TEST_RUNNER_SIMCAM_MTC=0")
	}

	sort.Strings(extra)
	return extra
}

// testRunnerRule reports whether the rules file at rulesPath is usable and has a "test-runner" rule (the list the hook
// restores).
func testRunnerRule(rulesPath string) bool {
	if rulesPath == "" || !xcode.Usable(rulesPath) {
		return false
	}

	data, err := os.ReadFile(rulesPath)
	if err != nil {
		return false
	}

	var parsed struct {
		Rules []struct {
			Name string          `json:"name"`
			Libs json.RawMessage `json:"libs"`
		} `json:"rules"`
	}

	if json.Unmarshal(data, &parsed) != nil {
		return false
	}

	for _, rule := range parsed.Rules {
		libs := strings.TrimSpace(strings.Trim(strings.TrimSpace(string(rule.Libs)), "[]"))
		if rule.Name == "test-runner" && len(rule.Libs) > 2 && libs != "" {
			return true
		}
	}

	return false
}

// sanitizerRuntime is the Xcode toolchain's simulator runtime for a sanitizer ("" if absent): the file Xcode copies
// into the test bundle. Inserted into the runner it does what Xcode's own insert of that copy did (checked with an ASan
// UI test on iOS 27.0).
func sanitizerRuntime(developerDir, sanitizer string) string {
	name := map[string]string{"address": "libclang_rt.asan_iossim_dynamic.dylib",
		"thread": "libclang_rt.tsan_iossim_dynamic.dylib"}[sanitizer]
	if name == "" || developerDir == "" {
		return ""
	}

	matches, _ := filepath.Glob(filepath.Join(developerDir, "Toolchains", "XcodeDefault.xctoolchain", "usr", "lib",
		"clang", "*", "lib", "darwin", name))
	sort.Strings(matches)
	for i := len(matches) - 1; i >= 0; i-- { // the newest clang dir (one is usually a symlink to the other)
		if fi, err := os.Stat(matches[i]); err == nil && fi.Mode().IsRegular() {
			return matches[i]
		}
	}

	return ""
}
