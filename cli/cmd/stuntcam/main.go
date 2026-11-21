// SPDX-License-Identifier: Apache-2.0

// Command stuntcam gives the iOS Simulator a camera for your own app's camera code (design §4.5).
//
// Files (cli/README.md, "Command files"):
//   - main.go: the command table, usage, main (dispatch, exit statuses) and flag parsing
//   - up.go, down.go, swap.go, env.go, chain.go, verify.go: arming a simulator, changing or ending it, the canary
//   - exec.go, hook.go, xcodebuild.go: exec, its runner hook, and reading the command's xcodebuild arguments
//   - status.go, sentinel.go, trace.go, doctor.go: what stuntcam knows about simulators and apps
//   - source.go, orient.go, control.go, webcam.go: live control of an armed simulator, and the Mac camera
//   - service.go, gc.go, version.go: the login service, cleanup, the version
//   - sims.go, xcoderecord.go, fallback.go, helpers.go: shared helpers
package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/verify"
)

type command struct {
	name, args, help string
	run              func(args []string) error
}

var commands []command

func init() {
	commands = []command{
		{"up", "[--udid U] [--mode boot|launchd] [--reboot] [--grant BUNDLE]... [--source S] [--log] [--build DIR] " +
			"[--skip-verify] [--allow-unverified]",
			"arm a simulator (boots it armed if it is shut down)", cmdUp},
		{"verify", "[--runtime V] [--fresh] [--keep] [--json] [--build DIR]",
			"canary verdict for this build on a runtime (private device set, cached)", cmdVerify},
		{"exec", "[--build DIR] -- COMMAND [ARGS…]",
			"run a command (xcodebuild test, a CI script) so simulators it boots come up armed", cmdExec},
		{"service", "install [--preboot U] [--only U]…|uninstall|status|run [--only U]…",
			"notify-only helper: arms simulators Xcode/Simulator boot (launchd mode) and says when Xcode Run needs `up`",
			cmdService},
		{"gc", "[--dry-run] [--keep-builds N]",
			"remove private canary devices for removed runtimes, unused builds and unpacked canaries, and state of deleted " +
				"simulators",
			cmdGC},
		{"down", "[--udid U]", "stop arming new launches (no reboot needed)", cmdDown},
		{"status", "[--json]", "every simulator stuntcam knows about: mode, build, latches", cmdStatus},
		{"swap", "[--udid U] [LOADER]",
			"point an armed simulator at another build, effective at the next launch", cmdSwap},
		{"chain", "add|rm|ls [--udid U] [LIB]",
			"extra libraries every app launch loads (e.g. another tool's loader)", cmdChain},
		{"sentinel", "ls|clear [--udid U] [--bundle B]", "crash/hang records and latches per app", cmdSentinel},
		{"trace", "[--json] [--udid U] [--pid N] BUNDLE",
			"an app's last 64 camera ops with host-clock times (shutter -> slot 12 -> still complete -> timestamp)",
			cmdTrace},
		{"env", "[--udid U]", "print SIMCTL_CHILD_* exports for per-launch arming (Detox, scripts)", cmdEnv},
		{"source", "[--udid U] [--front NAME] [--back NAME] scene|image:PATH|video:PATH|webcam[:NAME]",
			"switch the picture live, in running apps and the next ones (webcam: the Mac camera)", cmdSource},
		{"orient", "[--udid U] portrait|upsideDown|landscapeLeft|landscapeRight|auto",
			"set the virtual device orientation, live", cmdOrient},
		{"doctor", "[--json]", "check Xcode, runtimes, the host directory, the build and each simulator", cmdDoctor},
		{"version", "[--check]",
			"print the version and the commit it was built from (--check: verify the installed files and unpack the payload)",
			cmdVersion},
	}
}

func usage() {
	fmt.Fprint(os.Stderr, "usage: stuntcam <command> [options]\n\n")
	for _, c := range commands {
		fmt.Fprintf(os.Stderr, "  %-9s %s\n            %s\n", c.name, c.help, c.args)
	}
}

// Exit statuses. exitUsage is sysexits.h's EX_USAGE, as in the scripts; exitCannotRun, exitNotFound and
// exitSignalBase+N are what a shell reports for a command it can't run, can't find, or that signal N killed (exec
// passes its command's own status on).
const (
	exitOK           = 0
	exitFailed       = 1   // the command failed, or verify's canary did
	exitInconclusive = 2   // verify: the canary ran to no verdict
	exitUsage        = 64  // wrong arguments or a setup problem, never a verdict
	exitCannotRun    = 126 // exec: the command exists but can't be run
	exitNotFound     = 127 // exec: no such command
	exitSignalBase   = 128 // exec: plus the number of the signal that killed the command
)

// exitStatus is a command's result that ends stuntcam with this status and prints nothing more: the command has
// already said what happened (verify's verdict, exec's command).
type exitStatus int

func (e exitStatus) Error() string { return fmt.Sprintf("exit status %d", int(e)) }

// usageError exits exitUsage: wrong arguments or a setup problem, never a verdict.
type usageError struct{ error }

// Unwrap lets errors.Is(err, flag.ErrHelp) see through a usageError (`stuntcam up -h`).
func (u usageError) Unwrap() error { return u.error }

func usagef(format string, a ...any) error { return usageError{fmt.Errorf(format, a...)} }

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(exitUsage)
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
				if errors.Is(err, flag.ErrHelp) { // `stuntcam up -h`: the flag package printed the options
					return
				}
				var status exitStatus
				if errors.As(err, &status) {
					os.Exit(int(status))
				}
				fmt.Fprintln(os.Stderr, "stuntcam:", err)
				var u usageError
				if errors.As(err, &u) {
					os.Exit(exitUsage)
				}
				os.Exit(exitFailed)
			}
			return
		}
	}
	fmt.Fprintf(os.Stderr, "stuntcam: unknown command %q\n", os.Args[1])
	usage()
	os.Exit(exitUsage)
}

// parse parses flags and allows at most maxPos positional arguments, all after the flags: Go's flag package stops at
// the first non-flag, so `stuntcam swap LOADER --udid X` would silently drop --udid and act on another simulator.
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
