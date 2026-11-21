// SPDX-License-Identifier: Apache-2.0

package main

import (
	"fmt"
	"path/filepath"
	"slices"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// xcodebuildValueOpts are xcodebuild options that take the next argument as their value (so a value such as `-scheme
// test` is not taken for the test action). Options not listed are taken as flags.
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
	OK   bool              // the command runs xcodebuild
	Args []string          // xcodebuild's arguments (without "xcodebuild")
	Env  map[string]string // NAME=VALUE the wrappers set (env, arch -e)
	// a wrapper starts xcodebuild with a cleared environment (env -i, arch -c): no TEST_RUNNER_ variable reaches it
	Cleared bool
}

// parseXcodebuild finds xcodebuild in cmdline, through `env [-i] [-u N] [N=V…]`, `arch [-arm64|-x86_64|-arch A] [-e
// N=V] [-c]` and `xcrun [options]` wrappers (in any order, nested). A script that runs xcodebuild inside isn't
// recognized.
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
				takesValue := args[0] == "--sdk" || args[0] == "-sdk" || args[0] == "--toolchain" || args[0] == "-toolchain"
				if takesValue && len(args) > 1 {
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

// runsTests reports whether the command line runs xcodebuild's test or test-without-building action itself (directly or
// through env/arch/xcrun) with an environment the TEST_RUNNER_ variables reach.
func (c xbCommand) runsTests() bool {
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

// destinations sorts the command's -destination arguments (design §4.5): IOS, iOS simulators named by id= that simctl
// lists (UDIDs upper case); Named, `platform=iOS Simulator` destinations with no id (exec can't tell their arming in
// advance; the hook still covers a launchd-armed one from the runner's own environment); Foreign, everything else
// (another platform's simulator, a physical device, macOS / Mac Catalyst, an id simctl doesn't list): the runner hook
// is an iOS-simulator library, so exec doesn't use it at all then. generic/ destinations (build only) are ignored.
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
			case !slices.Contains(out.IOS, id):
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
