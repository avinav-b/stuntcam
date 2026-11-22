// SPDX-License-Identifier: Apache-2.0

package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/service"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

const (
	servicePoll    = 2 * time.Second // how often the login service looks at the simulators
	bootstrapTries = 5               // launchctl bootstrap attempts, bootstrapPause apart
	bootstrapPause = time.Second
	// agentStartWait is how long startAgent waits after the bootstrap before it checks that the agent runs.
	agentStartWait = time.Second
)

func cmdService(args []string) error {
	if len(args) == 0 {
		return usagef("usage: stuntcam service install|run (--only U … | --all) [--preboot U] | uninstall | status")
	}

	flags := flag.NewFlagSet("service", flag.ContinueOnError)
	preboot := flags.String("preboot", "", "boot this simulator armed when the service starts (at login)")
	var only multi
	flags.Var(&only, "only", "arm only this simulator (repeatable)")
	all := flags.Bool("all", false,
		"arm every iOS simulator that boots unarmed (on a shared Mac this includes other people's)")
	if err := parse(flags, args[1:], 0); err != nil {
		return err
	}

	if (args[0] == "install" || args[0] == "run") && len(only) == 0 && !*all {
		return usagef("service %s needs --only <udid> (repeatable) or --all: it arms simulators that boot without "+
			"stuntcam, and other tools' or people's simulators may be booted on this Mac", args[0])
	}

	if len(only) > 0 && *all {
		return usagef("--only and --all exclude each other")
	}

	for i, u := range only {
		only[i] = strings.ToUpper(u)
	}

	uid := fmt.Sprint(os.Getuid())
	switch args[0] {
	case "install":
		return serviceInstall(uid, *preboot, only)
	case "uninstall":
		return serviceUninstall(uid)
	case "status":
		serviceStatus(uid)
		return nil
	case "run":
		return serviceRun(*preboot, only)
	}

	return usagef("unknown service action %q (install, uninstall, status, run)", args[0])
}

// serviceInstall installs and starts the login agent (`service run` with the same build, Xcode and host directory as
// this shell).
func serviceInstall(uid, preboot string, only []string) error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}

	set, err := artifacts.Find("")
	if err != nil {
		return err
	}

	if set, err = artifacts.Stage(set); err != nil {
		return err
	}

	env := map[string]string{"STUNTCAM_BUILD": filepath.Dir(set.Loader), "STUNTCAM_STUB": set.Stub,
		"STUNTCAM_PROFILE": set.Profile}
	if set.Image != "" {
		env["STUNTCAM_IMAGE"] = set.Image
	}

	warnUnknownSims(only)
	if preboot != "" && len(only) > 0 && !containsFold(only, preboot) {
		return usagef("--preboot %s is not one of the --only simulators", preboot)
	}

	for _, k := range []string{"SIMCAM_HOME", "DEVELOPER_DIR"} { // the same Xcode and host directory as this shell
		if v := os.Getenv(k); v != "" {
			env[k] = v
		}
	}

	if home.Protected(exe) {
		return fmt.Errorf("the stuntcam binary is in a privacy-protected folder (%s); a login agent can't run it "+
			"from there", exe)
	}

	// launchd won't start a job whose log dir is missing
	for _, dir := range []string{filepath.Dir(service.PlistPath()), home.Logs()} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}

	// fails if the agent is not loaded, which is fine
	_ = exec.Command("launchctl", "bootout", "gui/"+uid+"/"+service.Label).Run()
	if err := home.WriteAtomic(service.PlistPath(), []byte(service.Plist(exe, env, preboot, only))); err != nil {
		return err
	}

	if err := startAgent(uid); err != nil {
		return err
	}

	fmt.Printf("installed %s (%s): it arms simulators that boot without stuntcam and tells you when Xcode Run needs "+
		"`stuntcam up`\n", service.Label, service.PlistPath())
	return nil
}

// warnUnknownSims warns about each --only UDID that isn't a simulator on this Mac (if the list can be read).
func warnUnknownSims(only []string) {
	devices, err := ctl.Devices()
	if err != nil {
		return
	}

	for _, udid := range only {
		found := false
		for _, device := range devices {
			found = found || device.UDID == udid
		}

		if !found {
			fmt.Fprintf(os.Stderr, "warning: --only %s is not a simulator on this Mac (xcrun simctl list devices)\n",
				udid)
		}
	}
}

// startAgent bootstraps the installed plist and checks, a second later, that the agent runs.
func startAgent(uid string) error {
	var out []byte
	var err error
	// right after a bootout, bootstrap can fail with "5: Input/output error" for a moment
	for try := 0; try < bootstrapTries; try++ {
		if out, err = exec.Command("launchctl", "bootstrap", "gui/"+uid, service.PlistPath()).CombinedOutput(); err == nil {
			break
		}

		time.Sleep(bootstrapPause)
	}

	if err != nil {
		return fmt.Errorf("launchctl bootstrap: %w: %s", err, out)
	}

	time.Sleep(agentStartWait)
	st, err := exec.Command("launchctl", "print", "gui/"+uid+"/"+service.Label).CombinedOutput()
	if err != nil || !strings.Contains(string(st), "state = running") {
		return fmt.Errorf("installed, but %s is not running; see %s", service.Label,
			filepath.Join(home.Logs(), "service.log"))
	}

	return nil
}

// serviceUninstall stops the login agent and removes its plist.
func serviceUninstall(uid string) error {
	// fails if the agent is not loaded, which is fine
	_ = exec.Command("launchctl", "bootout", "gui/"+uid+"/"+service.Label).Run()
	if err := os.Remove(service.PlistPath()); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	fmt.Println("uninstalled", service.Label, "(simulators it armed in launchd mode stay armed until `stuntcam down` "+
		"or a shutdown; its log stays in "+home.Logs()+")")
	return nil
}

// serviceStatus prints launchd's state, pid, program and last exit code of the agent, or "not running".
func serviceStatus(uid string) {
	out, err := exec.Command("launchctl", "print", "gui/"+uid+"/"+service.Label).CombinedOutput()
	if err != nil {
		fmt.Println("not running")
		return
	}

	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		for _, key := range []string{"state =", "pid =", "program =", "last exit code ="} {
			if strings.HasPrefix(line, key) {
				fmt.Println(line)
				break
			}
		}
	}
}

func serviceRun(preboot string, only []string) error {
	set, err := artifacts.Find("")
	if err != nil {
		return err
	}

	if set, err = artifacts.Stage(set); err != nil {
		return err
	}

	log := func(f string, a ...any) {
		fmt.Printf("%s %s\n", time.Now().Format("2006-01-02 15:04:05"), fmt.Sprintf(f, a...))
	}

	preboot = strings.ToUpper(preboot)
	if preboot != "" && len(only) > 0 && !containsFold(only, preboot) {
		log("preboot %s skipped: not one of --only", preboot)
	} else if preboot != "" {
		if _, err := os.Stat(arm.DownMarker(strings.ToUpper(preboot))); err == nil {
			log("preboot %s skipped: `stuntcam down` was run on it", preboot)
		} else if device, _, err := pickSim(preboot); err != nil {
			log("preboot %s: %v", preboot, err)
		} else if device.State != simctl.Booted {
			recordXcode(device.UDID, set.Rules, "service", noXcodeSummary, signalsFromThisProcess)
			err = arm.ArmBoot(ctl, device.UDID, set, arm.Vars(device.UDID, set, arm.Options{}))
			log("preboot %s: %v", device.Name, errText(err))
		}
	}

	handled := map[string]time.Time{}
	// The service leaves these sims alone:
	//   - sims armed by stuntcam, in boot or launchd mode;
	//   - sims armed by the scripts ("armed");
	//   - sims being armed right now, since a boot record is written before the boot;
	//   - sims the user ran `stuntcam down` on, until their next `up`.
	armed := func(device simctl.Device) service.State {
		if _, err := os.Stat(arm.DownMarker(device.UDID)); err == nil {
			return service.IsArmed
		}

		switch statusOf(device).Mode {
		case modeBoot, modeLaunchd, modeScriptArmed:
			return service.IsArmed
		case modeBooting: // `up` or `exec` is arming it: look again next time, so a boot that fails is still covered
			return service.Pending
		}

		return service.Unarmed
	}

	log("watching simulators (build %s)", set.BuildID())
	for {
		devices, err := ctl.Devices()
		if err == nil {
			for _, device := range service.ToArm(devices, armed, handled, only...) {
				handled[device.UDID] = device.LastBooted
				recordXcode(device.UDID, set.Rules, "service", noXcodeSummary, signalsFromLaunchdSim)
				err := arm.ArmLaunchd(ctl, device.UDID, set, arm.Vars(device.UDID, set, arm.Options{}))
				result := "armed"
				if err != nil {
					result = "failed: " + errText(err)
				}

				log("%s (%s) booted unarmed: launchd mode %s", device.Name, device.UDID, result)
				if err == nil {
					title, body := service.Notice(device)
					// a notice that cannot be shown changes nothing
					_ = exec.Command("osascript", "-e",
						fmt.Sprintf("display notification %q with title %q", body, title)).Run()
				}
			}
		}

		time.Sleep(servicePoll)
	}
}

// containsFold reports whether xs has x, ignoring case (simctl prints UDIDs in upper case; xcodebuild takes either).
func containsFold(xs []string, x string) bool {
	for _, v := range xs {
		if strings.EqualFold(v, x) {
			return true
		}
	}
	return false
}
