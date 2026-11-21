// SPDX-License-Identifier: Apache-2.0

// Package arm arms a simulator (design §4.1–4.2): it writes the sim's `current` file and gives every app launched in it the
// stub plus stuntcam's settings, either in the boot environment (boot mode: covers Xcode Run and tests; needs a (re)boot) or
// in the sim's launchd (launchd mode: no reboot, covers launches that don't set their own DYLD_INSERT_LIBRARIES).
package arm

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

// Sim is the part of simctl arming needs (an interface so it can be tested without a simulator).
type Sim interface {
	Boot(udid string, bootEnv []string) error
	Shutdown(udid string) error
	WaitBooted(udid string, bound time.Duration) error
	Getenv(udid, k string) (string, error)
	Setenv(udid, k, v string) error
	Unsetenv(udid, k string) error
}

type Mode string

const (
	Boot    Mode = "boot"
	Launchd Mode = "launchd"
)

// Options are the per-arm settings on top of the build.
type Options struct {
	StubLog bool              // log stub/loader decisions to logs/<udid>/stub.log
	Extra   map[string]string // more variables for every app, e.g. SIMCAM_SOURCE, SIMCAM_LAZY=0
}

// LegacyNames maps each engine setting Vars sets to its stuntcam 1.1.0 name. Vars sets both, with the same value: an
// older engine (a 1.1.0 build that `swap`, `up --build` or the login service's pinned build still runs) reads only the
// old name, and the engine reads the old one when the new one is missing (a sim armed by 1.1.0:
// engine/src/core/env.inc). Arm records list both, so `down` unsets both. Drop both sides together, in 2.0 at the
// earliest (docs/renames.md, "Upgrading from 1.1.0"). The keys are also the list of a sim's camera settings
// that `exec` never forwards from the caller's shell (cmd/stuntcam hook.go): when the old names go, keep that list.
var LegacyNames = map[string]string{
	"SIMCAM_FIG_SOURCES":   "M2_FAKE",
	"SIMCAM_FIG_SESSION":   "M2_SESSION_HOOK",
	"SIMCAM_OFF_KEY_LOG":   "M2_NOLOGDICT",
	"SIMCAM_PROFILE":       "M2_PLIST",
	"SIMCAM_FIXTURE_IMAGE": "M5_IMAGE",
	"SIMCAM_ENGINE_LOG":    "M5_LOG",
}

// appendSetting appends k=val and, for a LegacyNames setting, its 1.1.0 name with the same value right after it.
func appendSetting(v []string, k, val string) []string {
	v = append(v, k+"="+val)
	if old, ok := LegacyNames[k]; ok {
		v = append(v, old+"="+val)
	}
	return v
}

// Vars is the one variable list for both modes (scripts/arm-env.sh): the stub, the sim dir (whose `current` names the
// loader), the sentinel state dir, the chain file, the engine's config (each LegacyNames setting twice: new name, then
// old). The engine is not pinned: the loader loads the engine next to it, so `stuntcam swap` / an upgrade needs no
// reboot.
func Vars(udid string, s artifacts.Set, o Options) []string {
	v := []string{
		"DYLD_INSERT_LIBRARIES=" + s.Stub,
		"SIMCAM_SIM_DIR=" + home.Sim(udid),
		"SIMCAM_STATE_DIR=" + home.StateDir(udid),
		"SIMCAM_CHAIN=" + home.ChainFile(udid),
		"SIMCAM_CONTROL=" + home.ControlFile(udid), // stuntcam source / orient
	}
	v = appendSetting(v, "SIMCAM_FIG_SOURCES", "1")
	v = appendSetting(v, "SIMCAM_FIG_SESSION", "1")
	v = appendSetting(v, "SIMCAM_OFF_KEY_LOG", "1")
	if s.Profile != "" {
		v = appendSetting(v, "SIMCAM_PROFILE", s.Profile)
	}
	if s.Image != "" {
		v = appendSetting(v, "SIMCAM_FIXTURE_IMAGE", s.Image)
	}
	if o.StubLog {
		v = append(v, "SIMCAM_STUB_LOG="+filepath.Join(home.Logs(), udid, "stub.log"))
		v = appendSetting(v, "SIMCAM_ENGINE_LOG", filepath.Join(home.Logs(), udid, "dylib-%p.log"))
	}
	for k, val := range o.Extra {
		v = append(v, k+"="+val)
	}
	return v
}

// DownMarker exists after `stuntcam down` until the next `up` (ArmBoot / ArmLaunchd): the login service leaves such a sim
// alone. `env` and the self-test's private devices don't clear it.
func DownMarker(udid string) string { return filepath.Join(home.Sim(udid), "down") }

// Prepare writes everything the sim reads before any launch: current, an empty chain file, the state dir, the log dir.
func Prepare(udid string, s artifacts.Set) error {
	for _, d := range []string{home.Sim(udid), home.StateDir(udid), filepath.Join(home.Logs(), udid)} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	if _, err := os.Stat(home.ChainFile(udid)); os.IsNotExist(err) {
		if err := os.WriteFile(home.ChainFile(udid), nil, 0o644); err != nil {
			return err
		}
	}
	if s.Loader == "" { // caller keeps the existing current (stuntcam env)
		return nil
	}
	return home.WriteCurrent(udid, s.Loader)
}

// sameBootSlack is how many seconds after a boot record's At the sim's reported boot time may lie and still count as
// that boot: a margin for when simctl stamps it.
const sameBootSlack = 5

// SameBoot reports whether a boot-mode record is about the sim's current boot: booted, and not booted again since the
// record (lastBootedAt, or lastUsedAt on Xcode 27). An unknown boot time counts as a different boot (the safe direction:
// nothing assumes a boot environment that may be gone).
func SameBoot(r Record, booted bool, lastBooted time.Time) bool {
	return r.Mode == Boot && booted && !lastBooted.IsZero() && lastBooted.Unix() <= r.At+sameBootSlack
}

// Record is what `status` reads back: how and when a sim was armed.
type Record struct {
	Proto int      `json:"proto"`
	Mode  Mode     `json:"mode"`
	Build string   `json:"build"`
	At    int64    `json:"at"`
	Vars  []string `json:"vars"` // names set (launchd mode unsets exactly these on disarm)
	// Booting: written just before `simctl boot`, cleared once the boot is up. While it is set (and recent) the sim is
	// being armed: watchers leave it alone, whatever its boot timestamp says.
	Booting bool `json:"booting,omitempty"`
	Pid     int  `json:"pid,omitempty"` // the stuntcam process doing the boot (Booting only)
}

// Env is the record `stuntcam env` leaves: current written for per-launch arming, nothing in the sim's environment.
const Env Mode = "env"

// bootingRecordLife is how long a Booting record counts as in progress at most.
const bootingRecordLife = 180 * time.Second

// InProgress reports a boot record whose boot hasn't finished: its stuntcam process is alive and it is less than
// bootingRecordLife old (a crashed or interrupted stuntcam, or a clock change, can't leave it stuck).
func InProgress(r Record) bool {
	if !r.Booting {
		return false
	}
	if age := time.Since(time.Unix(r.At, 0)); age < 0 || age > bootingRecordLife {
		return false
	}
	return r.Pid > 0 && syscall.Kill(r.Pid, 0) == nil
}

// Config is sims/<udid>/config.json, read by the loader at each launch: the Xcode that was active when the sim was armed
// (design §4.1 "Which Xcode's libs", §4.5 step 1). The loader restores what <RulesDir>/<XcodeBuild>.json lists, or says
// "restore rules unverified" and uses the newest rules there. RulesDir is the per-content dir artifacts.StageRules made
// ($SIMCAM_HOME/restore-rules/<hash>), so an armed sim's rules never change under it; "" means none staged (built-in rules).
// BootSignals are the displaced-insert signal variables (IDB_*, ASAN_OPTIONS, …) of the sim's boot environment: every
// process has them, so the loader doesn't take them for a displaced per-launch insert. The loader reads proto,
// xcode_build, rules_dir and boot_signals.
type Config struct {
	Proto int `json:"proto"`
	xcode.Info
	RulesDir    string   `json:"rules_dir"`
	BootSignals []string `json:"boot_signals,omitempty"`
	Source      string   `json:"source"` // up, exec, service, env, swap
	At          int64    `json:"at"`
}

// WriteConfig records the Xcode for a sim (or an exec run directory), with the rules dir it was staged with and the boot
// environment's signal variables.
func WriteConfig(udid string, x xcode.Info, rulesDir, source string, bootSignals []string) error {
	if err := os.MkdirAll(home.Sim(udid), 0o755); err != nil {
		return err
	}
	c := Config{Proto: home.Proto, Info: x, RulesDir: rulesDir, BootSignals: bootSignals, Source: source, At: time.Now().Unix()}
	return writeConfig(udid, c)
}

// MaxConfigSize is the loader's config.json buffer less its NUL: a larger file is unusable to the loader (loader.c cfgbuf),
// which then applies its built-in rules and counts every boot signal as displaced.
const MaxConfigSize = 16384 - 2

func writeConfig(udid string, c Config) error {
	b, err := MarshalConfig(c)
	if err != nil {
		return err
	}
	if len(b) > MaxConfigSize && len(c.BootSignals) > 0 { // only boot_signals can grow without bound (long ASAN_OPTIONS, many IDB_*)
		fmt.Fprintf(os.Stderr, "warning: %s: the boot's signal variables don't fit the loader's %d bytes; "+
			"not recorded, so apps may say \"displaced\" for them\n", home.ConfigFile(udid), MaxConfigSize)
		c.BootSignals = nil
		if b, err = MarshalConfig(c); err != nil {
			return err
		}
	}
	if len(b) > MaxConfigSize {
		return fmt.Errorf("%s would be %d bytes, more than the loader reads (%d)", home.ConfigFile(udid), len(b), MaxConfigSize)
	}
	return home.WriteAtomic(home.ConfigFile(udid), b)
}

// SetRulesDir points an existing config.json at another staged rules dir and keeps the rest: what `swap` needs, so a build
// with new restore rules takes effect at the next launch without a reboot (design §4.1 B-MF4 (3)).
func SetRulesDir(udid, rulesDir string) error {
	c, err := ReadConfig(udid)
	if err != nil {
		return err
	}
	c.RulesDir = rulesDir
	return writeConfig(udid, c)
}

// MarshalConfig is config.json's bytes: indented, and with & < > written as themselves (json.Marshal's \u0026 escapes
// would need decoding in the loader).
func MarshalConfig(c Config) ([]byte, error) {
	var buf bytes.Buffer
	e := json.NewEncoder(&buf)
	e.SetEscapeHTML(false)
	e.SetIndent("", " ")
	err := e.Encode(c)
	return buf.Bytes(), err
}

// signalNames are the variables the loader's detect_displaced takes for a displaced insert (loader.c).
var signalNames = []string{"ASAN_OPTIONS", "TSAN_OPTIONS", "MALLOC_PROTECT_BEFORE", "MALLOC_FILL_SPACE", "MALLOC_STRICT_SIZE",
	"MALLOC_ALLOW_READS", "MALLOC_VECTOR_SIZE", "MALLOC_PERMIT_INSANE_REQUESTS"}

// Signals keeps the KEY=value entries of env that the loader treats as displaced-insert signals (IDB_*, ASAN_OPTIONS,
// TSAN_OPTIONS, Guard Malloc's settings), sorted; never nil.
func Signals(env []string) []string {
	out := []string{}
	for _, kv := range env {
		k, _, ok := strings.Cut(kv, "=")
		if !ok {
			continue
		}
		if strings.HasPrefix(k, "IDB_") || slices.Contains(signalNames, k) {
			out = append(out, kv)
		}
	}
	sort.Strings(out)
	return out
}

// ChildSignals is Signals of the SIMCTL_CHILD_ variables in env (prefix removed): what a `simctl boot` run with env puts in
// the boot environment besides stuntcam's own variables.
func ChildSignals(env []string) []string {
	var c []string
	for _, kv := range env {
		if r, ok := strings.CutPrefix(kv, "SIMCTL_CHILD_"); ok {
			c = append(c, r)
		}
	}
	return Signals(c)
}

func ReadConfig(udid string) (Config, error) {
	var c Config
	b, err := os.ReadFile(home.ConfigFile(udid))
	if err != nil {
		return c, err
	}
	return c, json.Unmarshal(b, &c)
}

func recordPath(udid string) string { return filepath.Join(home.Sim(udid), "arm.json") }

func WriteRecord(udid string, r Record) error {
	r.Proto = home.Proto
	b, _ := json.MarshalIndent(r, "", " ")
	return home.WriteAtomic(recordPath(udid), b)
}

func ReadRecord(udid string) (Record, error) {
	var r Record
	b, err := os.ReadFile(recordPath(udid))
	if err != nil {
		return r, err
	}
	return r, json.Unmarshal(b, &r)
}

func names(vars []string) []string {
	var n []string
	for _, kv := range vars {
		k, _, _ := strings.Cut(kv, "=")
		n = append(n, k)
	}
	return n
}

// bootReadyWait bounds the wait for a booted sim to be ready (bootstatus).
const bootReadyWait = 120 * time.Second

// notReady is the error for a boot that bootstatus didn't see finish in bootReadyWait.
func notReady(err error) error {
	return fmt.Errorf("booted, but not ready in %d s: %w", int(bootReadyWait/time.Second), err)
}

// ArmBoot (re)boots the sim with the variables as its boot environment.
func ArmBoot(sim Sim, udid string, s artifacts.Set, vars []string) error {
	snap := takeSnapshot(udid)
	if err := Prepare(udid, s); err != nil {
		return err
	}
	_ = os.Remove(DownMarker(udid)) // an explicit arm undoes `down`
	_ = sim.Shutdown(udid)          // fine if already shut down
	// recorded before the boot too, flagged in progress (with our pid), so a watcher (the login service) sees an arming,
	// not an unarmed boot; everything is put back as it was if the boot itself fails (the boot env then has no stub)
	booting := Record{Mode: Boot, Build: s.BuildID(), At: time.Now().Unix(), Vars: names(vars), Booting: true, Pid: os.Getpid()}
	if err := WriteRecord(udid, booting); err != nil {
		snap.restore(udid)
		return err
	}
	if err := sim.Boot(udid, vars); err != nil {
		snap.restore(udid)
		return err
	}
	final := WriteRecord(udid, Record{Mode: Boot, Build: s.BuildID(), At: time.Now().Unix(), Vars: names(vars)})
	if err := sim.WaitBooted(udid, bootReadyWait); err != nil {
		return notReady(err) // the boot env has the stub: the record stays, final
	}
	return final
}

// BootUnarmed boots a shut-down sim without stuntcam's boot environment (before a launchd arm), flagged in progress
// meanwhile so the login service doesn't arm it at the same time; the record is put back if the boot fails. The caller
// arms it (ArmLaunchd writes the final record).
func BootUnarmed(sim Sim, udid, build string) error {
	snap := takeSnapshot(udid)
	if err := os.MkdirAll(home.Sim(udid), 0o755); err != nil {
		return err
	}
	booting := Record{Mode: Launchd, Build: build, At: time.Now().Unix(), Booting: true, Pid: os.Getpid()}
	if err := WriteRecord(udid, booting); err != nil {
		return err
	}
	if err := sim.Boot(udid, nil); err != nil {
		snap.restore(udid)
		return err
	}
	if err := sim.WaitBooted(udid, bootReadyWait); err != nil {
		snap.restore(udid)
		return notReady(err)
	}
	return nil
}

// snapshot is a sim's host-side arming state (current, arm.json, the down marker), for rollback.
type snapshot struct {
	current, record []byte
	hasCur, hasRec  bool
	down            bool
}

func takeSnapshot(udid string) snapshot {
	var sn snapshot
	var err error
	sn.current, err = os.ReadFile(home.CurrentFile(udid))
	sn.hasCur = err == nil
	sn.record, err = os.ReadFile(recordPath(udid))
	sn.hasRec = err == nil
	_, err = os.Stat(DownMarker(udid))
	sn.down = err == nil
	return sn
}

func (sn snapshot) restore(udid string) {
	put := func(p string, b []byte, has bool) {
		if has {
			_ = home.WriteAtomic(p, b)
		} else {
			_ = os.Remove(p)
		}
	}
	put(home.CurrentFile(udid), sn.current, sn.hasCur)
	put(recordPath(udid), sn.record, sn.hasRec)
	if sn.down {
		_ = os.WriteFile(DownMarker(udid), []byte(fmt.Sprintf("%d\n", time.Now().Unix())), 0o644)
	} else {
		_ = os.Remove(DownMarker(udid))
	}
}

// MergeInsert appends stub to an existing DYLD_INSERT_LIBRARIES value, keeping other tools' entries in order (e.g. the T3
// Device panel's serve-sim loader) and dropping duplicates and older stuntcam libraries.
func MergeInsert(existing, stub string) string {
	return strings.Join(append(withoutOurs(existing), stub), ":")
}

// WithoutOurs removes stuntcam's libraries from an insert list ("" if nothing else is left).
func WithoutOurs(existing string) string { return strings.Join(withoutOurs(existing), ":") }

func withoutOurs(existing string) []string {
	var out []string
	seen := map[string]bool{}
	for _, e := range strings.Split(existing, ":") {
		if e == "" || seen[e] || strings.HasPrefix(filepath.Base(e), "libsimcam") {
			continue
		}
		seen[e] = true
		out = append(out, e)
	}
	return out
}

// ArmLaunchd sets the variables in the booted sim's launchd: read, merge, write, read back, up to 3 tries (design §4.2), as
// scripts/launchd-arm.sh does:
//   - a failed read of DYLD_INSERT_LIBRARIES aborts before anything changes (an empty read would drop other tools' entries);
//   - DYLD_INSERT_LIBRARIES is set last, so an app launched meanwhile never gets the stub without its settings;
//   - on a failed write it rolls back: DYLD_INSERT_LIBRARIES as found minus stuntcam's entries, the previous `current`,
//     every setting this arm names is unset (set or not: they are stuntcam's own), and arm.json says what is then
//     true (launchdBefore.rollback);
//   - the record of variable names is cumulative, so `down` unsets everything any arm set.
func ArmLaunchd(sim Sim, udid string, s artifacts.Set, vars []string) error {
	old, err := sim.Getenv(udid, "DYLD_INSERT_LIBRARIES")
	if err != nil {
		return fmt.Errorf("reading the simulator's DYLD_INSERT_LIBRARIES failed, not arming: %w", err)
	}
	before := launchdBefore{insert: old}
	_, downErr := os.Stat(DownMarker(udid))
	before.down = downErr == nil
	_ = os.Remove(DownMarker(udid)) // an explicit arm undoes `down` (put back if this arm rolls back)
	before.current, before.currentErr = os.ReadFile(home.CurrentFile(udid))
	prev, _ := ReadRecord(udid)
	if err := Prepare(udid, s); err != nil {
		return err
	}
	settings, stub := splitInsert(vars)
	all, dropped := launchdNames(prev, names(vars))
	before.record, before.recordErr = os.ReadFile(recordPath(udid))
	// the in-progress record this process's BootUnarmed wrote is not an earlier arm's: nothing to put back
	before.ownBooting = prev.Booting && prev.Pid == os.Getpid()
	// recorded before the writes: an arm killed half-way, or one that stops at a failed unset below, leaves a record
	// that names every variable for `down`
	if err := WriteRecord(udid, Record{Mode: Launchd, Build: s.BuildID(), At: time.Now().Unix(), Vars: all}); err != nil {
		return err
	}
	for _, n := range dropped {
		if err := sim.Unsetenv(udid, n); err != nil {
			return fmt.Errorf("unsetting %s (left by an earlier arm): %w", n, err)
		}
	}
	err = setAllVerified(sim, udid, settings)
	if err == nil {
		err = setVerified(sim, udid, "DYLD_INSERT_LIBRARIES", MergeInsert(old, stub))
	}
	if err != nil {
		before.rollback(sim, udid, settings, all, s.BuildID())
		return fmt.Errorf("%w; rolled back (other tools' DYLD_INSERT_LIBRARIES entries kept, stuntcam's removed)", err)
	}
	return nil
}

// splitInsert separates DYLD_INSERT_LIBRARIES (the stub; the last one if vars names it twice) from the other settings,
// which keep their order.
func splitInsert(vars []string) (settings []string, stub string) {
	for _, kv := range vars {
		if k, v, _ := strings.Cut(kv, "="); k == "DYLD_INSERT_LIBRARIES" {
			stub = v
		} else {
			settings = append(settings, kv)
		}
	}
	return settings, stub
}

// launchdNames is the record's cumulative list of names after a launchd arm setting names, and the names an earlier
// launchd arm (prev) set that this one doesn't (e.g. SIMCAM_SOURCE after `up --source`, then a plain `up`).
func launchdNames(prev Record, names []string) (all, dropped []string) {
	if prev.Mode != Launchd {
		return names, nil
	}
	for _, n := range prev.Vars {
		if !slices.Contains(names, n) {
			dropped = append(dropped, n)
		}
	}
	return union(prev.Vars, names), dropped
}

// setAllVerified sets the settings at once, each with setVerified, and joins their errors into one line (the service
// logs it). Each set and read-back goes through `simctl spawn` (0.5-1.5 s); one after another they took 10-25 s on a busy
// Mac, a window in which launches got no camera.
func setAllVerified(sim Sim, udid string, settings []string) error {
	errs := make([]error, len(settings))
	var wg sync.WaitGroup
	for i, kv := range settings {
		wg.Add(1)
		go func(i int, kv string) {
			defer wg.Done()
			k, v, _ := strings.Cut(kv, "=")
			errs[i] = setVerified(sim, udid, k, v)
		}(i, kv)
	}
	wg.Wait()
	var failed []string
	for _, e := range errs {
		if e != nil {
			failed = append(failed, e.Error())
		}
	}
	if len(failed) > 0 {
		return errors.New(strings.Join(failed, "; "))
	}
	return nil
}

// launchdBefore is what ArmLaunchd found before it changed anything, for its rollback.
type launchdBefore struct {
	insert     string // DYLD_INSERT_LIBRARIES as found
	down       bool   // the down marker existed
	current    []byte // the sim's `current`, if currentErr is nil
	currentErr error
	record     []byte // arm.json, if recordErr is nil
	recordErr  error
	ownBooting bool // arm.json was this process's own boot-in-progress record (BootUnarmed)
}

// rollback undoes a failed launchd arm: DYLD_INSERT_LIBRARIES as found minus stuntcam's entries, the previous `current`,
// every setting this arm names unset (set or not: they are stuntcam's own), the down marker put back, and an arm.json
// that says what is then true (all: this arm's names, plus an earlier launchd record's; build: the failed arm's):
//   - another arm's record is put back if every step above worked. A boot-mode record met here is from an earlier
//     boot, and `status` says so; after a launchd arm its `current` is back and none of its variables is set, which
//     `status` reads as "the stub is no longer in the simulator's launchd environment", and `down` still unsets its
//     names; an env record is per-launch arming again;
//   - no record before (or only this process's boot-in-progress one) and no `current`: none, if every step worked;
//   - otherwise a launchd record for the build `current` is back on, naming all: a sim the scripts armed (a `current`
//     without a record) would read as armed without one, though its stub is gone; and after a failed unset the names
//     that may still be set are there for `down`, which unsets only what a launchd record lists.
func (b launchdBefore) rollback(sim Sim, udid string, settings, all []string, build string) {
	if b.down { // first: a sim the user ran `down` on must not look armable if this process dies during the rollback
		_ = os.WriteFile(DownMarker(udid), []byte(fmt.Sprintf("%d\n", time.Now().Unix())), 0o644)
	}
	clean := true // every step that changes the sim's environment worked
	if rest := WithoutOurs(b.insert); rest != "" {
		clean = setVerified(sim, udid, "DYLD_INSERT_LIBRARIES", rest) == nil && clean
	} else {
		clean = sim.Unsetenv(udid, "DYLD_INSERT_LIBRARIES") == nil && clean
	}
	if b.currentErr == nil {
		_ = home.WriteAtomic(home.CurrentFile(udid), b.current)
	} else {
		_ = os.Remove(home.CurrentFile(udid))
	}
	for _, kv := range settings {
		k, _, _ := strings.Cut(kv, "=")
		clean = sim.Unsetenv(udid, k) == nil && clean
	}
	found := b.recordErr == nil && !b.ownBooting
	switch {
	case found && clean:
		_ = home.WriteAtomic(recordPath(udid), b.record)
	case !found && clean && b.currentErr != nil:
		_ = os.Remove(recordPath(udid))
	default:
		if _, loader, err := home.ReadCurrent(udid); err == nil {
			build = filepath.Base(filepath.Dir(loader))
		}
		_ = WriteRecord(udid, Record{Mode: Launchd, Build: build, At: time.Now().Unix(), Vars: all})
	}
}

func union(a, b []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, x := range append(append([]string{}, a...), b...) {
		if !seen[x] {
			seen[x] = true
			out = append(out, x)
		}
	}
	return out
}

// setVerifiedTries and setVerifiedPause: a launchctl setenv that doesn't read back is tried this often, this far apart.
const (
	setVerifiedTries = 3
	setVerifiedPause = 300 * time.Millisecond
)

// setVerified sets k in the sim's launchd and reads it back, up to setVerifiedTries times.
func setVerified(sim Sim, udid, k, v string) error {
	var last string
	for i := 0; i < setVerifiedTries; i++ {
		if err := sim.Setenv(udid, k, v); err == nil {
			if got, err := sim.Getenv(udid, k); err == nil && got == v {
				return nil
			} else {
				last = got
			}
		}
		time.Sleep(setVerifiedPause)
	}
	return fmt.Errorf("launchctl setenv %s did not stick (reads %q)", k, last)
}

// Disarm stops new launches from loading stuntcam, in either mode, without a reboot: the stub finds no `current` and loads
// nothing (it logs why). In launchd mode on a booted sim it also removes our variables and our stub from
// DYLD_INSERT_LIBRARIES, keeping other tools' entries; a shut-down sim's launchd environment is already gone, and its record
// is removed. On a booted boot-mode sim the record stays (it tells `status` and `env` the sim was disarmed while its boot
// environment still has the stub).
func Disarm(sim Sim, udid string, booted bool) error {
	// the marker first: the login service must never see this sim without current and without the marker
	if err := os.MkdirAll(home.Sim(udid), 0o755); err == nil {
		_ = os.WriteFile(DownMarker(udid), []byte(fmt.Sprintf("%d\n", time.Now().Unix())), 0o644)
	}
	if err := os.Remove(home.CurrentFile(udid)); err != nil && !os.IsNotExist(err) {
		return err
	}
	r, err := ReadRecord(udid)
	if err != nil {
		return nil
	}
	if !booted { // a shut-down sim's launchd environment and boot environment are both gone
		return os.Remove(recordPath(udid))
	}
	if r.Mode != Launchd {
		return nil
	}
	for _, k := range r.Vars {
		if k == "DYLD_INSERT_LIBRARIES" {
			old, err := sim.Getenv(udid, k)
			if err != nil {
				return fmt.Errorf("reading the simulator's DYLD_INSERT_LIBRARIES failed: %w", err)
			}
			if rest := WithoutOurs(old); rest != "" {
				if err := setVerified(sim, udid, k, rest); err != nil {
					return err
				}
				continue
			}
		}
		if err := sim.Unsetenv(udid, k); err != nil {
			return err
		}
	}
	return os.Remove(recordPath(udid))
}
