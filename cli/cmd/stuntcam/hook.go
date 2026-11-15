// SPDX-License-Identifier: Apache-2.0

package main

// exec's runner hook (design §4.2, §4.4, §4.5): on a destination that is launchd-armed or booted unarmed, Xcode's
// per-launch insert list replaces the stub's for a UI test's app under test, so `exec -- xcodebuild test`
// inserts libsimcamrunnerhook.dylib into the XCTest runner (TEST_RUNNER_DYLD_INSERT_LIBRARIES). The hook adds the stub and
// stuntcam's variables to every app the runner launches, and puts back what that variable displaced from the runner's own
// insert list (libXCTestBundleInject for hosted tests, which otherwise hang). What it adds per simulator comes from
// <run dir>/hook/<udid>.env, written here.

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/scheme"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// xcodebuildValueOpts are xcodebuild options that take the next argument as their value (so a value such as `-scheme test`
// is not taken for the test action). Options not listed are taken as flags.
var xcodebuildValueOpts = map[string]bool{}

func init() {
	for _, o := range strings.Fields(`-project -workspace -scheme -target -configuration -arch -sdk -destination
		-destination-timeout -derivedDataPath -resultBundlePath -resultBundleVersion -resultStreamPath -xctestrun -testPlan
		-only-testing -skip-testing -only-test-configuration -skip-test-configuration -testLanguage -testRegion
		-enableAddressSanitizer -enableThreadSanitizer -enableUndefinedBehaviorSanitizer -enableCodeCoverage
		-test-iterations -test-timeouts-enabled -default-test-execution-time-allowance -maximum-test-execution-time-allowance
		-parallel-testing-enabled -parallel-testing-worker-count -maximum-parallel-testing-workers
		-maximum-concurrent-test-device-destinations -maximum-concurrent-test-simulator-destinations
		-test-repetition-relaunch-enabled -collect-test-diagnostics -testProductsPath -xcconfig -toolchain -jobs
		-archivePath -exportPath -exportOptionsPlist -clonedSourcePackagesDirPath -packageCachePath
		-authenticationKeyPath -authenticationKeyID -authenticationKeyIssuerID -localizationPath -exportLanguage
		-defaultPackageRegistryURL -packageDependencySchedulerPolicy -scmProvider -test-enumeration-style
		-test-enumeration-format -test-enumeration-output-path -runFirstLaunch -resultBundleFormat`) {
		xcodebuildValueOpts[o] = true
	}
}

// xbCommand is the xcodebuild inside a command line: its own arguments, after any env / arch / xcrun wrappers, and the
// variables those wrappers set.
type xbCommand struct {
	OK      bool              // the command runs xcodebuild
	Args    []string          // xcodebuild's arguments (without "xcodebuild")
	Env     map[string]string // NAME=VALUE the wrappers set (env, arch -e)
	Cleared bool              // a wrapper starts xcodebuild with a cleared environment (env -i, arch -c): no TEST_RUNNER_ variable reaches it
}

// parseXcodebuild finds xcodebuild in cmdline, through `env [-i] [-u N] [N=V…]`, `arch [-arm64|-x86_64|-arch A] [-e N=V]
// [-c]` and `xcrun [options]` wrappers (in any order, nested). A script that runs xcodebuild inside isn't recognized.
func parseXcodebuild(cmdline []string) xbCommand {
	c := xbCommand{Env: map[string]string{}}
	args := cmdline
	for len(args) > 0 {
		switch filepath.Base(args[0]) {
		case "env":
			args = args[1:]
		envOpts:
			for len(args) > 0 {
				a := args[0]
				switch {
				case a == "-i" || a == "-" || a == "--ignore-environment":
					c.Cleared = true
				case (a == "-u" || a == "-P" || a == "-C" || a == "-S") && len(args) > 1:
					args = args[1:]
				case strings.HasPrefix(a, "-") && len(a) > 1:
				case strings.Contains(a, "=") && !strings.HasPrefix(a, "="):
					k, v, _ := strings.Cut(a, "=")
					c.Env[k] = v
				default:
					break envOpts
				}
				args = args[1:]
			}
		case "arch":
			args = args[1:]
			for len(args) > 0 && strings.HasPrefix(args[0], "-") {
				switch args[0] {
				case "-c":
					c.Cleared = true
				case "-arch", "-d":
					if len(args) > 1 {
						args = args[1:]
					}
				case "-e":
					if len(args) > 1 {
						k, v, _ := strings.Cut(args[1], "=")
						c.Env[k] = v
						args = args[1:]
					}
				}
				args = args[1:]
			}
		case "xcrun":
			args = args[1:]
			for len(args) > 0 && strings.HasPrefix(args[0], "-") { // xcrun's own options: --sdk X, --toolchain X, -v, …
				if (args[0] == "--sdk" || args[0] == "-sdk" || args[0] == "--toolchain" || args[0] == "-toolchain") && len(args) > 1 {
					args = args[1:]
				}
				args = args[1:]
			}
		case "xcodebuild":
			c.OK, c.Args = true, args[1:]
			return c
		default:
			return c
		}
	}
	return c
}

// actions are xcodebuild's action words: the arguments that are neither an option, an option's value nor a NAME=VALUE
// build setting.
func (c xbCommand) actions() []string {
	var out []string
	for i := 0; i < len(c.Args); i++ {
		a := c.Args[i]
		switch {
		case strings.HasPrefix(a, "-"):
			if xcodebuildValueOpts[a] && i+1 < len(c.Args) {
				i++
			}
		case strings.Contains(a, "="):
		default:
			out = append(out, a)
		}
	}
	return out
}

func (c xbCommand) test() bool {
	for _, a := range c.actions() {
		if a == "test" || a == "test-without-building" {
			return true
		}
	}
	return false
}

// xcodebuildTest reports whether cmdline runs xcodebuild's test or test-without-building action itself (directly or
// through env/arch/xcrun) with an environment the TEST_RUNNER_ variables reach.
func xcodebuildTest(cmdline []string) bool {
	c := parseXcodebuild(cmdline)
	return c.OK && !c.Cleared && c.test()
}

// optValues is the values of every `opt VALUE` (and `opt:VALUE`) in args.
func optValues(args []string, opt string) []string {
	var out []string
	for i := 0; i < len(args); i++ {
		if args[i] == opt && i+1 < len(args) {
			out = append(out, args[i+1])
			i++
		} else if v, ok := strings.CutPrefix(args[i], opt+":"); ok {
			out = append(out, v)
		}
	}
	return out
}

func lastOpt(args []string, opt string) string {
	v := optValues(args, opt)
	if len(v) == 0 {
		return ""
	}
	return v[len(v)-1]
}

// isIOSRuntime: a simulator runtime identifier of iOS (not tvOS, watchOS, visionOS).
func isIOSRuntime(id string) bool {
	return strings.HasPrefix(id, "com.apple.CoreSimulator.SimRuntime.iOS-")
}

// destinations sorts the command's -destination arguments (design §4.5): IOS, iOS simulators named by
// id= that simctl lists (UDIDs upper case); Named, `platform=iOS Simulator` destinations with no id (exec can't tell their
// arming in advance; the hook still covers a launchd-armed one from the runner's own environment); Foreign, everything else
// (another platform's simulator, a physical device, macOS / Mac Catalyst, an id simctl doesn't list): the runner hook is an
// iOS-simulator library, so exec doesn't use it at all then. generic/ destinations (build only) are ignored.
type destSet struct {
	IOS, Named, Foreign []string
}

func destinations(args []string, devs []simctl.Device) destSet {
	var out destSet
	byID := map[string]simctl.Device{}
	for _, d := range devs {
		byID[strings.ToUpper(d.UDID)] = d
	}
	for _, spec := range optValues(args, "-destination") {
		kv := map[string]string{}
		for _, p := range strings.Split(spec, ",") {
			if k, v, ok := strings.Cut(strings.TrimSpace(p), "="); ok {
				kv[strings.TrimSpace(k)] = strings.TrimSpace(v)
			}
		}
		if _, ok := kv["generic/platform"]; ok {
			continue
		}
		switch id := strings.ToUpper(kv["id"]); {
		case id != "":
			d, ok := byID[id]
			switch {
			case !ok:
				out.Foreign = append(out.Foreign, spec+" (not a simulator simctl lists)")
			case !isIOSRuntime(d.RuntimeID):
				out.Foreign = append(out.Foreign, fmt.Sprintf("%s (%s, %s)", spec, d.Name, runtimeName(d.RuntimeID)))
			case !contains(out.IOS, id):
				out.IOS = append(out.IOS, id)
			}
		case strings.EqualFold(kv["platform"], "iOS Simulator"):
			out.Named = append(out.Named, spec)
		default:
			out.Foreign = append(out.Foreign, spec)
		}
	}
	return out
}

// hookMode is what the runner hook does on a booted destination in this `status` mode: "boot" (inert: the boot env arms
// the app already), "launchd" (adds the stub; launchd gives the app the rest), "unarmed" (adds the stub and every
// variable), or "" (nothing to do: not booted, or being booted).
func hookMode(statusMode string) string {
	switch statusMode {
	case "boot":
		return "boot"
	case "launchd":
		return "launchd"
	case "booted-unarmed", "env", "armed":
		return "unarmed"
	}
	return ""
}

// hookEnvText is <udid>.env: "mode=<m>", then KEY=VALUE lines (the hook reads DYLD_INSERT_LIBRARIES as the stub to prepend).
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
// sanitizer runtimes in first unless the caller's own value already has that runtime, then the caller's; then the rest of
// the caller's value, then ours; no other stuntcam entry, no duplicate), the per-run hook dir, the stub for simulators exec
// couldn't name, the restore-rules file the hook reads its "test-runner" rule from ("" = its built-in copy), then extra.
func runnerHookEnv(callers, hook, dir, stub, rules string, first, extra []string) []string {
	var lead, rest []string
	for _, e := range strings.Split(callers, ":") {
		switch {
		case e == "" || strings.HasPrefix(filepath.Base(e), "libsimcam") || contains(lead, e) || contains(rest, e):
		case launchOnly(e):
			lead = append(lead, e)
		default:
			rest = append(rest, e)
		}
	}
	var keep []string
	for _, f := range first {
		if !contains(keep, f) && !hasRuntime(lead, filepath.Base(f)) {
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

// hookReport reads <dir>/<udid>.log for every simulator the hook ran on: app launches armed and failures (one line each).
type hookCount struct {
	Armed    int
	Failures []string
}

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

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }

// sortedHookUDIDs is hookReport's keys in order (for a stable summary).
func sortedHookUDIDs(m map[string]hookCount) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// testConfigs is how the command's test runners run: from an -xctestrun file (each target of the selected test
// configurations), else the test action of the -scheme's .xcscheme under the -project/-workspace dir (or wd): its test plan
// (-testPlan, else the default; one entry per selected configuration) or, without plans, its own settings.
// -enableAddressSanitizer / -enableThreadSanitizer YES|NO override every entry. nil, "" with no error: nothing known (no
// scheme found). An error: a test plan or .xctestrun exists and can't be read.
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
	if tp := abs(lastOpt(args, "-testProductsPath")); xr == "" && tp != "" { // test-without-building from an .xctestproducts
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
			if n, _, _ := strings.Cut(t, "/"); !contains(targets, n) {
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
		if p == "" { // unknown sanitizers: exporting the hook could drop a sanitizer runtime and hang the run (r2 M-1)
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
	flags := map[string]string{"address": lastOpt(args, "-enableAddressSanitizer"), "thread": lastOpt(args, "-enableThreadSanitizer")}
	if len(cs) == 0 && (flags["address"] != "" || flags["thread"] != "") {
		cs = []scheme.TestConfig{{}}
	}
	for i := range cs {
		for _, sn := range []string{"address", "thread"} {
			switch v := flags[sn]; {
			case strings.EqualFold(v, "YES") && !contains(cs[i].Sanitizers, sn):
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

// planRunnerHook decides whether exec inserts its runner hook into the command's test runners and with what (design §4.5).
// Not when: no destination needs it; any destination isn't an iOS simulator; the Xcode in use has no
// verified test-runner restore rule (STUNTCAM_HOOK_FORCE=1 overrides); the command's own wrapper sets
// TEST_RUNNER_DYLD_INSERT_LIBRARIES; a test plan or .xctestrun can't be read; the runners would need different sanitizer
// runtimes (two sanitizers, or one only in some configurations: one variable can't give each runner its own); or the
// sanitizer runtime isn't in the Xcode in use. Each "not" is one warning naming what UI tests then miss.
func planRunnerHook(in hookInputs) hookPlan {
	var p hookPlan
	if in.Hook == "" || (len(in.Hooked) == 0 && len(in.Dest.Named) == 0) {
		return p
	}
	getenv := func(k string) (string, bool) {
		for _, kv := range in.Environ {
			if n, v, ok := strings.Cut(kv, "="); ok && n == k {
				return v, true
			}
		}
		return "", false
	}
	where := strings.Join(append(append([]string{}, in.Hooked...), in.Dest.Named...), ", ")
	skip := func(why string) hookPlan {
		return hookPlan{Notes: append(p.Notes, fmt.Sprintf("stuntcam: %s: no runner hook, so UI tests' apps on %s get no camera (stuntcam up --udid <sim> --reboot arms them at boot instead)", why, where))}
	}
	if len(in.Dest.Foreign) > 0 {
		return skip("the command also tests on " + strings.Join(in.Dest.Foreign, "; ") + ", and the runner hook is for iOS simulators only")
	}
	if !in.RulesOK {
		if v, _ := getenv("STUNTCAM_HOOK_FORCE"); v != "1" {
			what := "Xcode " + in.XcodeBuild
			if in.XcodeBuild == "" {
				what = "the Xcode in use (not identified)"
			}
			return skip(fmt.Sprintf("%s has no verified test-runner restore rule (%s; STUNTCAM_HOOK_FORCE=1 uses the hook anyway, with its built-in Xcode 27.0 list)", what, rulesDirName(in.Rules)))
		}
		p.Notes = append(p.Notes, "stuntcam: STUNTCAM_HOOK_FORCE=1: runner hook used without a verified test-runner restore rule for this Xcode")
	}
	if dd, ok := in.WrapEnv["DEVELOPER_DIR"]; ok && xcode.NormalizeDeveloperDir(dd) != xcode.NormalizeDeveloperDir(in.DeveloperDir) {
		// the command runs another Xcode than the one exec identified: its rules and sanitizer runtimes are unknown (r2 L-1)
		return skip("the command sets DEVELOPER_DIR=" + dd + " (exec identified " + in.DeveloperDir + ")")
	}
	if _, ok := in.WrapEnv["TEST_RUNNER_DYLD_INSERT_LIBRARIES"]; ok {
		return skip("the command sets TEST_RUNNER_DYLD_INSERT_LIBRARIES itself (which would replace the hook's)")
	}
	cs, src, err := testConfigs(in.Args, in.WD)
	if err != nil {
		return skip(fmt.Sprintf("can't read how the tests run (%v), so exec can't tell which sanitizer runtime the runner needs", err))
	}
	// one sanitizer set for every runner, or no hook
	var sans []string
	same := true
	for i, c := range cs {
		for _, s := range c.Sanitizers {
			if !contains(sans, s) {
				sans = append(sans, s)
			}
		}
		if i > 0 && strings.Join(c.Sanitizers, ",") != strings.Join(cs[0].Sanitizers, ",") {
			same = false
		}
	}
	sort.Strings(sans)
	names := func() string {
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
	switch {
	case len(sans) > 1:
		return skip(fmt.Sprintf("the tests run with more than one sanitizer (%s; %s) and one TEST_RUNNER_DYLD_INSERT_LIBRARIES can't give each runner its own runtime", names(), src))
	case !same:
		return skip(fmt.Sprintf("only some test configurations run with the %s sanitizer (%s; %s) and one TEST_RUNNER_DYLD_INSERT_LIBRARIES can't give only those runners its runtime", sans[0], names(), src))
	}
	var first []string
	callers, _ := getenv("TEST_RUNNER_DYLD_INSERT_LIBRARIES")
	for _, sn := range sans {
		r := sanitizerRuntime(in.DeveloperDir, sn)
		if r == "" {
			if hasRuntime(strings.Split(callers, ":"), map[string]string{"address": "libclang_rt.asan_x", "thread": "libclang_rt.tsan_x"}[sn]) {
				continue // the caller's own TEST_RUNNER_DYLD_INSERT_LIBRARIES has it
			}
			return skip(fmt.Sprintf("the tests run with the %s sanitizer and its runtime isn't in %s (the hook would drop the runner's sanitizer runtime and hang the run)", sn, in.DeveloperDir))
		}
		first = append(first, r)
	}
	if len(first) > 0 {
		p.Notes = append(p.Notes, "stuntcam: sanitizer tests: the runner keeps "+strings.Join(first, ", "))
	}
	// what the scheme or plan inserts into the runner itself is replaced by the variable
	var dropped []string
	noMTC := false
	for _, c := range cs {
		if c.GuardMalloc && !contains(dropped, "Guard Malloc (libgmalloc)") {
			dropped = append(dropped, "Guard Malloc (libgmalloc)")
		}
		for _, l := range c.Inserts {
			if !contains(dropped, l) {
				dropped = append(dropped, l)
			}
		}
		noMTC = noMTC || c.NoMTC
	}
	if len(dropped) > 0 {
		p.Notes = append(p.Notes, fmt.Sprintf("stuntcam: %s inserts %s into the test runner; the runner hook's TEST_RUNNER_DYLD_INSERT_LIBRARIES replaces that list, so the runner runs without it (the app under test keeps its own)", src, strings.Join(dropped, ", ")))
	}
	// the caller's SIMCAM_* settings reach the runner (xcodebuild passes on only TEST_RUNNER_ variables), and a scheme that
	// turns the Main Thread Checker off keeps it off there
	var extra []string
	set := map[string]bool{}
	for _, kv := range in.Environ {
		k, v, _ := strings.Cut(kv, "=")
		if !strings.HasPrefix(k, "SIMCAM_") || set[k] {
			continue
		}
		switch k {
		case "SIMCAM_HOME", "SIMCAM_SIM_DIR", "SIMCAM_STATE_DIR", "SIMCAM_CHAIN", "SIMCAM_CONTROL", "SIMCAM_HOOK_DIR", "SIMCAM_HOOK_STUB", "SIMCAM_HOOK_RULES":
			continue // exec's own, or a sim's paths (the hook takes those from its .env file)
		}
		if _, ok := getenv("TEST_RUNNER_" + k); !ok {
			extra = append(extra, "TEST_RUNNER_"+k+"="+v)
			set[k] = true
		}
	}
	if _, ok := getenv("TEST_RUNNER_SIMCAM_MTC"); noMTC && !ok && !set["SIMCAM_MTC"] {
		extra = append(extra, "TEST_RUNNER_SIMCAM_MTC=0")
	}
	sort.Strings(extra)
	rules := in.Rules
	if !in.RulesOK {
		rules = "" // forced: the hook's built-in list
	}
	p.Export = true
	p.Env = runnerHookEnv(callers, in.Hook, in.Dir, in.Stub, rules, first, extra)
	for _, n := range in.Dest.Named {
		p.Notes = append(p.Notes, fmt.Sprintf("stuntcam: -destination %q names no simulator by id: if it is launchd-armed, UI tests' apps there get the camera through the runner hook; booted unarmed, they don't (name it by id=, or stuntcam up --udid <sim> first)", n))
	}
	return p
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
		if x.Name == "test-runner" && len(x.Libs) > 2 && strings.TrimSpace(strings.Trim(strings.TrimSpace(string(x.Libs)), "[]")) != "" {
			return true
		}
	}
	return false
}

// sanitizerRuntime is the Xcode toolchain's simulator runtime for a sanitizer ("" if absent): the file Xcode copies into the
// test bundle. Inserted into the runner it does what Xcode's own insert of that copy did (checked with an ASan UI test on iOS 27.0).
func sanitizerRuntime(developerDir, s string) string {
	name := map[string]string{"address": "libclang_rt.asan_iossim_dynamic.dylib", "thread": "libclang_rt.tsan_iossim_dynamic.dylib"}[s]
	if name == "" || developerDir == "" {
		return ""
	}
	m, _ := filepath.Glob(filepath.Join(developerDir, "Toolchains", "XcodeDefault.xctoolchain", "usr", "lib", "clang", "*", "lib", "darwin", name))
	sort.Strings(m)
	for i := len(m) - 1; i >= 0; i-- { // the newest clang dir (one is usually a symlink to the other)
		if fi, err := os.Stat(m[i]); err == nil && fi.Mode().IsRegular() {
			return m[i]
		}
	}
	return ""
}

func contains(xs []string, x string) bool {
	for _, v := range xs {
		if v == x {
			return true
		}
	}
	return false
}
