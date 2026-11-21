// SPDX-License-Identifier: Apache-2.0

// Package verify runs the canary on a throwaway simulator in stuntcam's private device set (design §5.3, U1) and caches the
// verdict. It is the Go port of scripts/verify.sh and shares its cache, its lock and its verdict rules, so the two can be used
// side by side:
//   - key = <runtime build>-<engine build>-<stub build>-c<8 hex SHA-1 of the canary executable>-<12 hex SHA-1 of the profile>;
//   - $SIMCAM_HOME/verdicts/<key>.json (proto 1) holds only pass or fail(...) verdicts, written atomically; an unreadable
//     file or another verdict is a cache miss;
//   - exported SIMCTL_CHILD_* settings reach the canary's boot but not the key, so with any of them the cache is neither
//     read nor written;
//   - one verify per runtime build at a time: $SIMCAM_HOME/verify-<runtime build>.lock (a directory with a pid file),
//     taken after the cache read, released only by its owner; a dead owner's lock is taken over;
//   - the device's sentinel state is cleared before each boot; the canary gets 60 s for its RESULT line (its own budget is
//     45 s); an INCONCLUSIVE canary is run once more, and so is one that exits without a verdict (a crash): two crashes in a
//     row are fail(canary exited without a verdict (crash) …); refused launches and a canary still running at 60 s are
//     inconclusive, which is never cached;
//   - SIGINT/SIGTERM/SIGHUP shut the device down (unless Keep), release the lock and exit 130;
//   - otherwise the verdict is returned without waiting for the device's shutdown: a detached child takes over the shutdown
//     and the lock, and the next verify for that runtime waits (bounded) for it (shutdown.go);
//   - an unchanged canary is not reinstalled or re-granted; the installed copy is hashed each time (install.go);
//   - on iOS 27 and later the canary launches no earlier than PostBootSettle (8 s) after bootstatus;
//   - a pass that needed a retry, a relaunch or a confirming launch is cached for RetriedPassTTL only, and a content fail
//     is cached only once a second launch confirms it (Result.Retried, Relaunched, FailedFirst; confirmFail).
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
	// Retried is the canary's retried:[…] list (photo stages whose first attempt missed only the frame-gap check and were
	// run again at the end of the same launch), "" if none. A pass with it is a retried pass (RetriedPass).
	Retried string `json:"retried,omitempty"`
	// Relaunched is the first launch's RESULT when the whole canary had to be relaunched (an inconclusive or crashed first
	// launch) and the second launch decided the verdict; "" if the first launch did. A pass after a relaunch is short-lived
	// like a retried pass: the second launch can't tell a post-boot stall from a regression that only hits the first launch
	// after a boot.
	Relaunched string `json:"relaunched,omitempty"`
	// FailedFirst is a content fail's RESULT when the launch run to confirm it passed, "" otherwise. A fail is
	// cached forever, so one launch's fail is confirmed before it is cached; a pass after it is short-lived like a relaunched
	// pass, and reported with the fail: one launch did fail, and a flaky check or an intermittent engine bug both look so.
	FailedFirst string `json:"failed_first,omitempty"`
	State       State  `json:"-"` // from Verdict
	Cached      bool   `json:"-"`
	Shipped     bool   `json:"-"` // Cached from the package's shipped verdicts (design §2 C-MF1), not this Mac's cache
	// PackagerXcode is the Xcode build scripts/package.sh ran with (shipped verdicts only, e.g. "27A266a").
	PackagerXcode string `json:"packager_xcode,omitempty"`
	NoCache       string `json:"-"` // why the cache was bypassed, if it was
}

// Key identifies what a verdict is about (the same string scripts/verify.sh builds).
func Key(runtimeBuild, engineBuild, stubBuild, canarySHA, profileSHA string) string {
	return runtimeBuild + "-" + engineBuild + "-" + stubBuild + "-c" + canarySHA + "-" + profileSHA
}

func sha1File(path string, n int) (string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	s := sha1.Sum(b)
	return hex.EncodeToString(s[:])[:n], nil
}

func ProfileSHA(path string) (string, error) { return sha1File(path, 12) }

// CanarySHA hashes the canary's executable (not the whole bundle), as `shasum "$APP/m10canary" | cut -c1-8` does.
func CanarySHA(app string) (string, error) { return sha1File(filepath.Join(app, "m10canary"), 8) }

type Options struct {
	Runtime  string // iOS version or runtime id; "" = newest
	Fresh    bool   // ignore the cache
	Keep     bool   // leave the device booted
	Progress func(string)
}

// ErrSetup is returned (wrapped) when verify can't start: no canary, no profile, simctl unusable. Not a verdict.
var ErrSetup = errors.New("verify setup")

// Run returns the verdict for the build on a runtime, from the cache or by running the canary.
func Run(set artifacts.Set, o Options) (Result, error) {
	say := o.Progress
	if say == nil {
		say = func(string) {}
	}
	r, rt, err := identify(set, o.Runtime)
	if err != nil || r.State == Inconclusive {
		return r, err
	}
	if r.NoCache != "" {
		say("not using the verdict cache: " + r.NoCache)
	}
	r.Notes = append(r.Notes, "canary "+set.Canary)
	if !o.Fresh && r.NoCache == "" {
		c, ok, why := lookup(r.Key, set.Verdicts, time.Now())
		if why != "" {
			say(why)
		}
		if ok {
			c.RuntimeVer = rt.Version
			return c, nil
		}
	}
	return runOnDevice(set, o, rt, r, say)
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
		err := fmt.Errorf("%w: canary app missing or incomplete: %s (scripts/build-fixtures.sh canary)", ErrSetup, set.Canary)
		return Result{}, simctl.Runtime{}, err
	}
	runtimes, err := simctl.Ctl{}.Runtimes()
	if err != nil {
		return Result{}, simctl.Runtime{}, fmt.Errorf("%w: %v", ErrSetup, err)
	}
	rt, ok := simctl.NewestIOS(runtimes, runtime)
	if !ok {
		verdict := fmt.Sprintf("inconclusive(no iOS runtime matching %q)", runtime)
		return Result{State: Inconclusive, Verdict: verdict}, simctl.Runtime{}, nil
	}
	profileSHA, err := ProfileSHA(set.Profile)
	if err != nil {
		return Result{}, simctl.Runtime{}, fmt.Errorf("%w: profile: %v", ErrSetup, err)
	}
	stubBuild := filepath.Base(filepath.Dir(set.Stub))
	r := Result{
		Key:          Key(rt.Build, set.BuildID(), stubBuild, canarySHA, profileSHA),
		RuntimeBuild: rt.Build,
		RuntimeVer:   rt.Version,
		EngineBuild:  set.BuildID(),
		StubBuild:    stubBuild,
		CanarySHA:    canarySHA,
		ProfileSHA:   profileSHA,
		At:           time.Now().Unix(),
		NoCache:      noCacheReason(),
	}
	return r, rt, nil
}

// runOnDevice runs the canary on the runtime's private device under the runtime's lock and returns r with the verdict.
// A signal ends the verify (running.interrupted); otherwise the device's shutdown is handed off to a detached child.
func runOnDevice(set artifacts.Set, o Options, rt simctl.Runtime, r Result, say func(string)) (Result, error) {
	mark("start")
	run := &running{ctl: simctl.Ctl{Set: home.Devices()}, keep: o.Keep}
	lock := lockDir(rt.Build)
	if err := acquireWait(lock, ShutdownWait, say); err != nil {
		r.State, r.Verdict = Inconclusive, "inconclusive("+err.Error()+")"
		return r, nil
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
		return r, err
	}
	ctl := run.ctl
	t0 := time.Now()
	dev, err := readyDevice(run, rt, say)
	if err != nil {
		r.State, r.Verdict = Inconclusive, "inconclusive(device: "+err.Error()+")"
		return r, nil
	}
	udid := dev.udid
	r.Device, r.CreateSeconds = udid, int(time.Since(t0).Seconds())
	if dev.created {
		r.Notes = append(r.Notes, "new private device")
	}
	t1 := time.Now()
	settle := settleFloor(rt.Version)
	plan, err := bootInstall(ctl, udid, set, false, settle, say)
	if err != nil {
		r.State, r.Verdict = Inconclusive, "inconclusive("+err.Error()+")"
		return r, nil
	}
	r.Notes = append(r.Notes, plan.Why)
	mark("bootinstall-done")
	r.BootSeconds = int(time.Since(t1).Seconds())
	runID := time.Now().Format("20060102-150405")
	decide(&r, launches{
		run: func(n string) canaryRun { return runCanary(run, udid, logPath(r.Key, runID, n)) },
		reboot: func() error {
			var err error
			plan, err = bootInstall(ctl, udid, set, true, settle, say)
			return err
		},
		redo: func(ran bool) (bool, error) { return redoSkippedInstall(ctl, udid, &plan, ran) },
	}, t0)
	return r, nil
}

// readyDevice finds or creates the private device, waits out a shutdown an earlier verify left unfinished, and hands the
// device to run (whose cleanup shuts it down).
func readyDevice(run *running, rt simctl.Runtime, say func(string)) (privateDevice, error) {
	dev, err := device(run.ctl, rt, say)
	if err != nil {
		return dev, err
	}
	// a shutdown handed off by an earlier verify that didn't finish (its child was killed): let it settle before the boot
	waitDevice(run.ctl, dev.udid, dev.state, ShutdownWait, say)
	run.mu.Lock()
	run.udid = dev.udid
	run.mu.Unlock()
	mark("device-ready created=%v", dev.created)
	return dev, nil
}

func shellArg(v string) string { return "'" + strings.ReplaceAll(v, "'", `'\''`) + "'" }
