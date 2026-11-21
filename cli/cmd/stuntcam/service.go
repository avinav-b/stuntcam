// SPDX-License-Identifier: Apache-2.0

package main

import (
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
	bootstrapTries = 5               // launchctl bootstrap attempts, a second apart
)

func cmdService(args []string) error {
	if len(args) == 0 {
		return usagef("usage: stuntcam service install|run (--only U … | --all) [--preboot U] | uninstall | status")
	}
	fs := flag.NewFlagSet("service", flag.ContinueOnError)
	preboot := fs.String("preboot", "", "boot this simulator armed when the service starts (at login)")
	var only multi
	fs.Var(&only, "only", "arm only this simulator (repeatable)")
	all := fs.Bool("all", false,
		"arm every iOS simulator that boots unarmed (on a shared Mac this includes other people's)")
	if err := parse(fs, args[1:], 0); err != nil {
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
		return fmt.Errorf("the stuntcam binary is in a privacy-protected folder (%s); a login agent can't run it from there",
			exe)
	}
	// launchd won't start a job whose log dir is missing
	for _, d := range []string{filepath.Dir(service.PlistPath()), home.Logs()} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
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
	ds, err := ctl.Devices()
	if err != nil {
		return
	}
	for _, u := range only {
		found := false
		for _, d := range ds {
			found = found || d.UDID == u
		}
		if !found {
			fmt.Fprintf(os.Stderr, "warning: --only %s is not a simulator on this Mac (xcrun simctl list devices)\n", u)
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
		time.Sleep(time.Second)
	}
	if err != nil {
		return fmt.Errorf("launchctl bootstrap: %v: %s", err, out)
	}
	time.Sleep(time.Second)
	st, err := exec.Command("launchctl", "print", "gui/"+uid+"/"+service.Label).CombinedOutput()
	if err != nil || !strings.Contains(string(st), "state = running") {
		return fmt.Errorf("installed, but %s is not running; see %s", service.Label,
			filepath.Join(home.Logs(), "service.log"))
	}
	return nil
}

// serviceUninstall stops the login agent and removes its plist.
func serviceUninstall(uid string) error {
	_ = exec.Command("launchctl", "bootout", "gui/"+uid+"/"+service.Label).Run()
	if err := os.Remove(service.PlistPath()); err != nil && !os.IsNotExist(err) {
		return err
	}
	fmt.Println("uninstalled", service.Label, "(simulators it armed in launchd mode stay armed until `stuntcam down` or "+
		"a shutdown; its log stays in "+home.Logs()+")")
	return nil
}

// serviceStatus prints launchd's state, pid, program and last exit code of the agent, or "not running".
func serviceStatus(uid string) {
	out, err := exec.Command("launchctl", "print", "gui/"+uid+"/"+service.Label).CombinedOutput()
	if err != nil {
		fmt.Println("not running")
		return
	}
	for _, l := range strings.Split(string(out), "\n") {
		l = strings.TrimSpace(l)
		for _, key := range []string{"state =", "pid =", "program =", "last exit code ="} {
			if strings.HasPrefix(l, key) {
				fmt.Println(l)
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
		} else if d, _, err := pickSim(preboot); err != nil {
			log("preboot %s: %v", preboot, err)
		} else if d.State != "Booted" {
			recordXcode(d.UDID, set.Rules, "service", false, true)
			err = arm.ArmBoot(ctl, d.UDID, set, arm.Vars(d.UDID, set, arm.Options{}))
			log("preboot %s: %v", d.Name, errText(err))
		}
	}
	handled := map[string]time.Time{}
	// leave alone: armed by stuntcam (boot or launchd), armed by the scripts ("armed"), being armed right now (a boot
	// record is written before the boot), and sims the user ran `stuntcam down` on (until their next `up`)
	armed := func(d simctl.Device) service.State {
		if _, err := os.Stat(arm.DownMarker(d.UDID)); err == nil {
			return service.IsArmed
		}
		switch statusOf(d).Mode {
		case modeBoot, modeLaunchd, modeScriptArmed:
			return service.IsArmed
		case modeBooting: // `up` or `exec` is arming it: look again next time, so a boot that fails is still covered
			return service.Pending
		}
		return service.Unarmed
	}
	log("watching simulators (build %s)", set.BuildID())
	for {
		ds, err := ctl.Devices()
		if err == nil {
			for _, d := range service.ToArm(ds, armed, handled, only...) {
				handled[d.UDID] = d.LastBooted
				recordXcode(d.UDID, set.Rules, "service", false, false)
				err := arm.ArmLaunchd(ctl, d.UDID, set, arm.Vars(d.UDID, set, arm.Options{}))
				result := "armed"
				if err != nil {
					result = "failed: " + errText(err)
				}
				log("%s (%s) booted unarmed: launchd mode %s", d.Name, d.UDID, result)
				if err == nil {
					title, body := service.Notice(d)
					_ = exec.Command("osascript", "-e", fmt.Sprintf("display notification %q with title %q", body, title)).Run()
				}
			}
		}
		time.Sleep(servicePoll)
	}
}

// containsFold: xs has x, ignoring case (simctl prints UDIDs in upper case; xcodebuild takes either case).
func containsFold(xs []string, x string) bool {
	for _, v := range xs {
		if strings.EqualFold(v, x) {
			return true
		}
	}
	return false
}
