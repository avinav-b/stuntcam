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
//   - a pass that needed the canary's in-process retry of a photo's frame-gap check is a retried pass (Result.Retried): it
//     is reported as such and cached for RetriedPassTTL only;
//   - a content fail (core=FAIL that isn't a crash) is confirmed by one more launch before it is cached: a second content
//     fail is the verdict and cached; a pass after it is a pass reported with the fail (Result.FailedFirst) and cached for
//     RetriedPassTTL only, like a relaunched pass; no verdict from the confirming launch leaves the fail uncached.
package verify

import (
	"bytes"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
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

// StateOf maps a verdict string to its state ("" = not a verdict).
func StateOf(verdict string) State {
	switch {
	case verdict == "pass":
		return Pass
	case strings.HasPrefix(verdict, "fail("):
		return Fail
	case strings.HasPrefix(verdict, "inconclusive("):
		return Inconclusive
	}
	return ""
}

// Classify turns the canary's RESULT line into a verdict, with scripts/verify.sh's text rules: fail(<the core:[…] list,
// up to the timing:[ or ext:[ list, 294 characters>), inconclusive(<the line after "RESULT ", 193 characters>).
func Classify(result string) (State, string) {
	switch {
	case strings.Contains(result, "core=PASS"):
		return Pass, "pass"
	case strings.Contains(result, "core=FAIL"):
		checks := ""
		if i := strings.Index(result, "core:["); i >= 0 {
			checks = result[i+len("core:["):]
			cut := false
			for _, sep := range []string{"] timing:[", "] ext:["} {
				if j := strings.Index(checks, sep); j >= 0 {
					checks, cut = checks[:j], true
				}
			}
			if !cut { // the list ends the line: drop its own closing bracket only
				checks = strings.TrimSuffix(checks, "]")
			}
		}
		return Fail, "fail(" + trim(checks, 294) + ")"
	default:
		r := result
		if len(r) >= 7 {
			r = r[7:] // "RESULT "
		} else {
			r = ""
		}
		return Inconclusive, "inconclusive(" + trim(r, 193) + ")"
	}
}

// Retried returns the canary's retried:[…] list (stages whose first attempt missed only timing checks and were run again
// at the end of the same launch), or "".
func Retried(result string) string {
	i := strings.Index(result, " retried:[")
	if i < 0 {
		return ""
	}
	rest := result[i+len(" retried:["):]
	for _, sep := range []string{"] core:[", "] timing:[", "] ext:["} {
		if j := strings.Index(rest, sep); j >= 0 {
			rest = rest[:j]
		}
	}
	return strings.TrimSuffix(rest, "]")
}

// Final is the result after a second run: the second one, except that fail(crash) needs two crashes in a row (a single
// crash after an inconclusive first run is still inconclusive, never cached), as in scripts/verify.sh.
func Final(first, second string) string {
	if strings.Contains(second, crashText) && !strings.Contains(first, crashText) {
		return "RESULT core=INCONCLUSIVE (the canary crashed once, after an inconclusive first run; not cached)"
	}
	return second
}

func trim(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

func cachePath(key string) string { return filepath.Join(home.Verdicts(), key+".json") }

// RetriedPassTTL is how long a retried pass is cached. The canary's in-process retry of a photo's frame-gap check
// runs ~13 s later in the same, already warm process, so it can't tell a post-boot stall (what it is for) from a
// regression that only hits the first photo of a process: both show as "-> retry ok". A plain pass is cached until the
// key changes; a retried pass is re-run by the first verify or `up` after an hour, which bounds how long such a regression
// can hide behind one cached verdict, without making every `up` of a burst (a CI matrix, a morning of rebuilds) pay a
// 20-25 s canary when a healthy engine needed the retry once. scripts/verify.sh applies the same rule.
const RetriedPassTTL = time.Hour

// RetriedPassTTLText is RetriedPassTTL for messages ("1 h").
func RetriedPassTTLText() string {
	if RetriedPassTTL%time.Hour == 0 {
		return fmt.Sprintf("%d h", RetriedPassTTL/time.Hour)
	}
	return fmt.Sprintf("%d min", RetriedPassTTL/time.Minute)
}

// RetriedPass says whether a verdict is a pass that needed the canary's frame-gap retry, a whole relaunch, or a launch to
// confirm a content fail that then passed: such a pass is cached for RetriedPassTTL only.
func (r Result) RetriedPass() bool {
	return r.State == Pass && (r.Retried != "" || r.Relaunched != "" || r.FailedFirst != "")
}

// ContentFail says whether a RESULT line is a content fail: core=FAIL from checks that ran, not a canary that exited
// without a verdict (a crash, which already needs two in a row).
func ContentFail(result string) bool {
	st, _ := Classify(result)
	return st == Fail && !strings.Contains(result, crashText)
}

// Confirmation is what confirmFail decided.
type Confirmation struct {
	Result      string // the RESULT line that decides the verdict
	FailedFirst string // the confirmed fail's RESULT when the confirming launch passed
	NoStore     bool   // a content fail the confirming launch couldn't confirm or refute: report it, don't cache it
	Note        string // for Result.Notes, "" if no confirming launch ran
}

// confirmFail runs one more launch (again) after a content fail, so a fail is cached only when two launches in a row
// failed on content (a photo-then-stop fail on 17.5 came from an engine race that a second launch passed, and the
// cached fail made `up` refuse that build until a --fresh verify). Anything else is returned as it is, without a launch.
func confirmFail(res string, again func() string) Confirmation {
	if !ContentFail(res) {
		return Confirmation{Result: res}
	}
	_, v1 := Classify(res)
	second := again()
	st2, v2 := Classify(second)
	switch {
	case ContentFail(second):
		return Confirmation{Result: second, Note: "confirmed by a second launch: " + trim(v1, 160)}
	case st2 == Pass:
		return Confirmation{Result: second, FailedFirst: trim(res, 300), Note: "the first launch failed (" + trim(v1, 160) + "); the launch run to confirm it passed"}
	default: // inconclusive, or a crash: no second opinion
		return Confirmation{Result: res, NoStore: true, Note: "not cached: the launch run to confirm this fail gave no verdict (" + trim(v2, 160) + ")"}
	}
}

// Cached returns a cached pass or fail for key; anything else (no file, unreadable, another verdict, a retried pass older
// than RetriedPassTTL) is a miss.
func Cached(key string) (Result, bool) {
	r, ok, _, _ := cached(key, time.Now())
	return r, ok
}

// cached is Cached; why says why an existing entry was not used (a retried pass that expired, an unreadable file), else "";
// present says whether this Mac's cache has any entry for key at all (used or not).
func cached(key string, now time.Time) (r Result, ok bool, why string, present bool) {
	b, err := os.ReadFile(cachePath(key))
	if err != nil {
		return Result{}, false, "", false
	}
	if json.Unmarshal(b, &r) != nil {
		return Result{}, false, "the cached verdict for this build is unreadable: running the canary again", true
	}
	if r.State = StateOf(r.Verdict); r.State != Pass && r.State != Fail {
		return Result{}, false, fmt.Sprintf("the cached verdict for this build is %q, not a pass or fail: running the canary again", r.Verdict), true
	}
	if r.Retried == "" { // a file scripts/verify.sh or an older CLI wrote: the RESULT line still says
		r.Retried = Retried(r.CanaryResult)
	}
	if r.RetriedPass() && now.Sub(time.Unix(r.At, 0)) > RetriedPassTTL {
		what := "needed a canary retry"
		switch {
		case r.Retried == "" && r.Relaunched != "":
			what = "needed a canary retry (a relaunch)"
		case r.Retried == "" && r.FailedFirst != "":
			what = "needed a canary retry (its first launch failed)"
		}
		return Result{}, false, fmt.Sprintf("the cached pass from %s %s and is over %s old: running the canary again", time.Unix(r.At, 0).Format("15:04"), what, RetriedPassTTLText()), true
	}
	r.Cached = true
	return r, true, "", true
}

// lookup is the verdict without a canary run: this Mac's cache, else a shipped verdict from shippedDir. Any entry this Mac
// has for key (also an expired retried or relaunched pass, or an unreadable one) means this Mac measured this build itself:
// the shipped verdict is not used then, and the canary runs again.
func lookup(key, shippedDir string, now time.Time) (Result, bool, string) {
	c, ok, why, present := cached(key, now)
	switch {
	case ok:
		return c, true, ""
	case present:
		if _, shipped := Shipped(shippedDir, key); shipped {
			why += " (this Mac's own verdict for this build takes precedence over the one shipped with the package)"
		}
		return Result{}, false, why
	}
	if c, ok := Shipped(shippedDir, key); ok {
		return c, true, ""
	}
	return Result{}, false, ""
}

// Shipped returns the package's verdict for key from dir (the installed layout's share/stuntcam/verdicts, written by
// scripts/package.sh --verdicts from release-gate runs): a plain pass only. This Mac's own cache is read first, and any
// entry there for key wins; a shipped fail, a retried or relaunched pass (short-lived by design) or an unreadable file is
// a miss, and the canary runs. `verify --fresh` and exported SIMCTL_CHILD_* bypass it like the cache.
func Shipped(dir, key string) (Result, bool) {
	if dir == "" || key == "" || strings.ContainsAny(key, "/\\") {
		return Result{}, false
	}
	var r Result
	b, err := os.ReadFile(filepath.Join(dir, key+".json"))
	if err != nil || json.Unmarshal(b, &r) != nil || r.Key != key {
		return Result{}, false
	}
	if r.State = StateOf(r.Verdict); r.State != Pass || r.Retried != "" || r.Relaunched != "" || r.FailedFirst != "" || Retried(r.CanaryResult) != "" {
		return Result{}, false
	}
	r.Cached, r.Shipped = true, true
	return r, true
}

// ShippedSource describes a shipped verdict for messages: when the release gate measured it and with which Xcode the
// package was built (recorded by scripts/package.sh).
func (r Result) ShippedSource() string {
	x := r.PackagerXcode
	if x == "" {
		x = "unknown"
	}
	return fmt.Sprintf("measured %s, packaged with Xcode %s", time.Unix(r.At, 0).Format("2006-01-02"), x)
}

func store(r Result) error {
	if r.State != Pass && r.State != Fail {
		return nil
	}
	if err := os.MkdirAll(home.Verdicts(), 0o755); err != nil {
		return err
	}
	r.Proto = home.Proto
	b, _ := json.MarshalIndent(r, "", " ")
	return home.WriteAtomic(cachePath(r.Key), b)
}

// noCacheReason names exported settings that change the canary's result but aren't in the key.
func noCacheReason() string {
	var n []string
	for _, kv := range os.Environ() {
		if k, _, _ := strings.Cut(kv, "="); strings.HasPrefix(k, "SIMCTL_CHILD_") || k == "SIMCAM_VERIFY_SETTLE_S" { // a measured floor
			n = append(n, k)
		}
	}
	return strings.Join(n, " ")
}

// ---- lock (shared with scripts/verify.sh) ----

func lockDir(runtimeBuild string) string {
	return filepath.Join(home.Root(), "verify-"+runtimeBuild+".lock")
}

func pidAlive(pid int) bool { return pid > 0 && syscall.Kill(pid, 0) == nil }

func readPid(dir string) int {
	b, _ := os.ReadFile(filepath.Join(dir, "pid"))
	p, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	return p
}

// acquire takes the lock or says who holds it.
func acquire(dir string) (bool, string) {
	if err := os.Mkdir(dir, 0o755); err != nil {
		if !os.IsExist(err) {
			return false, err.Error()
		}
		p := readPid(dir)
		if p == 0 { // its owner may not have written the pid yet
			time.Sleep(time.Second)
			p = readPid(dir)
		}
		if _, err := os.Stat(dir); p == 0 && os.IsNotExist(err) { // released meanwhile: try again
			if os.Mkdir(dir, 0o755) != nil {
				return false, "lock taken over by another verify"
			}
			return writeOwnPid(dir)
		}
		stale := false
		if p == 0 { // no pid for over 5 minutes: its owner died between mkdir and writing the pid
			if fi, err := os.Stat(dir); err == nil && time.Since(fi.ModTime()) > 5*time.Minute {
				stale = true
			}
		}
		if p != 0 && markerStale(dir) { // a shutdown child's lock older than the child can live: its pid was reused
			stale = true
		}
		if !stale && (p == 0 || pidAlive(p)) {
			return false, fmt.Sprintf("another verify is using this runtime's device%s; lock %s", map[bool]string{true: fmt.Sprintf(", pid %d", p), false: ""}[p != 0], dir)
		}
		os.RemoveAll(dir) // a dead owner's
		if err := os.Mkdir(dir, 0o755); err != nil {
			return false, "lock taken over by another verify"
		}
	}
	return writeOwnPid(dir)
}

func writeOwnPid(dir string) (bool, string) {
	if err := os.WriteFile(filepath.Join(dir, "pid"), []byte(strconv.Itoa(os.Getpid())+"\n"), 0o644); err != nil {
		os.RemoveAll(dir)
		return false, err.Error()
	}
	return true, ""
}

func release(dir string) {
	if readPid(dir) == os.Getpid() {
		os.RemoveAll(dir)
	}
}

type Options struct {
	Runtime  string // iOS version or runtime id; "" = newest
	Fresh    bool   // ignore the cache
	Keep     bool   // leave the device booted
	Progress func(string)
}

// ErrSetup is returned (wrapped) when verify can't start: no canary, no profile, simctl unusable. Not a verdict.
var ErrSetup = errors.New("verify setup")

// run state the signal handler needs
type running struct {
	mu   sync.Mutex
	ctl  simctl.Ctl
	udid string
	keep bool
	lock string
	cmd  *exec.Cmd
}

func (r *running) cleanup() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.cleanupLocked()
}

func (r *running) cleanupLocked() {
	if r.cmd != nil && r.cmd.Process != nil {
		_ = r.cmd.Process.Signal(syscall.SIGTERM)
	}
	if r.udid != "" && !r.keep {
		_ = r.ctl.Shutdown(r.udid)
	}
	if r.lock != "" {
		release(r.lock)
		r.lock = ""
	}
}

// settleShutdown shuts the device down again if a boot that was past cancelling brings it up after all. It watches for up
// to d and returns once no simctl call is still in flight (a boot that ignored Interrupt's SIGINT) and the device has
// stayed shut down for 2 s.
func settleShutdown(c simctl.Ctl, udid string, d time.Duration) {
	var downSince time.Time
	for end := time.Now().Add(d); time.Now().Before(end); time.Sleep(500 * time.Millisecond) {
		ds, err := c.Devices()
		if err != nil {
			continue
		}
		down := true
		for _, x := range ds {
			if x.UDID == udid && x.State != "Shutdown" {
				down = false
				_ = c.Shutdown(udid)
			}
		}
		switch {
		case !down || simctl.Busy():
			downSince = time.Time{}
		case downSince.IsZero():
			downSince = time.Now()
		case time.Since(downSince) >= 2*time.Second:
			return
		}
	}
}

// Run returns the verdict for the build on a runtime, from the cache or by running the canary.
func Run(set artifacts.Set, o Options) (Result, error) {
	say := o.Progress
	if say == nil {
		say = func(string) {}
	}
	if set.Canary == "" {
		return Result{}, fmt.Errorf("%w: no canary app in this build (scripts/build-fixtures.sh canary in a checkout)", ErrSetup)
	}
	ch, err := CanarySHA(set.Canary)
	if err != nil {
		return Result{}, fmt.Errorf("%w: canary app missing or incomplete: %s (scripts/build-fixtures.sh canary)", ErrSetup, set.Canary)
	}
	pub := simctl.Ctl{}
	rs, err := pub.Runtimes()
	if err != nil {
		return Result{}, fmt.Errorf("%w: %v", ErrSetup, err)
	}
	rt, ok := simctl.NewestIOS(rs, o.Runtime)
	if !ok {
		return Result{State: Inconclusive, Verdict: fmt.Sprintf("inconclusive(no iOS runtime matching %q)", o.Runtime)}, nil
	}
	ph, err := ProfileSHA(set.Profile)
	if err != nil {
		return Result{}, fmt.Errorf("%w: profile: %v", ErrSetup, err)
	}
	stubB := filepath.Base(filepath.Dir(set.Stub))
	r := Result{Key: Key(rt.Build, set.BuildID(), stubB, ch, ph), RuntimeBuild: rt.Build, RuntimeVer: rt.Version, EngineBuild: set.BuildID(),
		StubBuild: stubB, CanarySHA: ch, ProfileSHA: ph, At: time.Now().Unix(), NoCache: noCacheReason()}
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

	mark("start")
	run := &running{ctl: simctl.Ctl{Set: home.Devices()}, keep: o.Keep}
	lock := lockDir(rt.Build)
	if ok, why := acquireWait(lock, ShutdownWait, say); !ok {
		r.State, r.Verdict = Inconclusive, "inconclusive("+why+")"
		return r, nil
	}
	run.lock = lock
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	stop := make(chan struct{})
	go func() {
		select {
		case <-sigs:
			run.mu.Lock()                      // held until exit: Run can't go on (e.g. `up` arming the user's sim) after an interrupt
			simctl.Interrupt(10 * time.Second) // stop a boot in flight first, or it boots the device after the shutdown below
			lock := run.lock                   // kept until the device has settled, so the next verify can't boot it meanwhile
			run.lock = ""
			run.cleanupLocked()
			if run.udid != "" && !run.keep {
				settleShutdown(run.ctl, run.udid, 30*time.Second)
				if simctl.Busy() {
					fmt.Fprintf(os.Stderr, "warning: a simctl call ignored the interrupt; the private canary device may still boot: xcrun simctl --set %s shutdown %s\n", shellArg(home.Devices()), run.udid)
				}
			}
			if lock != "" {
				release(lock)
			}
			fmt.Println("verdict inconclusive(interrupted)")
			os.Exit(130)
		case <-stop:
		}
	}()
	defer func() {
		signal.Stop(sigs)
		close(stop)
		handoffShutdown(run) // the verdict doesn't wait for the device's shutdown; else cleanup shuts it down
		run.cleanup()
		mark("returned")
	}()

	if err := os.MkdirAll(home.Devices(), 0o755); err != nil {
		return r, err
	}
	ctl := run.ctl
	t0 := time.Now()
	udid, state, created, err := device(ctl, rt, say)
	if err != nil {
		r.State, r.Verdict = Inconclusive, "inconclusive(device: "+err.Error()+")"
		return r, nil
	}
	// a shutdown handed off by an earlier verify that didn't finish (its child was killed): let it settle before the boot
	waitDevice(ctl, udid, state, ShutdownWait, say)
	run.mu.Lock()
	run.udid = udid
	run.mu.Unlock()
	mark("device-ready created=%v", created)
	r.Device, r.CreateSeconds = udid, int(time.Since(t0).Seconds())
	if created {
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
		run: func(n string) (string, bool, bool) { return runCanary(run, udid, logPath(r.Key, runID, n)) },
		reboot: func() error {
			var err error
			plan, err = bootInstall(ctl, udid, set, true, settle, say)
			return err
		},
		redo: func(ran bool) (bool, error) { return redoSkippedInstall(ctl, udid, &plan, ran) },
	}, t0)
	return r, nil
}

// launches is how decide reaches the device: run launches the canary once (n: the log suffix: 1, 1r, 2, c) and returns its
// RESULT line and whether every launch was refused / the canary ran at all; reboot re-arms and reboots after refused
// launches; redo reinstalls and re-grants a skipped install after a launch that didn't start. Fakes in tests.
type launches struct {
	run    func(n string) (result string, refused, ran bool)
	reboot func() error
	redo   func(ran bool) (bool, error)
}

// decide runs the canary through l until it has a verdict (a relaunch after an INCONCLUSIVE or a crash, a confirming
// launch after a content fail), fills r's verdict, times (t0: when Run started) and notes, and caches a pass or fail.
func decide(r *Result, l launches, t0 time.Time) {
	t2 := time.Now()
	res, refused, ran := l.run("1")
	if refused {
		r.Notes = append(r.Notes, "every launch refused this boot; rebooted once")
		if err := l.reboot(); err != nil {
			r.State, r.Verdict = Inconclusive, "inconclusive(reboot after refused launches: "+err.Error()+")"
			return
		}
		res, _, ran = l.run("1r")
	}
	if st, _ := Classify(res); st == Inconclusive || strings.Contains(res, crashText) {
		r.Notes = append(r.Notes, "first run: "+trim(res, 160))
		if redone, err := l.redo(ran); err != nil {
			r.State, r.Verdict = Inconclusive, "inconclusive("+err.Error()+")"
			return
		} else if redone {
			r.Notes = append(r.Notes, "the canary did not start with the install or grant skipped: reinstalled and granted")
		}
		second, _, _ := l.run("2")
		r.Relaunched = trim(res, 300)
		res = Final(res, second)
	}
	conf := confirmFail(res, func() string {
		mark("confirm-fail")
		c, _, _ := l.run("c")
		return c
	})
	if conf.Note != "" {
		r.Notes = append(r.Notes, conf.Note)
	}
	res, r.FailedFirst = conf.Result, conf.FailedFirst
	mark("canary-done")
	r.CanarySeconds = int(time.Since(t2).Seconds())
	r.Times = fmt.Sprintf("create=%ds boot=%ds canary=%ds total=%ds", r.CreateSeconds, r.BootSeconds, r.CanarySeconds, int(time.Since(t0).Seconds()))
	r.CanaryResult = res
	r.State, r.Verdict = Classify(res)
	if r.Retried = Retried(res); r.Retried != "" {
		r.Notes = append(r.Notes, "canary stage retried: "+trim(r.Retried, 300))
	}
	if r.NoCache == "" && !conf.NoStore {
		if err := store(*r); err != nil {
			r.Notes = append(r.Notes, "could not cache the verdict: "+err.Error())
		}
	}
}

func logPath(key, runID, n string) string {
	return filepath.Join(home.Logs(), "verify-"+key+"-"+runID+"-"+n+".log")
}

// device finds or creates the private device for a runtime build: the newest "iPhone N Pro" the runtime supports. It
// returns the device's state as listed ("Shutdown" for a new one).
func device(ctl simctl.Ctl, rt simctl.Runtime, say func(string)) (string, string, bool, error) {
	name := "stuntcam-canary-" + rt.Build
	find := func() (string, string, error) {
		ds, err := ctl.Devices()
		if err != nil {
			return "", "", err
		}
		for _, d := range ds {
			if d.Name == name {
				return d.UDID, d.State, nil
			}
		}
		return "", "", nil
	}
	if u, st, err := find(); err != nil || u != "" {
		return u, st, false, err
	}
	types := proPhonesNewestFirst(rt.Supported)
	if len(types) == 0 {
		return "", "", false, fmt.Errorf("iOS %s supports no iPhone Pro device type", rt.Version)
	}
	say(fmt.Sprintf("creating %s in the private set %s (first use of iOS %s: about a minute)", name, home.Devices(), rt.Version))
	c := ctl
	c.Timeout = 10 * time.Minute // a first create on a busy Mac can be slow; a timed-out one may still finish
	u, err := c.Create(name, types[0].Identifier, rt.Identifier)
	if err == nil && u != "" {
		return u, "Shutdown", true, nil
	}
	if u2, st, _ := find(); u2 != "" { // it finished anyway
		return u2, st, true, nil
	}
	return "", "", false, fmt.Errorf("create %s (%s): %v", name, types[0].Name, err)
}

var proRe = regexp.MustCompile(`^iPhone (\d+) Pro$`)

func proPhonesNewestFirst(ts []simctl.DeviceType) []simctl.DeviceType {
	var out []simctl.DeviceType
	for _, t := range ts {
		if proRe.MatchString(t.Name) {
			out = append(out, t)
		}
	}
	num := func(t simctl.DeviceType) int {
		var n int
		fmt.Sscanf(proRe.FindStringSubmatch(t.Name)[1], "%d", &n)
		return n
	}
	for i := 0; i < len(out); i++ {
		for j := i + 1; j < len(out); j++ {
			if num(out[j]) > num(out[i]) {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	return out
}

// PostBootSettle is the earliest the canary is launched after bootstatus returns, on runtimes that need it (settleFloor).
// iOS 27.0 has a post-boot window in which the canary's photos stall the frame pump and its frames come slowly:
//   - with the reinstall skipped the canary launched ~2.3 s after bootstatus; launch -> first output took 2.7 s
//     (0.5 s on 26.3) and a photo's frame gap missed the 100 ms check on 27/30 warm verifies (116-886 ms), each passed only
//     by the canary's in-process retry. With guard + install + grant the launch comes 5-12 s after bootstatus.
//   - 27.0 idle, by floor: 0 (launch at bootstatus + 2.2 s): 5/5 photo-jpeg misses (265-553 ms), slow launch
//     2.2-3.0 s. 5 s: the launch is quick again (0.5 s) but the canary runs inside the window: 4/10 missed frames A's frame
//     count (3 frames in 1 s; never retried in-process, so a +15 s relaunch) and 5/10 a photo-jpeg gap (143-520 ms).
//     8 s: 0/8 misses (largest gap 95 ms). 11 s: 1/6 (a 123 ms hvc1 gap) at +3 s more.
//
// So 8 s. It overlaps the SpringBoard guard and any install or grant: only the remainder is slept (~5.8 s on a warm
// verify, which is what skipping the install had given back). 26.3 and 17.5 get none: on 26.3 the launch at bootstatus +
// 2.2 s gave 0 misses in 106 warm verifies (idle and under an AVCam xcodebuild loop), the same as with a 5 s floor (12 runs).
const PostBootSettle = 8 * time.Second

// settleFloor is the post-boot settle for an iOS version: PostBootSettle on 27 and later (and on a version it can't read,
// e.g. a future runtime), 0 before 27. SIMCAM_VERIFY_SETTLE_S=<seconds> overrides it (measurements).
func settleFloor(version string) time.Duration {
	if v := os.Getenv("SIMCAM_VERIFY_SETTLE_S"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil && f >= 0 {
			return time.Duration(f * float64(time.Second))
		}
	}
	major, err := strconv.Atoi(strings.SplitN(version, ".", 2)[0])
	if err == nil && major < 27 {
		return 0
	}
	return PostBootSettle
}

// waitSettle sleeps until settle has passed since booted (bootstatus returned); booted zero: no wait.
func waitSettle(booted time.Time, settle time.Duration) {
	if booted.IsZero() || settle <= 0 {
		return
	}
	left := settle - time.Since(booted)
	if left > 0 {
		time.Sleep(left)
	}
	mark("settle floor=%.1f waited=%.1f", settle.Seconds(), max(left, 0).Seconds())
}

// bootInstall boots the device armed and makes sure this build's canary is installed and granted. force reinstalls and
// re-grants whatever is on the device (the reboot after refused launches); otherwise planInstall decides, before the boot.
// It returns no earlier than settle after bootstatus returned (PostBootSettle).
func bootInstall(ctl simctl.Ctl, udid string, set artifacts.Set, force bool, settle time.Duration, say func(string)) (installPlan, error) {
	plan := planInstall(filepath.Join(ctl.Set, udid), udid, set.Canary)
	if force {
		plan.Install, plan.Grant, plan.Why = true, true, "reinstalling the canary after refused launches"
	}
	mark("install-plan install=%v grant=%v why=%q", plan.Install, plan.Grant, plan.Why)
	if plan.Install {
		_ = os.Remove(CanaryRecordFile(udid)) // until this install has succeeded
	}
	// a throwaway device: no sentinel history from earlier verifies (two attributed crashes would latch the canary and turn
	// the next verdict into a fail whose checks hide the cause)
	_ = os.RemoveAll(home.StateDir(udid))
	mark("armboot-start")
	ts := timedSim{simSteps: ctl, booted: new(time.Time)}
	if err := arm.ArmBoot(ts, udid, set, arm.Vars(udid, set, arm.Options{StubLog: true})); err != nil {
		return plan, err
	}
	// SpringBoard guard: install and launch only once SpringBoard has kept one pid for 2 s
	guardSpringBoard(ctl, filepath.Join(ctl.Set, udid), udid, 150*time.Millisecond)
	if err := installGrantFn(ctl, udid, plan); err != nil {
		return plan, err
	}
	waitSettle(*ts.booted, settle)
	return plan, nil
}

// installGrantFn is installGrant; a var so tests can stand in for it.
var installGrantFn = installGrant

// redoSkippedInstall: after a first run that never started (no CANARY output) on a verify that skipped the install or the
// grant, reinstall and re-grant before the second run, so a broken install or a lost grant can't make both runs
// inconclusive. It reports whether it did.
func redoSkippedInstall(ctl simctl.Ctl, udid string, plan *installPlan, ran bool) (bool, error) {
	if ran || (plan.Install && plan.Grant) {
		return false, nil
	}
	plan.Install, plan.Grant = true, true
	_ = os.Remove(CanaryRecordFile(udid))
	return true, installGrantFn(ctl, udid, *plan)
}

// installGrant carries out a plan's install and grants and records what is now on the device.
func installGrant(ctl simctl.Ctl, udid string, plan installPlan) error {
	if plan.Install {
		if err := ctl.Install(udid, plan.canary); err != nil {
			return fmt.Errorf("install failed: %v", err)
		}
	}
	mark("install-done skipped=%v", !plan.Install)
	granted := !plan.Grant
	if plan.Grant { // idempotent; it terminates the app if it's running
		e1 := ctl.Grant(udid, "camera", CanaryBundle)
		e2 := ctl.Grant(udid, "microphone", CanaryBundle)
		granted = e1 == nil && e2 == nil
	}
	mark("grant-done skipped=%v", !plan.Grant)
	if plan.Install || plan.Grant {
		_ = recordInstall(filepath.Join(ctl.Set, udid), udid, plan.hash, granted)
	}
	return nil
}

var resultRe = regexp.MustCompile(`RESULT[^\r\n]*`)

const crashText = "canary exited without a verdict (crash)"

// runCanary launches the canary with its console attached and waits for its RESULT line (60 s: the canary's budget is
// 45 s). On BG-REQUEST it sends the app to the background (Settings) and back, as scripts/run.sh does. A launch refused as
// "Busy" is retried twice after 2 s; refused reports that every try was refused. The console goes to logFile.
func runCanary(run *running, udid, logFile string) (result string, refused, ran bool) {
	ctl := run.ctl
	var buf *bytes.Buffer
	defer func() {
		if buf != nil {
			_ = os.MkdirAll(filepath.Dir(logFile), 0o755)
			_ = os.WriteFile(logFile, []byte(simctl.Snapshot(buf)), 0o644)
		}
	}()
	for try := 1; try <= 3; try++ {
		mark("launch try=%d log=%s", try, filepath.Base(logFile))
		cmd, b, err := ctl.LaunchConsole(udid, CanaryBundle, nil)
		if err != nil {
			return "RESULT core=INCONCLUSIVE (launch failed: " + err.Error() + ")", false, false
		}
		buf = b
		run.mu.Lock()
		run.cmd = cmd
		run.mu.Unlock()
		done := make(chan struct{})
		go func() { cmd.Wait(); close(done) }()
		bg, exited, seen := false, false, false
		deadline := time.Now().Add(60 * time.Second)
	wait:
		for time.Now().Before(deadline) {
			select {
			case <-done:
				exited = true
				break wait
			case <-time.After(100 * time.Millisecond):
			}
			s := simctl.Snapshot(buf)
			if !seen && strings.Contains(s, "CANARY") {
				seen = true
				mark("first-canary-output")
			}
			if resultRe.MatchString(s) {
				mark("result-seen")
				break
			}
			if !bg && strings.Contains(s, "BG-REQUEST") {
				mark("bg-request")
				bg = true
				_ = ctl.Launch(udid, "com.apple.Preferences")
				time.Sleep(1200 * time.Millisecond)
				_ = ctl.Launch(udid, CanaryBundle)
			}
		}
		time.Sleep(300 * time.Millisecond) // let the console flush
		if cmd.Process != nil {
			_ = cmd.Process.Signal(syscall.SIGTERM)
		}
		run.mu.Lock()
		run.cmd = nil
		run.mu.Unlock()
		go func() { <-done; simctl.Forget(cmd) }()
		_ = ctl.Terminate(udid, CanaryBundle)
		_ = ctl.Terminate(udid, "com.apple.Preferences")
		mark("terminated")
		s := simctl.Snapshot(buf)
		if m := resultRe.FindString(s); m != "" {
			return strings.TrimRight(m, "\r"), false, true
		}
		if (strings.Contains(s, "Busy") || strings.Contains(s, "failed preflight")) && !strings.Contains(s, "CANARY") {
			if try == 3 {
				return "RESULT core=INCONCLUSIVE (every launch refused: Busy)", true, false
			}
			_ = os.WriteFile(logFile+".refused-"+strconv.Itoa(try), []byte(s), 0o644)
			time.Sleep(2 * time.Second)
			continue
		}
		last := ""
		for _, l := range strings.Split(s, "\n") {
			if strings.Contains(l, "CANARY") || strings.Contains(l, "reason:") || strings.Contains(l, "failed to launch") {
				last = strings.TrimSpace(l)
			}
		}
		if exited && strings.Contains(s, "CANARY") { // it ran, then exited without a verdict
			return "RESULT core=FAIL ext=FAIL core:[" + crashText + "; last: " + trim(last, 120) + "]", false, true
		}
		return "RESULT core=INCONCLUSIVE (no verdict in 60 s; last: " + trim(last, 120) + ")", false, strings.Contains(s, "CANARY")
	}
	return "RESULT core=INCONCLUSIVE (no launch)", true, false
}

func shellArg(v string) string { return "'" + strings.ReplaceAll(v, "'", `'\''`) + "'" }
