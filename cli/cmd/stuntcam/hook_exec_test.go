// SPDX-License-Identifier: Apache-2.0
package main

// exec's runner-hook wiring (review r1: B1, B2, M1, M2, L3, L6, NITs)

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/avinav-b/stuntcam/cli/internal/scheme"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

func TestXcodebuildTest(t *testing.T) {
	yes := [][]string{
		{"xcodebuild", "test", "-scheme", "S"},
		{"/usr/bin/xcodebuild", "-scheme", "S", "-destination", "id=X", "test-without-building"},
		{"xcrun", "xcodebuild", "test"},
		{"xcrun", "--sdk", "iphonesimulator", "xcodebuild", "build", "test"},
		{"env", "A=1", "xcodebuild", "-project", "P.xcodeproj", "test"},
		{"/usr/bin/env", "-u", "X", "B=2", "arch", "-arm64", "xcrun", "xcodebuild", "test-without-building", "-xctestrun", "a.xctestrun"},
		{"arch", "-arch", "arm64", "-e", "K=V", "xcodebuild", "CODE_SIGNING_ALLOWED=NO", "test"},
		{"xcodebuild", "-quiet", "test"},
		{"xcodebuild", "-only-testing:T/test", "test"},
	}
	no := [][]string{
		{"xcodebuild", "build"}, {"xcodebuild", "build-for-testing"}, {"xcodebuild", "-only-testing:T/test"},
		{"./ci.sh", "test"}, {"xcrun", "simctl", "list"}, {}, {"xcrun"},
		{"xcodebuild", "-scheme", "test", "build"},                  // an option's value, not the action
		{"xcodebuild", "-testPlan", "test", "-destination", "test"}, // same
		{"xcodebuild", "TEST=test", "build"},                        // a build setting
		{"env", "-i", "PATH=/usr/bin", "xcodebuild", "test"},        // no TEST_RUNNER_ variable would reach it
		{"arch", "-c", "-arm64", "xcodebuild", "test"},
		{"env", "./run-tests.sh", "test"},
	}
	for _, c := range yes {
		if !xcodebuildTest(c) {
			t.Errorf("%q: not taken for xcodebuild test", c)
		}
	}
	for _, c := range no {
		if xcodebuildTest(c) {
			t.Errorf("%q: taken for xcodebuild test", c)
		}
	}
	c := parseXcodebuild([]string{"env", "TEST_RUNNER_DYLD_INSERT_LIBRARIES=/x", "arch", "-e", "K=V", "xcodebuild", "test", "-scheme", "S"})
	if !c.OK || c.Env["TEST_RUNNER_DYLD_INSERT_LIBRARIES"] != "/x" || c.Env["K"] != "V" || !reflect.DeepEqual(c.Args, []string{"test", "-scheme", "S"}) {
		t.Fatalf("%+v", c)
	}
}

var testDevs = []simctl.Device{
	{UDID: "IOS1", Name: "iPhone A", State: "Booted", RuntimeID: "com.apple.CoreSimulator.SimRuntime.iOS-27-0"},
	{UDID: "IOS2", Name: "iPhone B", State: "Shutdown", RuntimeID: "com.apple.CoreSimulator.SimRuntime.iOS-26-3"},
	{UDID: "TV1", Name: "Apple TV", State: "Booted", RuntimeID: "com.apple.CoreSimulator.SimRuntime.tvOS-26-0"},
	{UDID: "WATCH1", Name: "Watch", State: "Booted", RuntimeID: "com.apple.CoreSimulator.SimRuntime.watchOS-26-0"},
	{UDID: "XR1", Name: "Vision", State: "Booted", RuntimeID: "com.apple.CoreSimulator.SimRuntime.xrOS-26-0"},
}

func TestDestinations(t *testing.T) {
	d := func(specs ...string) destSet {
		var args []string
		for _, s := range specs {
			args = append(args, "-destination", s)
		}
		return destinations(append([]string{"test"}, args...), testDevs)
	}
	if got := d("id=ios1", "platform=iOS Simulator,id=IOS1", "id=IOS2"); !reflect.DeepEqual(got.IOS, []string{"IOS1", "IOS2"}) || got.Named != nil || got.Foreign != nil {
		t.Fatalf("%+v", got)
	}
	if got := d("platform=iOS Simulator,name=iPhone 17 Pro", "generic/platform=iOS Simulator"); len(got.Named) != 1 || got.IOS != nil || got.Foreign != nil {
		t.Fatalf("%+v", got)
	}
	for _, spec := range []string{"id=TV1", "platform=tvOS Simulator,id=TV1", "id=WATCH1", "platform=visionOS Simulator,id=XR1",
		"id=00008110-001234560E10801E" /* a phone */, "platform=macOS", "platform=macOS,variant=Mac Catalyst", "platform=iOS,name=My iPhone",
		"platform=tvOS Simulator,name=Apple TV", "name=iPhone 17 Pro"} {
		if got := d("id=IOS1", spec); len(got.Foreign) != 1 || !reflect.DeepEqual(got.IOS, []string{"IOS1"}) {
			t.Errorf("%q: %+v", spec, got)
		}
	}
}

func TestRunnerHookEnvSanitizerFirst(t *testing.T) {
	got := runnerHookEnv("/t/libown.dylib", "/h/hook.dylib", "/d", "/s", "", []string{"/x/libclang_rt.asan_iossim_dynamic.dylib"}, []string{"TEST_RUNNER_SIMCAM_MTC=0"})
	if got[0] != "TEST_RUNNER_DYLD_INSERT_LIBRARIES=/x/libclang_rt.asan_iossim_dynamic.dylib:/t/libown.dylib:/h/hook.dylib" || got[len(got)-1] != "TEST_RUNNER_SIMCAM_MTC=0" {
		t.Fatalf("%q", got)
	}
	// the caller's own runtime (any copy) is kept, moved first, and not doubled
	got = runnerHookEnv("/t/libown.dylib:/mine/libclang_rt.asan_iossim_dynamic.dylib", "/h/hook.dylib", "/d", "/s", "", []string{"/x/libclang_rt.asan_iossim_dynamic.dylib"}, nil)
	if got[0] != "TEST_RUNNER_DYLD_INSERT_LIBRARIES=/mine/libclang_rt.asan_iossim_dynamic.dylib:/t/libown.dylib:/h/hook.dylib" {
		t.Fatalf("%q", got[0])
	}
}

func TestHasRuntime(t *testing.T) {
	l := []string{"/a/libclang_rt.asan_iossim_dynamic.dylib", "/b/libown.dylib"}
	if !hasRuntime(l, "libclang_rt.asan_osx_dynamic.dylib") || hasRuntime(l, "/t/libclang_rt.tsan_iossim_dynamic.dylib") || hasRuntime(l, "libown.dylib") {
		t.Fatal()
	}
}

// the shape of Xcode 27.0's build-for-testing output for fixtures/u4diag's M11UIMixed plan (trimmed)
const xctestrunTwo = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>TestConfigurations</key><array>
 <dict><key>Name</key><string>ASan</string><key>TestTargets</key><array><dict>
  <key>BlueprintName</key><string>M11AppUITests</string><key>IsUITestBundle</key><true/><key>DefaultTestExecutionTimeAllowance</key><integer>600</integer>
  <key>EnvironmentVariables</key><dict><key>DYLD_INSERT_LIBRARIES</key><string>/usr/lib/libRPAC.dylib</string></dict>
  <key>TestingEnvironmentVariables</key><dict><key>DYLD_INSERT_LIBRARIES</key><string>__TESTBUNDLE__/Frameworks/libclang_rt.asan_iossim_dynamic.dylib</string></dict>
 </dict></array></dict>
 <dict><key>Name</key><string>TSan</string><key>TestTargets</key><array><dict>
  <key>BlueprintName</key><string>M11AppUITests</string>
  <key>EnvironmentVariables</key><dict><key>DYLD_INSERT_LIBRARIES</key><string>/usr/lib/libRPAC.dylib</string></dict>
  <key>TestingEnvironmentVariables</key><dict><key>DYLD_INSERT_LIBRARIES</key><string>__TESTBUNDLE__/Frameworks/libclang_rt.tsan_iossim_dynamic.dylib</string></dict>
 </dict></array></dict>
</array>
<key>TestPlan</key><dict><key>IsDefault</key><false/><key>Name</key><string>M11UIMixed</string></dict>
<key>__xctestrun_metadata__</key><dict><key>FormatVersion</key><integer>2</integer></dict>
</dict></plist>`

// a project dir with schemes (one with test plans) and .xctestrun files; returns its root
func testProject(t *testing.T) string {
	d := t.TempDir()
	dir := filepath.Join(d, "ios", "App.xcodeproj", "xcshareddata", "xcschemes")
	os.MkdirAll(filepath.Join(d, "ios", "Plans"), 0o755)
	os.MkdirAll(dir, 0o755)
	w := func(p, s string) {
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	w(filepath.Join(dir, "ASan.xcscheme"), `<Scheme><TestAction enableAddressSanitizer = "YES"></TestAction></Scheme>`)
	w(filepath.Join(dir, "Plain.xcscheme"), `<Scheme><TestAction></TestAction></Scheme>`)
	w(filepath.Join(dir, "NoMTC.xcscheme"), `<Scheme><TestAction disableMainThreadChecker = "YES"><AdditionalOptions><AdditionalOption key = "DYLD_INSERT_LIBRARIES" value = "/usr/lib/libgmalloc.dylib" isEnabled = "YES"></AdditionalOption></AdditionalOptions></TestAction></Scheme>`)
	w(filepath.Join(dir, "Plans.xcscheme"), `<Scheme><TestAction enableThreadSanitizer = "YES"><TestPlans>
		<TestPlanReference reference = "container:Plans/Plain.xctestplan"></TestPlanReference>
		<TestPlanReference reference = "container:ASan.xctestplan" default = "YES"></TestPlanReference>
		<TestPlanReference reference = "container:Mixed.xctestplan"></TestPlanReference>
		<TestPlanReference reference = "container:Two.xctestplan"></TestPlanReference>
		<TestPlanReference reference = "container:Gone.xctestplan"></TestPlanReference>
		<TestPlanReference reference = "container:Bad.xctestplan"></TestPlanReference></TestPlans></TestAction></Scheme>`)
	w(filepath.Join(d, "ios", "Plans", "Plain.xctestplan"), `{"configurations":[{"name":"C1","options":{}}],"defaultOptions":{},"version":1}`)
	w(filepath.Join(d, "ios", "ASan.xctestplan"), `{"configurations":[{"name":"Default","options":{}}],"defaultOptions":{"addressSanitizer":{"enabled":true}},"version":1}`)
	w(filepath.Join(d, "ios", "Mixed.xctestplan"), `{"configurations":[{"name":"ASan","options":{"addressSanitizer":{"enabled":true}}},{"name":"Off","options":{"mainThreadCheckerEnabled":false}}],"defaultOptions":{},"version":1}`)
	w(filepath.Join(d, "ios", "Two.xctestplan"), `{"configurations":[{"name":"ASan","options":{"addressSanitizer":{"enabled":true}}},{"name":"TSan","options":{"threadSanitizerEnabled":true}}],"defaultOptions":{"environmentVariableEntries":[{"key":"DYLD_INSERT_LIBRARIES","value":"/x/libreveal.dylib"}]},"version":1}`)
	w(filepath.Join(d, "ios", "Bad.xctestplan"), `{"configurations":[`)
	w(filepath.Join(d, "two.xctestrun"), xctestrunTwo)
	w(filepath.Join(d, "asan.xctestrun"), strings.Replace(xctestrunTwo, "tsan", "asan", 1))
	return d
}

func TestTestConfigs(t *testing.T) {
	d := testProject(t)
	san := func(cs []scheme.TestConfig) [][]string {
		var out [][]string
		for _, c := range cs {
			out = append(out, c.Sanitizers)
		}
		return out
	}
	P := []string{"-project", "ios/App.xcodeproj"}
	cases := []struct {
		args []string
		want [][]string
	}{
		{append([]string{"test", "-scheme", "ASan"}, P...), [][]string{{"address"}}},
		{append([]string{"test", "-scheme", "ASan", "-enableAddressSanitizer", "NO"}, P...), [][]string{nil}},
		{append([]string{"test", "-scheme", "Plain"}, P...), [][]string{nil}},
		{[]string{"test", "-scheme", "Plain", "-enableThreadSanitizer", "YES"}, [][]string{{"thread"}}},
		// B1: test plans win over the scheme's own (here TSan) attribute; the default plan, or -testPlan
		{append([]string{"test", "-scheme", "Plans"}, P...), [][]string{{"address"}}},
		{append([]string{"test", "-scheme", "Plans", "-testPlan", "Plain"}, P...), [][]string{nil}},
		{append([]string{"test", "-scheme", "Plans", "-testPlan", "Mixed"}, P...), [][]string{{"address"}, nil}},
		{append([]string{"test", "-scheme", "Plans", "-testPlan", "Mixed", "-only-test-configuration", "Off"}, P...), [][]string{nil}},
		{append([]string{"test", "-scheme", "Plans", "-testPlan", "Two", "-skip-test-configuration", "TSan"}, P...), [][]string{{"address"}}},
		// M2
		{[]string{"test-without-building", "-xctestrun", "two.xctestrun"}, [][]string{{"address"}, {"thread"}}},
		{[]string{"test-without-building", "-xctestrun", "two.xctestrun", "-only-test-configuration", "TSan"}, [][]string{{"thread"}}},
		{[]string{"test-without-building", "-xctestrun", "asan.xctestrun"}, [][]string{{"address"}, {"address"}}},
		{[]string{"test-without-building", "-xctestrun", "two.xctestrun", "-only-testing:Other/T"}, nil},
	}
	for _, c := range cases {
		cs, _, err := testConfigs(c.args, d)
		if err != nil || !reflect.DeepEqual(san(cs), c.want) {
			t.Errorf("%q: %q (%v), want %q", c.args, san(cs), err, c.want)
		}
	}
	for _, args := range [][]string{
		append([]string{"test", "-scheme", "Plans", "-testPlan", "Gone"}, P...),
		append([]string{"test", "-scheme", "Plans", "-testPlan", "Bad"}, P...),
		append([]string{"test", "-scheme", "Plans", "-testPlan", "Unlisted"}, P...),
		append([]string{"test", "-scheme", "Plain", "-testPlan", "X"}, P...),
		{"test-without-building", "-xctestrun", "missing.xctestrun"},
		{"test", "-scheme", "Nowhere"}, // r2 M-1: unknown sanitizers decline instead of exporting the hook
		{"test-without-building", "-testProductsPath", "none.xctestproducts"},
	} {
		if _, _, err := testConfigs(args, d); err == nil {
			t.Errorf("%q: no error", args)
		}
	}
	cs, _, _ := testConfigs(append([]string{"test", "-scheme", "NoMTC"}, P...), d)
	if len(cs) != 1 || !cs[0].NoMTC || !cs[0].GuardMalloc {
		t.Fatalf("%+v", cs)
	}
	cs, _, _ = testConfigs(append([]string{"test", "-scheme", "Plans", "-testPlan", "Two"}, P...), d)
	if len(cs) != 2 || !reflect.DeepEqual(cs[0].Inserts, []string{"/x/libreveal.dylib"}) {
		t.Fatalf("%+v", cs)
	}
}

// the exec wiring (L6): what planRunnerHook exports, and when it declines
func TestPlanRunnerHook(t *testing.T) {
	d := testProject(t)
	dev := filepath.Join(d, "Xcode.app", "Contents", "Developer")
	lib := filepath.Join(dev, "Toolchains", "XcodeDefault.xctoolchain", "usr", "lib", "clang", "21", "lib", "darwin")
	os.MkdirAll(lib, 0o755)
	asan := filepath.Join(lib, "libclang_rt.asan_iossim_dynamic.dylib")
	os.WriteFile(asan, []byte("x"), 0o644)
	rules := filepath.Join(d, "27A266a.json")
	os.WriteFile(rules, []byte(`{"rules":[{"name":"test-runner","libs":[{"path":"/usr/lib/libMainThreadChecker.dylib"}]}]}`), 0o644)
	norule := filepath.Join(d, "27B1.json")
	os.WriteFile(norule, []byte(`{"rules":[{"name":"hosted-tests","libs":[]}]}`), 0o644)
	if !testRunnerRule(rules) || testRunnerRule(norule) || testRunnerRule(filepath.Join(d, "none.json")) || testRunnerRule("") {
		t.Fatal("testRunnerRule")
	}
	base := func(args ...string) hookInputs {
		a := append([]string{"test", "-project", "ios/App.xcodeproj", "-destination", "id=IOS1"}, args...)
		return hookInputs{Args: a, WD: d, Dest: destinations(a, testDevs), Hooked: []string{"iPhone A"}, Hook: "/h/libsimcamrunnerhook.dylib",
			Dir: "/run/hook", Stub: "/s/libsimcamstub.dylib", Rules: rules, RulesOK: true, XcodeBuild: "27A266a", DeveloperDir: dev,
			Environ: []string{"PATH=/usr/bin", "TEST_RUNNER_DYLD_INSERT_LIBRARIES=/t/libmine.dylib", "SIMCAM_HOOK_DIAG=1", "SIMCAM_HOME=/x",
				"TEST_RUNNER_SIMCAM_XCODE_LIBS=1", "SIMCAM_XCODE_LIBS=0"}}
	}
	get := func(p hookPlan, k string) string {
		for _, kv := range p.Env {
			if n, v, _ := strings.Cut(kv, "="); n == k {
				return v
			}
		}
		return "<unset>"
	}
	noted := func(p hookPlan, sub string) bool {
		for _, n := range p.Notes {
			if strings.Contains(n, sub) {
				return true
			}
		}
		return false
	}
	// plain: hooked, the caller's TEST_RUNNER_ insert kept (first), SIMCAM_* settings forwarded unless the caller set the
	// TEST_RUNNER_ one, exec's own SIMCAM_HOME not
	p := planRunnerHook(base("-scheme", "Plain"))
	if !p.Export || get(p, "TEST_RUNNER_DYLD_INSERT_LIBRARIES") != "/t/libmine.dylib:/h/libsimcamrunnerhook.dylib" || get(p, "TEST_RUNNER_SIMCAM_HOOK_RULES") != rules ||
		get(p, "TEST_RUNNER_SIMCAM_HOOK_DIAG") != "1" || get(p, "TEST_RUNNER_SIMCAM_XCODE_LIBS") != "<unset>" || get(p, "TEST_RUNNER_SIMCAM_HOME") != "<unset>" ||
		get(p, "TEST_RUNNER_SIMCAM_MTC") != "<unset>" || len(p.Notes) != 0 {
		t.Fatalf("plain: %+v", p)
	}
	// r2 L-1: a wrapper running another Xcode declines; the same Xcode named as the .app doesn't
	inL1 := base("-scheme", "Plain")
	inL1.WrapEnv = map[string]string{"DEVELOPER_DIR": "/Applications/Xcode-26.app/Contents/Developer"}
	if p := planRunnerHook(inL1); p.Export || !noted(p, "DEVELOPER_DIR") {
		t.Fatalf("other Xcode: %+v", p)
	}
	inL1.WrapEnv = map[string]string{"DEVELOPER_DIR": strings.TrimSuffix(dev, "/Contents/Developer")}
	if p := planRunnerHook(inL1); strings.HasSuffix(dev, "/Contents/Developer") && !p.Export {
		t.Fatalf("same Xcode as .app: %+v", p)
	}
	// ASan through a test plan (B1): the toolchain runtime first
	p = planRunnerHook(base("-scheme", "Plans"))
	if !p.Export || get(p, "TEST_RUNNER_DYLD_INSERT_LIBRARIES") != asan+":/t/libmine.dylib:/h/libsimcamrunnerhook.dylib" || !noted(p, "the runner keeps") {
		t.Fatalf("plan asan: %+v", p)
	}
	// the sanitizer runtime missing from this Xcode: no hook, unless the caller's own variable has it
	in := base("-scheme", "ASan")
	in.DeveloperDir = filepath.Join(d, "nowhere")
	if p = planRunnerHook(in); p.Export || !noted(p, "isn't in") {
		t.Fatalf("no runtime: %+v", p)
	}
	in.Environ = []string{"TEST_RUNNER_DYLD_INSERT_LIBRARIES=/mine/libclang_rt.asan_iossim_dynamic.dylib"}
	if p = planRunnerHook(in); !p.Export || get(p, "TEST_RUNNER_DYLD_INSERT_LIBRARIES") != "/mine/libclang_rt.asan_iossim_dynamic.dylib:/h/libsimcamrunnerhook.dylib" {
		t.Fatalf("caller's runtime: %+v", p)
	}
	// declined, with one warning naming the destination: mixed plan, two sanitizers, unreadable plan, an .xctestrun with two,
	// a non-iOS-simulator destination, no verified rule, the command's own wrapper setting the variable
	with := func(in hookInputs, f func(*hookInputs)) hookInputs { f(&in); return in }
	for name, in := range map[string]hookInputs{
		"mixed":     base("-scheme", "Plans", "-testPlan", "Mixed"),
		"two":       base("-scheme", "Plans", "-testPlan", "Two"),
		"bad plan":  base("-scheme", "Plans", "-testPlan", "Bad"),
		"xctestrun": base("-xctestrun", "two.xctestrun"),
		"tvOS":      base("-scheme", "Plain", "-destination", "platform=tvOS Simulator,id=TV1"),
		"watchOS":   base("-scheme", "Plain", "-destination", "id=WATCH1"),
		"device":    base("-scheme", "Plain", "-destination", "id=00008110-001234560E10801E"),
		"macOS":     base("-scheme", "Plain", "-destination", "platform=macOS,variant=Mac Catalyst"),
		"no rule":   with(base("-scheme", "Plain"), func(i *hookInputs) { i.Rules, i.RulesOK, i.XcodeBuild = norule, false, "27B1" }),
		"wrapper":   with(base("-scheme", "Plain"), func(i *hookInputs) { i.WrapEnv = map[string]string{"TEST_RUNNER_DYLD_INSERT_LIBRARIES": "/x"} }),
	} {
		if p := planRunnerHook(in); p.Export || len(p.Notes) != 1 || !strings.Contains(p.Notes[0], "no runner hook") || !strings.Contains(p.Notes[0], "iPhone A") {
			t.Errorf("%s: %+v", name, p)
		}
	}
	// forced without a verified rule: hooked, with the hook's built-in list
	in = with(base("-scheme", "Plain"), func(i *hookInputs) {
		i.Rules, i.RulesOK = norule, false
		i.Environ = append(i.Environ, "STUNTCAM_HOOK_FORCE=1")
	})
	if p = planRunnerHook(in); !p.Export || get(p, "TEST_RUNNER_SIMCAM_HOOK_RULES") != "<unset>" || !noted(p, "STUNTCAM_HOOK_FORCE") {
		t.Fatalf("forced: %+v", p)
	}
	// a scheme with the Main Thread Checker off and Guard Malloc (L3)
	if p = planRunnerHook(base("-scheme", "NoMTC")); !p.Export || get(p, "TEST_RUNNER_SIMCAM_MTC") != "0" || !noted(p, "Guard Malloc") {
		t.Fatalf("nomtc: %+v", p)
	}
	// only a named iOS simulator destination: hooked, and said before the run; nothing to hook: nothing said
	in = base("-scheme", "Plain")
	in.Args = []string{"test", "-scheme", "Plain", "-destination", "platform=iOS Simulator,name=iPhone 17 Pro"}
	in.Dest, in.Hooked = destinations(in.Args, testDevs), nil
	if p = planRunnerHook(in); !p.Export || !noted(p, "names no simulator by id") {
		t.Fatalf("named: %+v", p)
	}
	in.Args = []string{"test", "-scheme", "Plain"}
	in.Dest = destinations(in.Args, testDevs)
	if p = planRunnerHook(in); p.Export || len(p.Notes) != 0 {
		t.Fatalf("none: %+v", p)
	}
	if p = planRunnerHook(with(base("-scheme", "Plain"), func(i *hookInputs) { i.Hook = "" })); p.Export {
		t.Fatalf("no hook in this build: %+v", p)
	}
}

// w4 e45: Xcode 27's .xctestproducts keeps the .xctestrun at Tests/<n>/<scheme>.xctestrun
func TestTestProductsPath(t *testing.T) {
	d := t.TempDir()
	xr := filepath.Join(d, "P.xctestproducts", "Tests", "0", "M11UI.xctestrun")
	os.MkdirAll(filepath.Dir(xr), 0o755)
	os.WriteFile(xr, []byte(`<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict></dict></plist>`), 0o644)
	if _, got, err := testConfigs([]string{"test-without-building", "-testProductsPath", "P.xctestproducts"}, d); got != xr && (err == nil || !strings.Contains(err.Error(), xr)) {
		t.Fatalf("found %q (%v), want %s", got, err, xr)
	}
}
