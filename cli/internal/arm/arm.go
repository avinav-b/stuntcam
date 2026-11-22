// SPDX-License-Identifier: Apache-2.0

// Package arm arms a simulator. It writes the sim's `current` file and gives every app launched in it the stub and
// stuntcam's settings. There are two ways to do that:
//   - Boot mode puts them in the boot environment. It covers Xcode Run and tests, but needs a boot or reboot.
//   - Launchd mode puts them in the sim's launchd. It needs no reboot, but covers only launches that don't set their
//     own DYLD_INSERT_LIBRARIES.
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

// LegacyNames maps each engine setting that Vars sets to its stuntcam 1.1.0 name. Vars sets both, with the same value.
//   - An older engine reads only the old name. That is a 1.1.0 build that `swap`, `up --build` or the login service's
//     pinned build still runs.
//   - The current engine reads the old name when the new one is missing, as in a sim armed by 1.1.0
//     (engine/src/core/env.inc).
//
// Arm records list both names, so `down` unsets both. Drop both sides together, in 2.0 at the earliest
// (docs/UPGRADING.md).
// The keys are also the list of a sim's camera settings that `exec` never forwards from the caller's shell
// (cmd/stuntcam hook.go). When the old names go, keep that list.
var LegacyNames = map[string]string{
	"SIMCAM_FIG_SOURCES":   "M2_FAKE",
	"SIMCAM_FIG_SESSION":   "M2_SESSION_HOOK",
	"SIMCAM_OFF_KEY_LOG":   "M2_NOLOGDICT",
	"SIMCAM_PROFILE":       "M2_PLIST",
	"SIMCAM_FIXTURE_IMAGE": "M5_IMAGE",
	"SIMCAM_ENGINE_LOG":    "M5_LOG",
}

// appendSetting appends key=value and, for a LegacyNames setting, its 1.1.0 name with the same value right after it.
func appendSetting(vars []string, key, value string) []string {
	vars = append(vars, key+"="+value)
	if old, ok := LegacyNames[key]; ok {
		vars = append(vars, old+"="+value)
	}
	return vars
}

// Vars is the one variable list for both modes, as in scripts/arm-env.sh. It holds the stub, the sim dir (whose
// `current` names the loader), the sentinel state dir, the chain file and the engine's config. Each LegacyNames
// setting appears twice: the new name, then the old one.
// The engine is not pinned. The loader loads the engine next to it, so `stuntcam swap` or an upgrade needs no reboot.
func Vars(udid string, set artifacts.Set, opts Options) []string {
	vars := []string{
		"DYLD_INSERT_LIBRARIES=" + set.Stub,
		"SIMCAM_SIM_DIR=" + home.Sim(udid),
		"SIMCAM_STATE_DIR=" + home.StateDir(udid),
		"SIMCAM_CHAIN=" + home.ChainFile(udid),
		"SIMCAM_CONTROL=" + home.ControlFile(udid), // stuntcam source / orient
	}

	vars = appendSetting(vars, "SIMCAM_FIG_SOURCES", "1")
	vars = appendSetting(vars, "SIMCAM_FIG_SESSION", "1")
	vars = appendSetting(vars, "SIMCAM_OFF_KEY_LOG", "1")
	if set.Profile != "" {
		vars = appendSetting(vars, "SIMCAM_PROFILE", set.Profile)
	}

	if set.Image != "" {
		vars = appendSetting(vars, "SIMCAM_FIXTURE_IMAGE", set.Image)
	}

	if opts.StubLog {
		vars = append(vars, "SIMCAM_STUB_LOG="+filepath.Join(home.Logs(), udid, "stub.log"))
		vars = appendSetting(vars, "SIMCAM_ENGINE_LOG", filepath.Join(home.Logs(), udid, "dylib-%p.log"))
	}

	for key, value := range opts.Extra {
		vars = append(vars, key+"="+value)
	}

	return vars
}

// DownMarker exists after `stuntcam down` until the next `up` (ArmBoot / ArmLaunchd): the login service leaves such a
// sim alone. `env` and the self-test's private devices don't clear it.
func DownMarker(udid string) string { return filepath.Join(home.Sim(udid), "down") }

// Prepare writes everything the sim reads before any launch: current, an empty chain file, the state dir, the log dir.
func Prepare(udid string, set artifacts.Set) error {
	for _, dir := range []string{home.Sim(udid), home.StateDir(udid), filepath.Join(home.Logs(), udid)} {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}

	if _, err := os.Stat(home.ChainFile(udid)); os.IsNotExist(err) {
		if err := os.WriteFile(home.ChainFile(udid), nil, 0o644); err != nil {
			return err
		}
	}

	if set.Loader == "" { // caller keeps the existing current (stuntcam env)
		return nil
	}

	return home.WriteCurrent(udid, set.Loader)
}

// sameBootSlack is how many seconds after a boot record's At the sim's reported boot time may lie and still count as
// that boot: a margin for when simctl stamps it.
const sameBootSlack = 5

// SameBoot reports whether a boot-mode record is about the sim's current boot: the sim is booted, and hasn't booted
// again since the record. The boot time is lastBootedAt, or lastUsedAt on Xcode 27.
// An unknown boot time counts as a different boot. That is the safe direction: nothing assumes a boot environment
// that may be gone.
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
	// Booting is set just before `simctl boot` and cleared once the boot is up. While it is set (and recent) the sim
	// is being armed, and watchers leave it alone whatever its boot timestamp says.
	Booting bool `json:"booting,omitempty"`
	Pid     int  `json:"pid,omitempty"` // the stuntcam process doing the boot (Booting only)
}

// Env is the record `stuntcam env` leaves: current written for per-launch arming, nothing in the sim's environment.
const Env Mode = "env"

// bootingRecordLife is how long a Booting record counts as in progress at most.
const bootingRecordLife = 180 * time.Second

// InProgress reports a boot record whose boot hasn't finished: its stuntcam process is alive and it is less than
// bootingRecordLife old (a crashed or interrupted stuntcam, or a clock change, can't leave it stuck).
func InProgress(record Record) bool {
	if !record.Booting {
		return false
	}
	if age := time.Since(time.Unix(record.At, 0)); age < 0 || age > bootingRecordLife {
		return false
	}
	return record.Pid > 0 && syscall.Kill(record.Pid, 0) == nil
}

// Config is sims/<udid>/config.json, which the loader reads at each launch. It records the Xcode that was active when
// the sim was armed. The loader reads proto, xcode_build, rules_dir and boot_signals.
//   - The loader restores what <RulesDir>/<XcodeBuild>.json lists. Without that file it says "restore rules
//     unverified" and uses the newest rules there.
//   - RulesDir is the per-content dir that artifacts.StageRules made ($SIMCAM_HOME/restore-rules/<hash>), so an armed
//     sim's rules never change under it. "" means none were staged, and the loader uses its built-in rules.
//   - BootSignals are the displaced-insert signal variables of the sim's boot environment, such as IDB_* and
//     ASAN_OPTIONS. Every process has them, so the loader doesn't take them for a displaced per-launch insert.
type Config struct {
	Proto int `json:"proto"`
	xcode.Info
	RulesDir    string   `json:"rules_dir"`
	BootSignals []string `json:"boot_signals,omitempty"`
	Source      string   `json:"source"` // up, exec, service, env, swap
	At          int64    `json:"at"`
}

// WriteConfig records the Xcode for a sim (or an exec run directory), with the rules dir it was staged with and the
// boot environment's signal variables.
func WriteConfig(udid string, x xcode.Info, rulesDir, source string, bootSignals []string) error {
	if err := os.MkdirAll(home.Sim(udid), 0o755); err != nil {
		return err
	}
	c := Config{Proto: home.Proto, Info: x, RulesDir: rulesDir, BootSignals: bootSignals, Source: source, At: time.Now().Unix()}
	return writeConfig(udid, c)
}

// MaxConfigSize is the loader's config.json buffer less its NUL: a larger file is unusable to the loader (loader.c
// gConfigJson), which then applies its built-in rules and counts every boot signal as displaced.
const MaxConfigSize = 16384 - 2

func writeConfig(udid string, config Config) error {
	data, err := MarshalConfig(config)
	if err != nil {
		return err
	}

	if len(data) > MaxConfigSize && len(config.BootSignals) > 0 { // only boot_signals can grow without bound (long ASAN_OPTIONS, many IDB_*)
		fmt.Fprintf(os.Stderr, "warning: %s: the boot's signal variables don't fit the loader's %d bytes; "+
			"not recorded, so apps may say \"displaced\" for them\n", home.ConfigFile(udid), MaxConfigSize)
		config.BootSignals = nil
		if data, err = MarshalConfig(config); err != nil {
			return err
		}
	}

	if len(data) > MaxConfigSize {
		return fmt.Errorf("%s would be %d bytes, more than the loader reads (%d)", home.ConfigFile(udid), len(data), MaxConfigSize)
	}

	return home.WriteAtomic(home.ConfigFile(udid), data)
}

// SetRulesDir points an existing config.json at another staged rules dir and keeps the rest: what `swap` needs, so a
// build with new restore rules takes effect at the next launch without a reboot.
func SetRulesDir(udid, rulesDir string) error {
	config, err := ReadConfig(udid)
	if err != nil {
		return err
	}
	config.RulesDir = rulesDir
	return writeConfig(udid, config)
}

// MarshalConfig is config.json's bytes: indented, and with & < > written as themselves (json.Marshal's \u0026 escapes
// would need decoding in the loader).
func MarshalConfig(config Config) ([]byte, error) {
	var buf bytes.Buffer
	encoder := json.NewEncoder(&buf)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", " ")
	err := encoder.Encode(config)
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
		key, _, ok := strings.Cut(kv, "=")
		if !ok {
			continue
		}

		if strings.HasPrefix(key, "IDB_") || slices.Contains(signalNames, key) {
			out = append(out, kv)
		}
	}

	sort.Strings(out)
	return out
}

// ChildSignals is Signals of the SIMCTL_CHILD_ variables in env (prefix removed): what a `simctl boot` run with env
// puts in the boot environment besides stuntcam's own variables.
func ChildSignals(env []string) []string {
	var stripped []string
	for _, kv := range env {
		if variable, ok := strings.CutPrefix(kv, "SIMCTL_CHILD_"); ok {
			stripped = append(stripped, variable)
		}
	}
	return Signals(stripped)
}

func ReadConfig(udid string) (Config, error) {
	var config Config
	data, err := os.ReadFile(home.ConfigFile(udid))
	if err != nil {
		return config, err
	}
	err = json.Unmarshal(data, &config) // on a JSON error, config keeps what did parse
	return config, err
}

func recordPath(udid string) string { return filepath.Join(home.Sim(udid), "arm.json") }

func WriteRecord(udid string, record Record) error {
	record.Proto = home.Proto
	data, _ := json.MarshalIndent(record, "", " ") // can't fail: strings, numbers and bools
	return home.WriteAtomic(recordPath(udid), data)
}

func ReadRecord(udid string) (Record, error) {
	data, err := os.ReadFile(recordPath(udid))
	if err != nil {
		return Record{}, err
	}
	return parseRecord(data)
}

// parseRecord parses arm.json's bytes. On a JSON error it returns what did parse, with the error.
func parseRecord(data []byte) (Record, error) {
	var record Record
	err := json.Unmarshal(data, &record)
	return record, err
}

func names(vars []string) []string {
	var out []string
	for _, kv := range vars {
		name, _, _ := strings.Cut(kv, "=")
		out = append(out, name)
	}
	return out
}

// bootReadyWait bounds the wait for a booted sim to be ready (bootstatus).
const bootReadyWait = 120 * time.Second

// notReady is the error for a boot that bootstatus didn't see finish in bootReadyWait.
func notReady(err error) error {
	return fmt.Errorf("booted, but not ready in %d s: %w", int(bootReadyWait/time.Second), err)
}

// ArmBoot (re)boots the sim with the variables as its boot environment.
func ArmBoot(sim Sim, udid string, set artifacts.Set, vars []string) error {
	snap := takeSnapshot(udid)
	if err := Prepare(udid, set); err != nil {
		return err
	}

	_ = os.Remove(DownMarker(udid)) // an explicit arm undoes `down`
	_ = sim.Shutdown(udid)          // fine if already shut down
	// The record is written before the boot too, flagged in progress with our pid. A watcher such as the login
	// service then sees an arming, not an unarmed boot.
	// If the boot itself fails, everything is put back as it was, because the boot environment then has no stub.
	booting := Record{Mode: Boot, Build: set.BuildID(), At: time.Now().Unix(), Vars: names(vars), Booting: true, Pid: os.Getpid()}
	if err := WriteRecord(udid, booting); err != nil {
		snap.restore(udid)
		return err
	}

	if err := sim.Boot(udid, vars); err != nil {
		snap.restore(udid)
		return err
	}

	final := WriteRecord(udid, Record{Mode: Boot, Build: set.BuildID(), At: time.Now().Unix(), Vars: names(vars)})
	if err := sim.WaitBooted(udid, bootReadyWait); err != nil {
		return notReady(err) // the boot env has the stub: the record stays, final
	}

	return final
}

// BootUnarmed boots a shut-down sim without stuntcam's boot environment, before a launchd arm.
// Meanwhile the record flags the sim as in progress, so the login service doesn't arm it at the same time. The old
// record is put back if the boot fails.
// The caller then arms the sim, and ArmLaunchd writes the final record.
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
	var snap snapshot
	var err error
	snap.current, err = os.ReadFile(home.CurrentFile(udid))
	snap.hasCur = err == nil
	snap.record, err = os.ReadFile(recordPath(udid))
	snap.hasRec = err == nil
	_, err = os.Stat(DownMarker(udid))
	snap.down = err == nil
	return snap
}

// restore puts the snapshot back. Each step is best effort: one that fails can't be retried here, and the caller
// returns the error that made it restore.
func (sn snapshot) restore(udid string) {
	put := func(path string, data []byte, has bool) {
		if has {
			_ = home.WriteAtomic(path, data)
		} else {
			_ = os.Remove(path)
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

// MergeInsert appends stub to an existing DYLD_INSERT_LIBRARIES value, keeping other tools' entries in order (e.g. the
// T3 Device panel's serve-sim loader) and dropping duplicates and older stuntcam libraries.
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

// ArmLaunchd sets the variables in the booted sim's launchd, as scripts/launchd-arm.sh does. Each one is read,
// merged, written and read back, with up to setVerifiedTries tries.
//   - A failed read of DYLD_INSERT_LIBRARIES stops before anything changes. Going on after an empty read would drop
//     other tools' entries.
//   - DYLD_INSERT_LIBRARIES is set last, so an app launched meanwhile never gets the stub without its settings.
//   - A failed write rolls everything back (launchdBefore.rollback).
//   - The record of variable names is cumulative, so `down` unsets everything any arm set.
func ArmLaunchd(sim Sim, udid string, set artifacts.Set, vars []string) error {
	before, err := readLaunchdBefore(sim, udid)
	if err != nil {
		return err
	}

	// an explicit arm undoes `down`; the rollback puts the marker back. A failed remove (usually: there is none) only
	// keeps the login service away from the sim.
	_ = os.Remove(DownMarker(udid))
	if err := Prepare(udid, set); err != nil {
		return err
	}

	settings, stub := splitInsert(vars)
	all, dropped := launchdNames(before.prev, names(vars))
	// recorded before the writes: an arm killed half-way, or one that stops at a failed unset below, leaves a record
	// that names every variable for `down`
	if err := WriteRecord(udid, Record{Mode: Launchd, Build: set.BuildID(), At: time.Now().Unix(), Vars: all}); err != nil {
		return err
	}

	for _, name := range dropped {
		if err := sim.Unsetenv(udid, name); err != nil {
			return fmt.Errorf("unsetting %s (left by an earlier arm): %w", name, err)
		}
	}

	err = setAllVerified(sim, udid, settings)
	if err == nil {
		err = setVerified(sim, udid, "DYLD_INSERT_LIBRARIES", MergeInsert(before.insert, stub))
	}

	if err != nil {
		before.rollback(sim, udid, settings, all, set.BuildID())
		return fmt.Errorf("%w; rolled back (other tools' DYLD_INSERT_LIBRARIES entries kept, stuntcam's removed)", err)
	}

	return nil
}

// readLaunchdBefore reads everything a launchd arm's rollback puts back, before the arm changes anything. It only
// reads; ArmLaunchd says why a failed read of DYLD_INSERT_LIBRARIES stops the arm.
func readLaunchdBefore(sim Sim, udid string) (launchdBefore, error) {
	insert, err := sim.Getenv(udid, "DYLD_INSERT_LIBRARIES")
	if err != nil {
		return launchdBefore{}, fmt.Errorf("reading the simulator's DYLD_INSERT_LIBRARIES failed, not arming: %w", err)
	}

	before := launchdBefore{insert: insert}
	_, downErr := os.Stat(DownMarker(udid))
	before.down = downErr == nil
	before.current, before.currentErr = os.ReadFile(home.CurrentFile(udid))
	before.record, before.recordErr = os.ReadFile(recordPath(udid))
	if before.recordErr == nil {
		// a record that doesn't parse keeps what did parse, as ReadRecord does; its bytes still go back
		before.prev, _ = parseRecord(before.record)
	}

	// the in-progress record this process's BootUnarmed wrote is not an earlier arm's: nothing to put back
	before.ownBooting = before.prev.Booting && before.prev.Pid == os.Getpid()
	return before, nil
}

// splitInsert separates DYLD_INSERT_LIBRARIES (the stub; the last one if vars names it twice) from the other settings,
// which keep their order.
func splitInsert(vars []string) (settings []string, stub string) {
	for _, kv := range vars {
		if key, value, _ := strings.Cut(kv, "="); key == "DYLD_INSERT_LIBRARIES" {
			stub = value
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
	for _, name := range prev.Vars {
		if !slices.Contains(names, name) {
			dropped = append(dropped, name)
		}
	}
	return union(prev.Vars, names), dropped
}

// setAllVerified sets the settings at once, each with setVerified. It joins their errors into one line, which the
// service logs.
// They run at the same time because each set and read-back goes through `simctl spawn`, which takes 0.5-1.5 s. One
// after another they took 10-25 s on a busy Mac, and launches in that window got no camera.
func setAllVerified(sim Sim, udid string, settings []string) error {
	errs := make([]error, len(settings))
	var wg sync.WaitGroup
	for i, kv := range settings {
		wg.Add(1)
		go func(i int, kv string) {
			defer wg.Done()

			key, value, _ := strings.Cut(kv, "=")
			errs[i] = setVerified(sim, udid, key, value)
		}(i, kv)
	}

	wg.Wait()
	var failed []string
	for _, setErr := range errs {
		if setErr != nil {
			failed = append(failed, setErr.Error())
		}
	}

	if len(failed) > 0 {
		return errors.New(strings.Join(failed, "; "))
	}

	return nil
}

// launchdBefore is what ArmLaunchd found before it changed anything (readLaunchdBefore), for its rollback.
type launchdBefore struct {
	insert     string // DYLD_INSERT_LIBRARIES as found
	down       bool   // the down marker existed
	current    []byte // the sim's `current`, if currentErr is nil
	currentErr error
	record     []byte // arm.json, if recordErr is nil
	recordErr  error
	prev       Record // record parsed (zero if there was none): an earlier launchd arm's names stay recorded
	ownBooting bool   // arm.json was this process's own boot-in-progress record (BootUnarmed)
}

// rollback undoes a failed launchd arm. It:
//   - sets DYLD_INSERT_LIBRARIES back to what it found, without stuntcam's entries;
//   - puts the previous `current` back;
//   - unsets every setting this arm names, whether or not it was set, because they are stuntcam's own;
//   - puts the down marker back;
//   - writes an arm.json that says what is then true. `all` is this arm's names plus an earlier launchd record's, and
//     `build` is the failed arm's.
//
// Which record arm.json gets:
//   - If every step worked and another arm had a record, that record is put back.
//     A boot-mode record met here is from an earlier boot, and `status` says so.
//     After a launchd record, its `current` is back and none of its variables is set. `status` reads that as "the stub
//     is no longer in the simulator's launchd environment", and `down` still unsets its names.
//     After an env record, arming is per-launch again.
//   - If every step worked, and there was no record (or only this process's boot-in-progress one) and no `current`,
//     there is no record.
//   - Otherwise it gets a launchd record for the build that `current` is back on, naming `all`. Without it, a sim the
//     scripts armed (a `current` with no record) would read as armed though its stub is gone. And after a failed
//     unset, the names that may still be set are there for `down`, which unsets only what a launchd record lists.
//
// The file writes are best effort: one that fails can't be retried here, and ArmLaunchd returns the arm's error
// either way. A failed change to the sim's environment makes the rollback unclean, which picks the record above.
func (before launchdBefore) rollback(sim Sim, udid string, settings, all []string, build string) {
	// first: a sim the user ran `down` on must not look armable if this process dies during the rollback
	if before.down {
		_ = os.WriteFile(DownMarker(udid), []byte(fmt.Sprintf("%d\n", time.Now().Unix())), 0o644)
	}

	clean := true // every step that changes the sim's environment worked
	if rest := WithoutOurs(before.insert); rest != "" {
		clean = setVerified(sim, udid, "DYLD_INSERT_LIBRARIES", rest) == nil && clean
	} else {
		clean = sim.Unsetenv(udid, "DYLD_INSERT_LIBRARIES") == nil && clean
	}

	if before.currentErr == nil {
		_ = home.WriteAtomic(home.CurrentFile(udid), before.current)
	} else {
		_ = os.Remove(home.CurrentFile(udid))
	}

	for _, kv := range settings {
		key, _, _ := strings.Cut(kv, "=")
		clean = sim.Unsetenv(udid, key) == nil && clean
	}

	found := before.recordErr == nil && !before.ownBooting
	switch {
	case found && clean:
		_ = home.WriteAtomic(recordPath(udid), before.record)
	case !found && clean && before.currentErr != nil:
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
	for _, name := range append(append([]string{}, a...), b...) {
		if !seen[name] {
			seen[name] = true
			out = append(out, name)
		}
	}
	return out
}

// A launchctl setenv that doesn't read back is tried setVerifiedTries times, setVerifiedPause apart.
const (
	setVerifiedTries = 3
	setVerifiedPause = 300 * time.Millisecond
)

// setVerified sets key in the sim's launchd and reads it back, up to setVerifiedTries times.
func setVerified(sim Sim, udid, key, value string) error {
	var last string
	for i := 0; i < setVerifiedTries; i++ {
		if err := sim.Setenv(udid, key, value); err == nil {
			if got, err := sim.Getenv(udid, key); err == nil && got == value {
				return nil
			} else {
				last = got
			}
		}

		time.Sleep(setVerifiedPause)
	}

	return fmt.Errorf("launchctl setenv %s did not stick (reads %q)", key, last)
}

// Disarm stops new launches from loading stuntcam, in either mode, without a reboot. The stub finds no `current` and
// loads nothing, and logs why.
//   - In launchd mode on a booted sim, it also removes our variables, and our stub from DYLD_INSERT_LIBRARIES. Other
//     tools' entries stay.
//   - A shut-down sim's launchd environment is already gone, and its record is removed.
//   - On a booted boot-mode sim the record stays. It tells `status` and `env` that the sim was disarmed while its boot
//     environment still has the stub.
func Disarm(sim Sim, udid string, booted bool) error {
	// the marker first: the login service must never see this sim without current and without the marker. Best effort:
	// removing current below stops new launches loading stuntcam either way.
	if err := os.MkdirAll(home.Sim(udid), 0o755); err == nil {
		_ = os.WriteFile(DownMarker(udid), []byte(fmt.Sprintf("%d\n", time.Now().Unix())), 0o644)
	}

	if err := os.Remove(home.CurrentFile(udid)); err != nil && !os.IsNotExist(err) {
		return err
	}

	record, err := ReadRecord(udid)
	if err != nil {
		return nil
	}

	if !booted { // a shut-down sim's launchd environment and boot environment are both gone
		return os.Remove(recordPath(udid))
	}

	if record.Mode != Launchd {
		return nil
	}

	for _, name := range record.Vars {
		if name == "DYLD_INSERT_LIBRARIES" {
			old, err := sim.Getenv(udid, name)
			if err != nil {
				return fmt.Errorf("reading the simulator's DYLD_INSERT_LIBRARIES failed: %w", err)
			}

			if rest := WithoutOurs(old); rest != "" {
				if err := setVerified(sim, udid, name, rest); err != nil {
					return err
				}

				continue
			}
		}

		if err := sim.Unsetenv(udid, name); err != nil {
			return err
		}
	}

	return os.Remove(recordPath(udid))
}
