// SPDX-License-Identifier: Apache-2.0
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/webcam"
)

// newWebcamEnv is the camera helper's environment. The home is cleaned the way arm.Vars builds SIMCAM_SIM_DIR, from
// which the engine derives the home it hashes into ring names.
func newWebcamEnv() *webcam.Env { return webcam.Real(filepath.Clean(home.Root()), os.Stdout) }

// webcamSpec reports whether spec is the webcam source, and returns the camera NAME after "webcam:" ("" for the default
// camera).
func webcamSpec(spec string) (name string, ok bool) {
	if spec == "webcam" {
		return "", true
	}
	if n, ok := strings.CutPrefix(spec, "webcam:"); ok && n != "" {
		return n, true
	}
	return "", false
}

// startWebcam resolves the camera (NAME, or --front/--back: the engine feeds one camera to both positions) against
// the helper's camera list, once, makes sure a helper that serves this engine runs for it and has camera access, and
// records it as the default camera. Returns the camera's uniqueID: the source is webcam:<uniqueID> from then on.
func startWebcam(env *webcam.Env, set artifacts.Set, name, front, back string) (string, error) {
	if set.Helper == "" {
		return "", errors.New("this build has no camera helper (in a checkout: helper/build.sh; an installed " +
			"stuntcam ships one)")
	}

	if !filepath.IsAbs(env.Home) {
		return "", fmt.Errorf("the host directory %s is not an absolute path", env.Home)
	}

	var names []string
	for _, camera := range []string{name, front, back} {
		if camera != "" {
			names = append(names, camera)
		}
	}

	if len(names) == 0 {
		names = []string{""}
	}

	var cameras []webcam.Camera
	for _, camera := range names {
		if camera != webcam.SyntheticID {
			var err error
			if cameras, err = env.List(set.Helper); err != nil {
				return "", fmt.Errorf("listing the Mac's cameras: %w", err)
			}

			break
		}
	}

	var picked []webcam.Camera
	for _, camera := range names {
		resolved, err := webcam.Resolve(cameras, camera)
		if err != nil {
			return "", usageError{err}
		}

		if len(picked) > 0 && picked[0].ID != resolved.ID {
			return "", usagef("--front, --back and webcam:NAME name different cameras (%s, %s): this engine feeds "+
				"one camera to both positions", picked[0].ID, resolved.ID)
		}

		picked = append(picked, resolved)
	}

	chosen := picked[0]
	if chosen.Continuity {
		fmt.Printf("note: %s is a Continuity Camera: opening it wakes the phone\n", chosen.Name)
	}

	if _, err := env.Ensure(set.Helper, chosen); err != nil {
		return "", err
	}

	if chosen.ID != webcam.SyntheticID {
		if err := env.WriteDefault(chosen.ID); err != nil {
			return "", err
		}
	}

	return chosen.ID, nil
}

// controlSource is the source= line of a simulator's control file ("" if none).
func controlSource(udid string) string {
	data, _ := os.ReadFile(home.ControlFile(udid)) // no control file: no source line
	for _, line := range strings.Split(string(data), "\n") {
		if source, ok := strings.CutPrefix(strings.TrimSpace(line), "source="); ok {
			return source
		}
	}
	return ""
}

// webcamInUse says whether another simulator still uses its webcam session: booted (if the list can be read) and its
// control file still selecting webcam.
func webcamInUse() func(udid string) bool {
	booted := map[string]bool{}
	devices, err := ctl.Devices()
	for _, device := range devices {
		if device.State == simctl.Booted {
			booted[device.UDID] = true
		}
	}

	return func(udid string) bool {
		if err == nil && !booted[udid] {
			return false
		}

		_, ok := webcamSpec(controlSource(udid))
		return ok
	}
}

// endWebcam ends simulator udid's webcam session: its helpers stop unless another simulator still uses them.
func endWebcam(udid string) error {
	env := newWebcamEnv()
	if len(webcam.ReadSession(env.Home, udid)) == 0 {
		return nil
	}
	return env.Switch(udid, nil, webcamInUse())
}

// webcamDoctor adds doctor's webcam lines: the helper of this build (signature, Gatekeeper, ids), every helper's state
// and permission, ring-version and shm-owner problems, and the simulators with a webcam session.
func webcamDoctor(set artifacts.Set, setErr error, report *doctorReport) {
	env := newWebcamEnv()
	if setErr == nil {
		doctorBuildHelper(env, set, report)
	}

	pidFiles := webcam.PidFiles(env.Home)
	for _, cam := range pidFiles {
		doctorRunningHelper(env, cam, report)
	}

	if len(pidFiles) == 0 {
		report.info("webcam: no camera helper running (`stuntcam source webcam` starts one)")
	}

	doctorWebcamSessions(env, report)
	if os.Getenv("SSH_CONNECTION") != "" || os.Getenv("SSH_TTY") != "" {
		report.info("webcam: over ssh (and on headless CI) nobody can answer the camera prompt: use `video:PATH` or " +
			"`webcam:synthetic` there")
	}
}

// doctorBuildHelper reports this build's camera helper: its signature, then its bundle id, team and Gatekeeper
// verdict.
func doctorBuildHelper(env *webcam.Env, set artifacts.Set, report *doctorReport) {
	if set.Helper == "" {
		report.info("webcam: this build has no camera helper (in a checkout: helper/build.sh)")
		return
	}

	err := env.Verify(set.Helper)
	report.check(err == nil, fmt.Sprintf("webcam helper %s: %s (codesign --verify --strict)%s", set.HelperSHA(),
		set.Helper, errText(err)))
	if err != nil {
		return
	}

	// it exits non-zero for an unsigned helper, and its output then has no Identifier=
	codesignOut, _ := exec.Command("/usr/bin/codesign", "-dv", set.Helper).CombinedOutput()
	id := field(string(codesignOut), "Identifier=")
	team := field(string(codesignOut), "TeamIdentifier=")
	report.info(fmt.Sprintf("webcam helper bundle id %s, team %s; Gatekeeper (spctl -a -t exec): %s", id,
		team, gatekeeperVerdict(set.Helper)))
}

// gatekeeperVerdict is spctl's assessment of the helper app, with the source it names (such as "Notarized
// Developer ID").
func gatekeeperVerdict(helper string) string {
	spctl := exec.Command("/usr/sbin/spctl", "-a", "-t", "exec", "-vv", helper)
	spctlOut, spctlErr := spctl.CombinedOutput()
	verdict := "accepted"
	if spctlErr != nil {
		verdict = "not accepted (expected for a dev build)"
	}

	if source := field(string(spctlOut), "source="); source != "" {
		verdict += ", " + source
	}

	return verdict
}

// doctorRunningHelper reports the camera helper of one camera from its pid file: a problem first, then its state for
// a live helper, then shm names another user owns.
func doctorRunningHelper(env *webcam.Env, cam string, report *doctorReport) {
	pidFile, live, err := env.ReadPid(cam)
	switch {
	case err != nil:
		report.warn(fmt.Sprintf("webcam: %s: %v", webcam.PidPath(env.Home, cam), err))
		return
	case pidFile.Auth == "denied":
		report.warn("webcam: denied (" + webcam.SettingsPath + ")")
	case !live:
		report.info(fmt.Sprintf("webcam: the camera helper for %s exited (pid %d); `stuntcam source webcam` "+
			"starts it again, `stuntcam gc` removes the pid file", pidFile.Camera, pidFile.Pid))
		return
	case webcam.SharedRing(pidFile.Rings, env.Rings) == 0:
		report.warn(fmt.Sprintf("webcam: update stuntcam (ring %s, engine accepts %s)", rings(pidFile.Rings),
			rings(env.Rings)))
	case pidFile.Auth == "prompting":
		report.warn("webcam: waiting for the camera prompt: allow camera access for Stuntcam Camera in the dialog")
	}

	if live {
		report.ok(fmt.Sprintf("webcam: helper %s (pid %d) camera %s: state %s, auth %s, ring %s", pidFile.Sha,
			pidFile.Pid, pidFile.Camera, pidFile.State, pidFile.Auth, rings(pidFile.Rings)))
	}

	doctorShmOwners(env, cam, pidFile.Rings, report)
}

// doctorShmOwners fails each of the camera's shared-memory ring names that another user owns: the engine couldn't
// open it.
func doctorShmOwners(env *webcam.Env, cam string, ringVersions []int, report *doctorReport) {
	for _, v := range ringVersions {
		name := webcam.ShmName(env.Uid, cam, v)
		uid, err := env.ShmOwner(name)
		if err == nil && uid != env.Uid {
			report.fail(fmt.Sprintf("webcam: shm name %s owned by uid %d", name, uid))
		} else if errors.Is(err, syscall.EACCES) {
			report.fail(fmt.Sprintf("webcam: shm name %s owned by another user", name))
		}
	}
}

// doctorWebcamSessions lists the simulators with a webcam session, with their cameras and source.
func doctorWebcamSessions(env *webcam.Env, report *doctorReport) {
	sims, _ := os.ReadDir(filepath.Join(env.Home, "sims")) // no dir: no sessions
	for _, simDir := range sims {
		if cameras := webcam.ReadSession(env.Home, simDir.Name()); len(cameras) > 0 {
			report.info(fmt.Sprintf("webcam: simulator %s uses camera %s (source %s)", simDir.Name(),
				strings.Join(cameras, ", "), controlSource(simDir.Name())))
		}
	}
}

func field(s, key string) string {
	for _, line := range strings.Split(s, "\n") {
		if value, ok := strings.CutPrefix(line, key); ok {
			return strings.TrimSpace(value)
		}
	}
	return ""
}

func rings(r []int) string {
	var s []string
	for _, v := range r {
		s = append(s, fmt.Sprintf("v%d", v))
	}
	return strings.Join(s, ",")
}

// gcHelpers cleans up after camera helpers: pid files and rings of exited ones, and helper/<sha>/ dirs that are neither
// this build's (current) nor named by a running helper, under the unpack lock.
func gcHelpers(current string, dry bool, remove func(what, p string)) {
	env := newWebcamEnv()
	root := webcam.HelperRoot(env.Home)
	if _, err := os.Stat(root); err == nil && !dry {
		unlock, err := artifacts.LockDir(root)
		if err != nil {
			fmt.Println("keeping the camera helpers:", err)
			return
		}
		defer unlock()
	}

	env.GC(map[string]bool{current: current != ""}, dry, webcam.ShmUnlink, webcam.ShmProducer, remove)
	gcStale(root, env.Now(), remove)
}
