// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/state"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// cmdExec runs a command with the boot environment exported (cli/README.md, exec): it prepares a run directory,
// boots shut-down -destination simulators armed, sets up the runner hook for `xcodebuild test` on launchd-armed and
// unarmed ones, runs the command and reports which apps used the camera. Its exit status is the command's.
func cmdExec(args []string) error {
	cmdline, build, err := parseExecArgs(args)
	if err != nil {
		return err
	}

	set, err := artifacts.Find(build)
	if err != nil {
		return err
	}

	if set, err = artifacts.Stage(set); err != nil {
		return err
	}

	run, err := newExecRun(set)
	if err != nil {
		return err
	}

	xcodebuildCmd := parseXcodebuild(cmdline)
	runsTests := xcodebuildCmd.runsTests()
	ids := destinationIDs(cmdline)
	var candidates []hookCandidate // launchd-armed or unarmed iOS destinations, hooked only if planRunnerHook says so
	var dest destSet
	if devices, err := ctl.Devices(); err == nil {
		dest = destinations(cmdline, devices)
		preBooted, err := run.preBootDestinations(ids, devices)
		if err != nil {
			return err
		}

		candidates = run.hookCandidates(ids, devices, preBooted, xcodebuildCmd)
	}

	if runsTests && set.Hook != "" && (len(candidates) > 0 || len(dest.Named) > 0) {
		run.setUpRunnerHook(xcodebuildCmd, dest, candidates)
	}

	return run.runAndReport(cmdline, ids)
}

// parseExecArgs splits exec's arguments into the command line (after an optional "--") and --build.
func parseExecArgs(args []string) (cmdline []string, build string, err error) {
	flags := flag.NewFlagSet("exec", flag.ContinueOnError)
	buildFlag := flags.String("build", "", "build dir")
	if err := flags.Parse(args); err != nil { // the command after "--" keeps its own options
		return nil, "", usageError{err}
	}

	cmdline = flags.Args()
	if len(cmdline) > 0 && cmdline[0] == "--" {
		cmdline = cmdline[1:]
	}

	if len(cmdline) == 0 {
		return nil, "", usagef("usage: stuntcam exec [--build DIR] -- COMMAND [ARGS…]")
	}

	return cmdline, *buildFlag, nil
}

// execRun is one `stuntcam exec`: the build, the run directory the simulators the command boots share, and the
// command's environment.
type execRun struct {
	set     artifacts.Set
	dir     string     // the run directory's name under sims/: exec-<ns>-<pid>
	xcode   xcode.Info // the Xcode the command's xcodebuild uses (the same DEVELOPER_DIR)
	env     []string   // the command's environment
	hookDir string     // <run dir>/hook: the runner hook's <udid>.env files and logs
	hooked  []simctl.Device
}

// newExecRun prepares the run directory (current, chain, state, the Xcode record) and the command's environment: this
// process's, plus SIMCTL_CHILD_ boot variables naming the run directory. The simulators the command boots share it,
// since their UDIDs aren't known in advance; it is unique per process, so parallel CI jobs don't share one.
func newExecRun(set artifacts.Set) (*execRun, error) {
	run := &execRun{set: set, dir: fmt.Sprintf("exec-%d-%d", time.Now().UnixNano(), os.Getpid())}
	if err := arm.Prepare(run.dir, set); err != nil {
		return nil, err
	}

	run.xcode = recordXcode(run.dir, set.Rules, "exec", noXcodeSummary, signalsFromThisProcess)
	run.env = os.Environ()
	for _, kv := range arm.Vars(run.dir, set, arm.Options{}) {
		key, value, _ := strings.Cut(kv, "=")
		if key == "DYLD_INSERT_LIBRARIES" { // keep another tool's insert the caller already exported
			value = arm.MergeInsert(os.Getenv("SIMCTL_CHILD_DYLD_INSERT_LIBRARIES"), value)
		}

		run.env = append(run.env, "SIMCTL_CHILD_"+key+"="+value)
	}

	run.hookDir = filepath.Join(home.Sim(run.dir), "hook")
	return run, nil
}

// preBootDestinations boots armed each shut-down iOS simulator among the command's -destination ids, and returns the
// ones it booted.
// xcodebuild boots a shut-down -destination itself, and that boot doesn't get the SIMCTL_CHILD_ variables. On Xcode
// 27.0 the sim then had no stuntcam variable and the test host had no stub. So the sim is booted armed before the
// command runs.
// Only iOS simulators are booted, because stuntcam arms nothing else.
func (r *execRun) preBootDestinations(ids []string, devices []simctl.Device) (map[string]bool, error) {
	booted := map[string]bool{}
	for _, udid := range ids {
		for _, device := range devices {
			if device.UDID != udid {
				continue
			}

			ok, err := r.preBoot(device)
			if err != nil {
				return nil, err
			}

			if ok {
				booted[device.UDID] = true
			}
		}
	}

	return booted, nil
}

// preBoot boots one destination armed if it is a shut-down iOS simulator nobody is booting already, and reports whether
// it did.
func (r *execRun) preBoot(device simctl.Device) (bool, error) {
	if rec, err := arm.ReadRecord(device.UDID); err == nil && arm.InProgress(rec) {
		fmt.Fprintf(os.Stderr, "stuntcam: %s is being booted armed by another stuntcam (pid %d); "+
			"not booting it again\n", device.Name, rec.Pid)
		return false, nil
	}

	if device.State == simctl.Shutdown && !isIOSRuntime(device.RuntimeID) {
		fmt.Fprintf(os.Stderr, "stuntcam: %s is a %s simulator: not booting it armed (stuntcam arms iOS simulators "+
			"only)\n", device.Name, runtimeName(device.RuntimeID))
		return false, nil
	}

	if device.State != simctl.Shutdown {
		return false, nil
	}

	fmt.Fprintf(os.Stderr, "stuntcam: booting %s (%s) armed for the command (xcodebuild's own boot would not be)\n",
		device.Name, device.UDID)
	// armed with its own directory (as `up` does), so `down`, `swap`, `status` and `sentinel` see it
	recordXcode(device.UDID, r.set.Rules, "exec", noXcodeSummary, signalsFromThisProcess)
	if err := arm.ArmBoot(ctl, device.UDID, r.set, arm.Vars(device.UDID, r.set, arm.Options{})); err != nil {
		return false, fmt.Errorf("booting %s armed: %w", device.Name, err)
	}

	if !springboardUp(device.UDID, springboardWait) {
		fmt.Fprintf(os.Stderr, "stuntcam: %s has no SpringBoard after %d s; running the command anyway\n", device.Name,
			int(springboardWait/time.Second))
	}

	return true, nil
}

// hookCandidate is a booted destination the runner hook may arm: its hook mode (hookLaunchd or hookUnarmed) and how
// the messages call it.
type hookCandidate struct {
	device simctl.Device
	mode   runnerHookMode
	what   string
}

// hookCandidates goes through the command's already-booted iOS destinations and returns the ones the runner hook may
// arm. A destination booted armed gets an inert hookBoot file when the command runs tests; a launchd-armed or unarmed
// one goes to runnerHookCandidate.
func (r *execRun) hookCandidates(ids []string, devices []simctl.Device, preBooted map[string]bool,
	xcodebuildCmd xbCommand) []hookCandidate {
	isDest := map[string]bool{}
	for _, udid := range ids {
		isDest[udid] = true
	}

	var candidates []hookCandidate
	for _, device := range devices {
		if !isDest[device.UDID] || device.State != simctl.Booted || !isIOSRuntime(device.RuntimeID) {
			continue
		}

		mode := hookBoot
		if !preBooted[device.UDID] {
			mode = hookMode(statusOf(device).Mode)
		}

		switch mode {
		case hookNone:
			continue
		case hookBoot:
			if xcodebuildCmd.runsTests() {
				r.writeBootHook(device)
			}
		case hookLaunchd, hookUnarmed:
			if candidate, ok := r.runnerHookCandidate(device, mode, xcodebuildCmd); ok {
				candidates = append(candidates, candidate)
			}
		}
	}

	return candidates
}

// writeBootHook writes a boot-armed destination's hookBoot file; the hook stays inert there.
func (r *execRun) writeBootHook(device simctl.Device) {
	if err := writeHookEnv(r.hookDir, device.UDID, hookBoot, nil); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v\n", err)
	}
}

// runnerHookCandidate says whether a launchd-armed or unarmed destination is a candidate for the runner hook: it is
// when the command is `xcodebuild test` and this build has the hook. Otherwise a warning says what the command's test
// launches there miss, except for a launchd-armed sim and a command that isn't xcodebuild: launchd arming already
// covers simctl launch, RN, Flutter and Detox.
func (r *execRun) runnerHookCandidate(device simctl.Device, mode runnerHookMode,
	xcodebuildCmd xbCommand) (hookCandidate, bool) {
	what := "booted unarmed"
	if mode == hookLaunchd {
		what = "launchd-armed"
	}

	if !xcodebuildCmd.runsTests() {
		if mode == hookLaunchd && !xcodebuildCmd.OK {
			return hookCandidate{}, false
		}

		fmt.Fprintf(os.Stderr, "stuntcam: %s is %s; Xcode test launches there get no camera unless the command is "+
			"`xcodebuild test` itself (the runner hook), and Xcode Run never does "+
			"(stuntcam up --udid %s --reboot first)\n", device.Name, what, device.UDID)
		return hookCandidate{}, false
	}

	if r.set.Hook == "" {
		fmt.Fprintf(os.Stderr, "stuntcam: %s is %s and this build has no runner hook (%s; rebuild with "+
			"./build.sh): the command's test launches there get no camera (stuntcam up --udid %s --reboot first)\n",
			device.Name, what, artifacts.HookName, device.UDID)
		return hookCandidate{}, false
	}

	return hookCandidate{device: device, mode: mode, what: what}, true
}

// setUpRunnerHook asks planRunnerHook whether the command's test runners get the hook, prints what it decided, and if
// so writes each candidate's hook file and adds the hook's variables to the command's environment.
func (r *execRun) setUpRunnerHook(xcodebuildCmd xbCommand, dest destSet, candidates []hookCandidate) {
	workDir, _ := os.Getwd() // "" if the working directory is gone: no scheme is found from it
	rules := ""
	if r.set.Rules != "" && r.xcode.Build != "" {
		rules = filepath.Join(r.set.Rules, r.xcode.Build+".json")
	}

	var names []string
	for _, c := range candidates {
		names = append(names, c.device.Name)
	}

	plan := planRunnerHook(hookInputs{
		Args:         xcodebuildCmd.Args,
		WrapEnv:      xcodebuildCmd.Env,
		WD:           workDir,
		Dest:         dest,
		Hooked:       names,
		Hook:         r.set.Hook,
		Dir:          r.hookDir,
		Stub:         r.set.Stub,
		Rules:        rules,
		RulesOK:      testRunnerRule(rules),
		XcodeBuild:   r.xcode.Build,
		DeveloperDir: r.xcode.DeveloperDir,
		Environ:      os.Environ(),
	})
	for _, n := range plan.Notes {
		fmt.Fprintln(os.Stderr, n)
	}

	if !plan.Export {
		return
	}

	for _, c := range candidates {
		r.hookDestination(c)
	}

	if err := os.MkdirAll(r.hookDir, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook\n", err)
	} else {
		r.env = append(r.env, plan.Env...)
	}
}

// hookDestination writes one candidate's hook file: for a launchd-armed sim the stub to insert (launchd gives the app
// every other variable), for an unarmed one every variable, after preparing it as `stuntcam env` does.
func (r *execRun) hookDestination(candidate hookCandidate) {
	device := candidate.device
	var hookVars []string
	if candidate.mode == hookLaunchd {
		// a failed read finds no stub: the set's stub is used
		cur, _ := ctl.Getenv(device.UDID, "DYLD_INSERT_LIBRARIES")
		stub := stubIn(cur, artifacts.StubName)
		if stub == "" {
			stub = r.set.Stub
		}

		hookVars = []string{"DYLD_INSERT_LIBRARIES=" + stub}
	} else {
		// stays env-armed afterwards, as after `stuntcam env` (cli/README.md, exec)
		if err := envArm(device, r.set, "exec"); err != nil {
			fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook there: its test launches get no camera\n", err)
			return
		}

		hookVars = arm.Vars(device.UDID, r.set, arm.Options{})
	}

	if err := writeHookEnv(r.hookDir, device.UDID, candidate.mode, hookVars); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook on %s\n", err, device.Name)
		return
	}

	r.hooked = append(r.hooked, device)
	fmt.Fprintf(os.Stderr, "stuntcam: %s is %s: UI tests' apps get the camera through the runner hook; a hosted "+
		"test's host app doesn't (stuntcam up --udid %s --reboot arms that too)\n", device.Name, candidate.what,
		device.UDID)
}

// runAndReport runs the command, forwarding signals, then reports the runner hook's launches and the camera's use,
// and returns the command's exit status.
func (r *execRun) runAndReport(cmdline, ids []string) error {
	// activations are counted in the run directory and in each -destination's own directory: one booted armed before
	// this command (by `up`, or an earlier exec's pre-boot) logs there, not in the run directory (otherwise a second
	// exec on the same destination would report no camera use)
	counted := append([]string{r.dir}, ids...)
	cmd := exec.Command(cmdline[0], cmdline[1:]...)
	cmd.Env = r.env
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	sigs := catchSignals()
	before := activations(counted) // a reused sim's directory has earlier activations
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v\n", err)
		if errors.Is(err, os.ErrPermission) {
			return exitStatus(exitCannotRun)
		}

		return exitStatus(exitNotFound)
	}

	forwardSignals(sigs, cmd)
	runErr := cmd.Wait()
	apps := cameraUse(counted, before)
	r.reportHook()
	if len(apps) > 0 {
		fmt.Fprintf(os.Stderr, "stuntcam: the camera was used by %s\n", strings.Join(apps, ", "))
	} else {
		fmt.Fprintln(os.Stderr, "stuntcam: no app used the camera in the command's simulators")
	}

	if exitErr, ok := runErr.(*exec.ExitError); ok {
		if status, ok := exitErr.Sys().(syscall.WaitStatus); ok && status.Signaled() {
			return exitStatus(exitSignalBase + int(status.Signal())) // as a shell reports a child killed by a signal
		}

		return exitStatus(exitErr.ExitCode())
	}

	signal.Stop(sigs)
	return runErr
}

// catchSignals starts catching the signals exec passes on to its command. stuntcam waits for the command and still
// reports.
//   - Signals that started out ignored, as under nohup or in a background job, stay ignored.
//   - SIGTERM and SIGHUP sent to stuntcam are passed on. Some CI runners send them.
//   - A terminal's Ctrl-C reaches the command by itself when stuntcam is the terminal's foreground job. So the first
//     SIGINT is passed on only otherwise, as for a `kill -INT` from a script with no terminal. A second one is always
//     passed on.
//
// Catching stays on through the report. When stuntcam passes the command's status on, it stays on until stuntcam
// exits.
func catchSignals() chan os.Signal {
	var watch []os.Signal
	for _, sig := range []os.Signal{os.Interrupt, syscall.SIGTERM, syscall.SIGHUP} {
		if !signal.Ignored(sig) {
			watch = append(watch, sig)
		}
	}

	sigs := make(chan os.Signal, 2)
	if len(watch) > 0 {
		signal.Notify(sigs, watch...)
	}

	return sigs
}

// forwardSignals passes the caught signals on to the started command, as catchSignals says.
func forwardSignals(sigs chan os.Signal, cmd *exec.Cmd) {
	tty := inForegroundOfTerminal() // then a terminal Ctrl-C reaches the child too
	go func() {
		interrupts := 0
		for sig := range sigs {
			if sig == os.Interrupt {
				interrupts++
				if tty && interrupts == 1 {
					continue
				}
			}

			if cmd.Process != nil {
				_ = cmd.Process.Signal(sig) // it may have exited already
			}
		}
	}()
}

// cameraUse lists, sorted, each app with activations in the counted directories since before: "<bundle> (<n>
// activations)".
func cameraUse(counted []string, before map[string]int) []string {
	var apps []string
	for k, n := range activations(counted) {
		if d := n - before[k]; d > 0 {
			apps = append(apps, fmt.Sprintf("%s (%d %s)", k, d, plural(d, "activation", "activations")))
		}
	}
	sort.Strings(apps)
	return apps
}

// reportHook prints, per simulator the runner hook ran on, the app launches it armed and its failures.
func (r *execRun) reportHook() {
	names := map[string]string{}
	for _, device := range r.hooked {
		names[device.UDID] = device.Name
	}

	report := hookReport(r.hookDir)
	for _, udid := range sortedKeys(report) {
		count := report[udid]
		name := names[udid]
		if name == "" {
			name = udid
		}

		if count.Armed > 0 || names[udid] != "" {
			fmt.Fprintf(os.Stderr, "stuntcam: runner hook on %s: %d app %s armed\n", name, count.Armed,
				plural(count.Armed, "launch", "launches"))
		}

		for _, f := range count.Failures {
			fmt.Fprintf(os.Stderr, "stuntcam: runner hook on %s %s\n", name, f)
		}
	}
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

// inForegroundOfTerminal reports whether this process's group is the foreground group of its controlling terminal: then
// a Ctrl-C there is delivered to the whole group (the child included) and must not be forwarded again. No terminal, or
// a background job: false.
func inForegroundOfTerminal() bool {
	tty, err := os.Open("/dev/tty")
	if err != nil {
		return false
	}
	defer tty.Close()

	var foreground int32
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, tty.Fd(), uintptr(syscall.TIOCGPGRP),
		uintptr(unsafe.Pointer(&foreground)))
	if errno != 0 {
		return false
	}

	return int(foreground) == syscall.Getpgrp()
}
