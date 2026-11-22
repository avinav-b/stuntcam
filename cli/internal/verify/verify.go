// SPDX-License-Identifier: Apache-2.0

// Package verify runs the canary self-test on a throwaway simulator in stuntcam's private device set and caches the
// verdict for each build and runtime. It is the Go port of scripts/verify.sh and shares that script's verdict cache,
// lock and verdict rules, so the two can be used side by side.
//
// Run is the entry point. The cache key is Key; the verdict cache is in cache.go and the per-runtime lock in lock.go.
// device.go finds, boots and settles the private device; install.go reinstalls and re-grants the canary only when
// needed; canary.go and outcome.go run it once and read the result; verdict.go decides when to run it again and what
// is cached; running.go handles signals; shutdown.go shuts the device down after the verdict is returned.
package verify

import (
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

const CanaryBundle = "dev.simcam.spike.m10canary"

type State string

const (
	Pass         State = "pass"
	Fail         State = "fail"
	Inconclusive State = "inconclusive"
)

// Result is a verdict. The JSON fields are the cache file's (scripts/verify.sh writes the same ones; it has no per-step
// seconds, only "times").
type Result struct {
	Proto         int      `json:"proto"`
	Key           string   `json:"key"`
	Verdict       string   `json:"verdict"` // pass | fail(...) | inconclusive(...)
	CanaryResult  string   `json:"result"`  // the canary's RESULT line
	RuntimeBuild  string   `json:"runtime_build"`
	RuntimeVer    string   `json:"runtime_version,omitempty"`
	EngineBuild   string   `json:"engine_build"`
	StubBuild     string   `json:"stub_build"`
	CanarySHA     string   `json:"canary_sha"`
	ProfileSHA    string   `json:"profile_sha"`
	Device        string   `json:"device"`
	Times         string   `json:"times,omitempty"`
	At            int64    `json:"at"`
	CreateSeconds int      `json:"create_s,omitempty"`
	BootSeconds   int      `json:"boot_s,omitempty"`
	CanarySeconds int      `json:"canary_s,omitempty"`
	Notes         []string `json:"notes,omitempty"`
	// Retried is the canary's retried:[…] list (photo stages whose first attempt missed only the frame-gap check and
	// were run again at the end of the same launch), "" if none. A pass with it is a retried pass (RetriedPass).
	Retried string `json:"retried,omitempty"`
	// Relaunched is the first launch's RESULT when the whole canary had to be relaunched (an inconclusive or crashed
	// first launch) and the second launch decided the verdict; "" if the first launch did. A pass after a relaunch is
	// short-lived like a retried pass: the second launch can't tell a post-boot stall from a regression that only hits
	// the first launch after a boot.
	Relaunched string `json:"relaunched,omitempty"`
	// FailedFirst is a content fail's RESULT when the launch run to confirm it passed, "" otherwise. A fail is cached
	// forever, so one launch's fail is confirmed before it is cached; a pass after it is short-lived like a relaunched
	// pass, and reported with the fail: one launch did fail, and a flaky check or an intermittent engine bug both look
	// so.
	FailedFirst string `json:"failed_first,omitempty"`
	State       State  `json:"-"` // from Verdict
	Cached      bool   `json:"-"`
	// Shipped means the verdict came from the package's shipped verdicts (the release gate's passes), not this Mac's
	// cache.
	Shipped bool `json:"-"`
	// PackagerXcode is the Xcode build scripts/package.sh ran with (shipped verdicts only, e.g. "27A266a").
	PackagerXcode string `json:"packager_xcode,omitempty"`
	NoCache       string `json:"-"` // why the cache was bypassed, if it was
}

// Key identifies what a verdict is about, the same string scripts/verify.sh builds:
// <runtime build>-<engine build>-<stub build>-c<8 hex SHA-1 of the canary executable>-<12 hex SHA-1 of the profile>.
func Key(runtimeBuild, engineBuild, stubBuild, canarySHA, profileSHA string) string {
	return runtimeBuild + "-" + engineBuild + "-" + stubBuild + "-c" + canarySHA + "-" + profileSHA
}

func sha1File(path string, hexDigits int) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	sum := sha1.Sum(data)
	return hex.EncodeToString(sum[:])[:hexDigits], nil
}

// How many hex digits of a SHA-1 a verdict key uses for the profile and the canary executable.
const (
	profileSHADigits = 12
	canarySHADigits  = 8
)

// ProfileSHA hashes the profile, as `shasum "$PROFILE" | cut -c1-12` does.
func ProfileSHA(path string) (string, error) { return sha1File(path, profileSHADigits) }

// CanarySHA hashes the canary's executable (not the whole bundle), as `shasum "$APP/m10canary" | cut -c1-8` does.
func CanarySHA(app string) (string, error) {
	return sha1File(filepath.Join(app, "m10canary"), canarySHADigits)
}

type Options struct {
	Runtime  string // iOS version or runtime id; "" = newest
	Fresh    bool   // ignore the cache
	Keep     bool   // leave the device booted
	Progress func(string)
}

// ErrSetup is returned (wrapped) when verify can't start: no canary, no profile, simctl unusable. Not a verdict.
var ErrSetup = errors.New("verify setup")

// Run returns the verdict for the build on a runtime, from the cache or by running the canary.
func Run(set artifacts.Set, opts Options) (Result, error) {
	say := opts.Progress
	if say == nil {
		say = func(string) {}
	}

	result, simRuntime, err := identify(set, opts.Runtime)
	if err != nil || result.State == Inconclusive {
		return result, err
	}

	if result.NoCache != "" {
		say("not using the verdict cache: " + result.NoCache)
	}

	result.Notes = append(result.Notes, "canary "+set.Canary)
	if !opts.Fresh && result.NoCache == "" {
		hit, ok, reason := lookup(result.Key, set.Verdicts, time.Now())
		if reason != "" {
			say(reason)
		}

		if ok {
			hit.RuntimeVer = simRuntime.Version
			return hit, nil
		}
	}

	return runOnDevice(set, opts, simRuntime, result, say)
}

// identify checks the build's canary and profile, picks the runtime and returns the verdict-to-be with its key. No
// matching runtime is already a verdict (inconclusive); a missing canary or profile, or a simctl that can't list
// runtimes, is ErrSetup.
func identify(set artifacts.Set, runtime string) (Result, simctl.Runtime, error) {
	if set.Canary == "" {
		err := fmt.Errorf("%w: no canary app in this build (scripts/build-fixtures.sh canary in a checkout)", ErrSetup)
		return Result{}, simctl.Runtime{}, err
	}

	canarySHA, err := CanarySHA(set.Canary)
	if err != nil {
		err := fmt.Errorf("%w: canary app missing or incomplete: %s (scripts/build-fixtures.sh canary)", ErrSetup,
			set.Canary)
		return Result{}, simctl.Runtime{}, err
	}

	runtimes, err := simctl.Ctl{}.Runtimes()
	if err != nil {
		return Result{}, simctl.Runtime{}, fmt.Errorf("%w: %w", ErrSetup, err)
	}

	simRuntime, ok := simctl.NewestIOS(runtimes, runtime)
	if !ok {
		verdict := fmt.Sprintf("inconclusive(no iOS runtime matching %q)", runtime)
		return Result{State: Inconclusive, Verdict: verdict}, simctl.Runtime{}, nil
	}

	profileSHA, err := ProfileSHA(set.Profile)
	if err != nil {
		return Result{}, simctl.Runtime{}, fmt.Errorf("%w: profile: %w", ErrSetup, err)
	}

	stubBuild := filepath.Base(filepath.Dir(set.Stub))
	result := Result{
		Key:          Key(simRuntime.Build, set.BuildID(), stubBuild, canarySHA, profileSHA),
		RuntimeBuild: simRuntime.Build,
		RuntimeVer:   simRuntime.Version,
		EngineBuild:  set.BuildID(),
		StubBuild:    stubBuild,
		CanarySHA:    canarySHA,
		ProfileSHA:   profileSHA,
		At:           time.Now().Unix(),
		NoCache:      noCacheReason(),
	}

	return result, simRuntime, nil
}

// runOnDevice runs the canary on the runtime's private device under the runtime's lock and returns result with the
// verdict. A signal ends the verify (running.interrupted); otherwise the device's shutdown is handed off to a detached
// child.
func runOnDevice(set artifacts.Set, opts Options, simRuntime simctl.Runtime, result Result,
	say func(string)) (Result, error) {
	mark("start")
	run := &running{ctl: simctl.Ctl{Set: home.Devices()}, keep: opts.Keep}
	lock := lockDir(simRuntime.Build)
	if err := acquireWait(lock, ShutdownWait, say); err != nil {
		result.State, result.Verdict = Inconclusive, "inconclusive("+err.Error()+")"
		return result, nil
	}
	run.lock = lock
	stopSignals := run.onSignal()
	defer func() {
		stopSignals()
		handoffShutdown(run) // the verdict doesn't wait for the device's shutdown; else cleanup shuts it down
		run.cleanup()
		mark("returned")
	}()

	if err := os.MkdirAll(home.Devices(), 0o755); err != nil {
		return result, err
	}
	ctl := run.ctl
	start := time.Now()
	dev, err := readyDevice(run, simRuntime, say)
	if err != nil {
		result.State, result.Verdict = Inconclusive, "inconclusive(device: "+err.Error()+")"
		return result, nil
	}
	udid := dev.udid
	result.Device, result.CreateSeconds = udid, int(time.Since(start).Seconds())
	if dev.created {
		result.Notes = append(result.Notes, "new private device")
	}
	bootStart := time.Now()
	settle := settleFloor(simRuntime.Version)
	plan, err := bootInstall(ctl, udid, set, false, settle, say)
	if err != nil {
		result.State, result.Verdict = Inconclusive, "inconclusive("+err.Error()+")"
		return result, nil
	}
	result.Notes = append(result.Notes, plan.Why)
	mark("bootinstall-done")
	result.BootSeconds = int(time.Since(bootStart).Seconds())
	runID := time.Now().Format("20060102-150405")
	decide(&result, launches{
		run: func(attempt string) canaryRun { return runCanary(run, udid, logPath(result.Key, runID, attempt)) },
		reboot: func() error {
			var err error
			plan, err = bootInstall(ctl, udid, set, true, settle, say)
			return err
		},
		redo: func(ran bool) (bool, error) { return redoSkippedInstall(ctl, udid, &plan, ran) },
	}, start)
	return result, nil
}

// readyDevice finds or creates the private device, waits out a shutdown an earlier verify left unfinished, and hands
// the device to run (whose cleanup shuts it down).
func readyDevice(run *running, simRuntime simctl.Runtime, say func(string)) (privateDevice, error) {
	dev, err := device(run.ctl, simRuntime, say)
	if err != nil {
		return dev, err
	}

	// a shutdown handed off by an earlier verify that didn't finish (its child was killed): let it settle before the
	// boot
	waitDevice(run.ctl, dev.udid, dev.state, ShutdownWait, say)
	run.mu.Lock()
	run.udid = dev.udid
	run.mu.Unlock()
	mark("device-ready created=%v", dev.created)
	return dev, nil
}

func shellArg(v string) string { return "'" + strings.ReplaceAll(v, "'", `'\''`) + "'" }
