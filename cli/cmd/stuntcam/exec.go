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
	r, err := newExecRun(set)
	if err != nil {
		return err
	}
	xcodebuildCmd := parseXcodebuild(cmdline)
	runsTests := xcodebuildCmd.runsTests()
	ids := destinationIDs(cmdline)
	var candidates []hookCandidate // launchd-armed or unarmed iOS destinations, hooked only if planRunnerHook says so
	var dest destSet
	if ds, err := ctl.Devices(); err == nil {
		dest = destinations(cmdline, ds)
		preBooted, err := r.preBootDestinations(ids, ds)
		if err != nil {
			return err
		}
		candidates = r.hookCandidates(ids, ds, preBooted, xcodebuildCmd)
	}
	if runsTests && set.Hook != "" && (len(candidates) > 0 || len(dest.Named) > 0) {
		r.setUpRunnerHook(xcodebuildCmd, dest, candidates)
	}
	return r.runAndReport(cmdline, ids)
}

// parseExecArgs splits exec's arguments into the command line (after an optional "--") and --build.
func parseExecArgs(args []string) (cmdline []string, build string, err error) {
	fs := flag.NewFlagSet("exec", flag.ContinueOnError)
	buildFlag := fs.String("build", "", "build dir")
	if err := fs.Parse(args); err != nil { // the command after "--" keeps its own options
		return nil, "", usageError{err}
	}
	cmdline = fs.Args()
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
	r := &execRun{set: set, dir: fmt.Sprintf("exec-%d-%d", time.Now().UnixNano(), os.Getpid())}
	if err := arm.Prepare(r.dir, set); err != nil {
		return nil, err
	}
	r.xcode = recordXcode(r.dir, set.Rules, "exec", false, true)
	r.env = os.Environ()
	for _, kv := range arm.Vars(r.dir, set, arm.Options{}) {
		k, v, _ := strings.Cut(kv, "=")
		if k == "DYLD_INSERT_LIBRARIES" { // keep another tool's insert the caller already exported
			v = arm.MergeInsert(os.Getenv("SIMCTL_CHILD_DYLD_INSERT_LIBRARIES"), v)
		}
		r.env = append(r.env, "SIMCTL_CHILD_"+k+"="+v)
	}
	r.hookDir = filepath.Join(home.Sim(r.dir), "hook")
	return r, nil
}

// preBootDestinations boots armed each shut-down iOS simulator among the command's -destination ids and returns the
// ones it booted. xcodebuild boots a shut-down -destination itself, and that boot does not get the SIMCTL_CHILD_
// variables (seen on Xcode 27.0: no stuntcam variable in the sim, no stub in the test host); so it is booted armed
// before the command runs. iOS simulators only: stuntcam arms nothing else.
func (r *execRun) preBootDestinations(ids []string, ds []simctl.Device) (map[string]bool, error) {
	booted := map[string]bool{}
	for _, u := range ids {
		for _, d := range ds {
			if d.UDID != u {
				continue
			}
			ok, err := r.preBoot(d)
			if err != nil {
				return nil, err
			}
			if ok {
				booted[d.UDID] = true
			}
		}
	}
	return booted, nil
}

// preBoot boots one destination armed if it is a shut-down iOS simulator nobody is booting already, and reports whether
// it did.
func (r *execRun) preBoot(d simctl.Device) (bool, error) {
	if rec, err := arm.ReadRecord(d.UDID); err == nil && arm.InProgress(rec) {
		fmt.Fprintf(os.Stderr, "stuntcam: %s is being booted armed by another stuntcam (pid %d); not booting it again\n",
			d.Name, rec.Pid)
		return false, nil
	}
	if d.State == "Shutdown" && !isIOSRuntime(d.RuntimeID) {
		fmt.Fprintf(os.Stderr, "stuntcam: %s is a %s simulator: not booting it armed (stuntcam arms iOS simulators only)\n",
			d.Name, runtimeName(d.RuntimeID))
		return false, nil
	}
	if d.State != "Shutdown" {
		return false, nil
	}
	fmt.Fprintf(os.Stderr, "stuntcam: booting %s (%s) armed for the command (xcodebuild's own boot would not be)\n",
		d.Name, d.UDID)
	// armed with its own directory (as `up` does), so `down`, `swap`, `status` and `sentinel` see it
	recordXcode(d.UDID, r.set.Rules, "exec", false, true)
	if err := arm.ArmBoot(ctl, d.UDID, r.set, arm.Vars(d.UDID, r.set, arm.Options{})); err != nil {
		return false, fmt.Errorf("booting %s armed: %w", d.Name, err)
	}
	if !springboardUp(d.UDID, springboardWait) {
		fmt.Fprintf(os.Stderr, "stuntcam: %s has no SpringBoard after %d s; running the command anyway\n", d.Name,
			int(springboardWait/time.Second))
	}
	return true, nil
}

// hookCandidate is a booted destination the runner hook may arm: its hook mode ("launchd" or "unarmed") and how
// the messages call it.
type hookCandidate struct {
	d          simctl.Device
	mode, what string
}

// hookCandidates goes through the command's already-booted iOS destinations: a boot-armed one gets a "boot" hook file
// when the command runs tests (the hook is inert there); a launchd-armed or unarmed one is a candidate for the runner
// hook when the command is `xcodebuild test` and this build has the hook, else a warning says what its test launches
// miss.
func (r *execRun) hookCandidates(ids []string, ds []simctl.Device, preBooted map[string]bool,
	xcodebuildCmd xbCommand) []hookCandidate {
	runsTests := xcodebuildCmd.runsTests()
	isDest := map[string]bool{}
	for _, u := range ids {
		isDest[u] = true
	}
	var candidates []hookCandidate
	for _, d := range ds {
		if !isDest[d.UDID] || d.State != "Booted" || !isIOSRuntime(d.RuntimeID) {
			continue
		}
		m := "boot"
		if !preBooted[d.UDID] {
			m = hookMode(statusOf(d).Mode)
		}
		what := map[string]string{"launchd": "launchd-armed", "unarmed": "booted unarmed"}[m]
		switch {
		case m == "" || (m == "boot" && !runsTests):
		case m == "boot":
			if err := writeHookEnv(r.hookDir, d.UDID, "boot", nil); err != nil {
				fmt.Fprintf(os.Stderr, "stuntcam: %v\n", err)
			}
		case !runsTests && m == "launchd" && !xcodebuildCmd.OK:
			// launchd arming already covers simctl launch, RN, Flutter, Detox
		case !runsTests:
			fmt.Fprintf(os.Stderr, "stuntcam: %s is %s; Xcode test launches there get no camera unless the command is "+
				"`xcodebuild test` itself (the runner hook), and Xcode Run never does (stuntcam up --udid %s --reboot first)\n",
				d.Name, what, d.UDID)
		case r.set.Hook == "":
			fmt.Fprintf(os.Stderr, "stuntcam: %s is %s and this build has no runner hook (%s; rebuild with ./build.sh): the "+
				"command's test launches there get no camera (stuntcam up --udid %s --reboot first)\n",
				d.Name, what, artifacts.HookName, d.UDID)
		default:
			candidates = append(candidates, hookCandidate{d, m, what})
		}
	}
	return candidates
}

// setUpRunnerHook asks planRunnerHook whether the command's test runners get the hook, prints what it decided, and if
// so writes each candidate's hook file and adds the hook's variables to the command's environment.
func (r *execRun) setUpRunnerHook(xcodebuildCmd xbCommand, dest destSet, candidates []hookCandidate) {
	wd, _ := os.Getwd()
	rules := ""
	if r.set.Rules != "" && r.xcode.Build != "" {
		rules = filepath.Join(r.set.Rules, r.xcode.Build+".json")
	}
	var names []string
	for _, c := range candidates {
		names = append(names, c.d.Name)
	}
	plan := planRunnerHook(hookInputs{Args: xcodebuildCmd.Args, WrapEnv: xcodebuildCmd.Env, WD: wd, Dest: dest,
		Hooked: names, Hook: r.set.Hook, Dir: r.hookDir, Stub: r.set.Stub, Rules: rules, RulesOK: testRunnerRule(rules),
		XcodeBuild: r.xcode.Build, DeveloperDir: r.xcode.DeveloperDir, Environ: os.Environ()})
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
func (r *execRun) hookDestination(c hookCandidate) {
	d := c.d
	var hookVars []string
	if c.mode == "launchd" {
		cur, _ := ctl.Getenv(d.UDID, "DYLD_INSERT_LIBRARIES")
		stub := stubIn(cur, artifacts.StubName)
		if stub == "" {
			stub = r.set.Stub
		}
		hookVars = []string{"DYLD_INSERT_LIBRARIES=" + stub}
	} else {
		// stays env-armed afterwards, as after `stuntcam env` (cli/README.md, exec)
		if err := envArm(d, r.set, "exec"); err != nil {
			fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook there: its test launches get no camera\n", err)
			return
		}
		hookVars = arm.Vars(d.UDID, r.set, arm.Options{})
	}
	if err := writeHookEnv(r.hookDir, d.UDID, c.mode, hookVars); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v; no runner hook on %s\n", err, d.Name)
		return
	}
	r.hooked = append(r.hooked, d)
	fmt.Fprintf(os.Stderr, "stuntcam: %s is %s: UI tests' apps get the camera through the runner hook; a hosted test's "+
		"host app doesn't (stuntcam up --udid %s --reboot arms that too)\n", d.Name, c.what, d.UDID)
}

// runAndReport runs the command, forwarding signals, then reports the runner hook's launches and the camera's use,
// and returns the command's exit status.
func (r *execRun) runAndReport(cmdline, ids []string) error {
	// activations are counted in the run directory and in each -destination's own directory: one booted armed before this
	// command (by `up`, or an earlier exec's pre-boot) logs there, not in the run directory (otherwise a second exec on
	// the same destination would report no camera use)
	counted := append([]string{r.dir}, ids...)
	c := exec.Command(cmdline[0], cmdline[1:]...)
	c.Env, c.Stdin, c.Stdout, c.Stderr = r.env, os.Stdin, os.Stdout, os.Stderr
	sigs := catchSignals()
	before := activations(counted) // a reused sim's directory has earlier activations
	if err := c.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "stuntcam: %v\n", err)
		if errors.Is(err, os.ErrPermission) {
			return exitStatus(exitCannotRun)
		}
		return exitStatus(exitNotFound)
	}
	forwardSignals(sigs, c)
	runErr := c.Wait()
	apps := cameraUse(counted, before)
	r.reportHook()
	if len(apps) > 0 {
		fmt.Fprintf(os.Stderr, "stuntcam: the camera was used by %s\n", strings.Join(apps, ", "))
	} else {
		fmt.Fprintln(os.Stderr, "stuntcam: no app used the camera in the command's simulators")
	}
	if ee, ok := runErr.(*exec.ExitError); ok {
		if ws, ok := ee.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
			return exitStatus(exitSignalBase + int(ws.Signal())) // as a shell reports a child killed by a signal
		}
		return exitStatus(ee.ExitCode())
	}
	signal.Stop(sigs)
	return runErr
}

// catchSignals starts catching the signals exec passes on to its command. stuntcam waits for the command and still
// reports. Signals that started out ignored (nohup, a background job) stay ignored. SIGTERM/HUP sent to stuntcam (some
// CI runners) are passed on. A terminal Ctrl-C reaches the command by itself when stuntcam is the terminal's foreground
// job, so the first SIGINT is passed on only otherwise (a `kill -INT` from a script, no terminal); a second one always
// is. Catching stays on through the report and, when stuntcam passes the command's status on, until it exits.
func catchSignals() chan os.Signal {
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
	return sigs
}

// forwardSignals passes the caught signals on to the started command, as catchSignals says.
func forwardSignals(sigs chan os.Signal, c *exec.Cmd) {
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
	for _, d := range r.hooked {
		names[d.UDID] = d.Name
	}
	report := hookReport(r.hookDir)
	for _, u := range sortedKeys(report) {
		count := report[u]
		n := names[u]
		if n == "" {
			n = u
		}
		if count.Armed > 0 || names[u] != "" {
			fmt.Fprintf(os.Stderr, "stuntcam: runner hook on %s: %d app %s armed\n", n, count.Armed,
				plural(count.Armed, "launch", "launches"))
		}
		for _, f := range count.Failures {
			fmt.Fprintf(os.Stderr, "stuntcam: runner hook on %s %s\n", n, f)
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
	t, err := os.Open("/dev/tty")
	if err != nil {
		return false
	}
	defer t.Close()
	var pg int32
	_, _, e := syscall.Syscall(syscall.SYS_IOCTL, t.Fd(), uintptr(syscall.TIOCGPGRP), uintptr(unsafe.Pointer(&pg)))
	if e != 0 {
		return false
	}
	return int(pg) == syscall.Getpgrp()
}
