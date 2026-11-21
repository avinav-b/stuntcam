// SPDX-License-Identifier: Apache-2.0
package arm

import (
	"errors"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

type fakeSim struct {
	mu          sync.Mutex // ArmLaunchd sets variables concurrently
	env         map[string]string
	booted      []string
	flaky       int    // Setenv calls that silently don't stick
	failKey     string // Setenv of this key never sticks
	getErr      bool   // Getenv fails
	bootErr     bool   // Boot fails
	order       []string
	unset       []string // every Unsetenv call's name, in order
	unsetErrKey string   // Unsetenv of this key fails and leaves it set
}

func (f *fakeSim) Boot(u string, e []string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.bootErr {
		return errors.New("Unable to boot device in current state: Booted")
	}
	f.booted = e
	return nil
}
func (f *fakeSim) Shutdown(string) error                  { return nil }
func (f *fakeSim) WaitBooted(string, time.Duration) error { return nil }
func (f *fakeSim) Getenv(_ string, k string) (string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.getErr {
		return "", errors.New("spawn failed")
	}
	return f.env[k], nil
}
func (f *fakeSim) Unsetenv(_ string, k string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.unset = append(f.unset, k)
	if k != "" && k == f.unsetErrKey {
		return errors.New("launchctl unsetenv failed")
	}
	delete(f.env, k)
	return nil
}
func (f *fakeSim) Setenv(_ string, k, v string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.order = append(f.order, k)
	if k == f.failKey {
		return nil
	}
	if f.flaky > 0 {
		f.flaky--
		return nil
	}
	f.env[k] = v
	return nil
}

func set(t *testing.T) artifacts.Set {
	return artifacts.Set{Stub: "/h/builds/s1/libsimcamstub.dylib", Loader: "/h/builds/b1/libsimcamloader.dylib", Engine: "/h/builds/b1/libsimcam.dylib"}
}

func TestMergeInsert(t *testing.T) {
	cases := map[string]string{
		"":                    "/s/libsimcamstub.dylib",
		"/t3/serve-sim.dylib": "/t3/serve-sim.dylib:/s/libsimcamstub.dylib",
		"/t3/a.dylib:/old/libsimcamstub.dylib:/t3/a.dylib": "/t3/a.dylib:/s/libsimcamstub.dylib",
	}
	for in, want := range cases {
		if got := MergeInsert(in, "/s/libsimcamstub.dylib"); got != want {
			t.Errorf("%q -> %q, want %q", in, got, want)
		}
	}
	if got := WithoutOurs("/a:/x/libsimcamstub.dylib:/b"); got != "/a:/b" {
		t.Error(got)
	}
}

func TestLaunchdArmAndDisarmKeepOtherTools(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	f := &fakeSim{env: map[string]string{"DYLD_INSERT_LIBRARIES": "/t3/serve-sim.dylib"}, flaky: 1}
	s := set(t)
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err != nil {
		t.Fatal(err)
	}
	if f.env["DYLD_INSERT_LIBRARIES"] != "/t3/serve-sim.dylib:"+s.Stub || f.env["SIMCAM_SIM_DIR"] != home.Sim("U") {
		t.Fatalf("%v", f.env)
	}
	if _, l, err := home.ReadCurrent("U"); err != nil || l != s.Loader {
		t.Fatal(l, err)
	}
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err != nil || strings.Count(f.env["DYLD_INSERT_LIBRARIES"], "libsimcam") != 1 {
		t.Fatalf("re-arm duplicated: %v %v", f.env, err)
	}
	if f.order[len(f.order)-1] != "DYLD_INSERT_LIBRARIES" {
		t.Errorf("DYLD_INSERT_LIBRARIES not set last: %v", f.order)
	}
	if err := Disarm(f, "U", true); err != nil {
		t.Fatal(err)
	}
	if f.env["DYLD_INSERT_LIBRARIES"] != "/t3/serve-sim.dylib" || f.env["SIMCAM_SIM_DIR"] != "" {
		t.Fatalf("after disarm: %v", f.env)
	}
	if _, _, err := home.ReadCurrent("U"); err == nil {
		t.Fatal("current still there")
	}
}

func TestBootArmPassesVarsAndRecords(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	f := &fakeSim{env: map[string]string{}}
	s := set(t)
	vars := Vars("U", s, Options{StubLog: true, Extra: map[string]string{"SIMCAM_LAZY": "0"}})
	if err := ArmBoot(f, "U", s, vars); err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(f.booted, " ")
	for _, want := range []string{"DYLD_INSERT_LIBRARIES=" + s.Stub, "SIMCAM_SIM_DIR=", "SIMCAM_LAZY=0", "SIMCAM_STUB_LOG=" + filepath.Join(home.Logs(), "U", "stub.log")} {
		if !strings.Contains(joined, want) {
			t.Errorf("boot env lacks %s", want)
		}
	}
	if strings.Contains(joined, "SIMCAM_DYLIB=") {
		t.Error("engine pinned in the boot env (upgrades would need a reboot)")
	}
	r, err := ReadRecord("U")
	if err != nil || r.Mode != Boot || r.Build != "b1" {
		t.Fatalf("%+v %v", r, err)
	}
}

func TestLaunchdArmRollsBack(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	// an earlier arm on another build, then a re-arm that can't set SIMCAM_CHAIN
	if err := home.WriteCurrent("U", "/h/builds/b0/libsimcamloader.dylib"); err != nil {
		t.Fatal(err)
	}
	f := &fakeSim{env: map[string]string{"DYLD_INSERT_LIBRARIES": "/t3/serve-sim.dylib:/h/builds/s0/libsimcamstub.dylib"}, failKey: "SIMCAM_CHAIN"}
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err == nil {
		t.Fatal("no error")
	}
	if f.env["DYLD_INSERT_LIBRARIES"] != "/t3/serve-sim.dylib" {
		t.Errorf("DYLD_INSERT_LIBRARIES after rollback: %q", f.env["DYLD_INSERT_LIBRARIES"])
	}
	if _, l, _ := home.ReadCurrent("U"); l != "/h/builds/b0/libsimcamloader.dylib" {
		t.Errorf("current not restored: %q", l)
	}
	if f.env["SIMCAM_SIM_DIR"] != "" || f.env["SIMCAM_STATE_DIR"] != "" {
		t.Errorf("variables set before the failure not unset: %v", f.env)
	}
	// a `current` without a record (how the scripts arm): the rollback took the stub out, so a launchd record for the
	// build `current` is back on says so (without one, `status` would read the sim as armed outside stuntcam)
	if r, err := ReadRecord("U"); err != nil || r.Mode != Launchd || r.Build != "b0" || r.Booting {
		t.Errorf("record after the rollback: %+v %v, want a launchd record for build b0", r, err)
	}
}

// After a rollback the record says what is true about the sim, also where "as it was found" would not:
//   - a sim the scripts armed (a `current`, no record) has lost its stub and settings: a launchd record for the build
//     `current` is back on lets `status` say the stub is gone and `down` clean up; without one it would read as armed;
//   - the in-progress record this process's own BootUnarmed wrote is not a record to put back;
//   - when one of the rollback's own unsets fails, a record names every variable that may be left, for `down`.
func TestLaunchdArmRollbackRecordTellsTheTruth(t *testing.T) {
	b2 := artifacts.Set{Stub: "/h/builds/s2/libsimcamstub.dylib", Loader: "/h/builds/b2/libsimcamloader.dylib",
		Engine: "/h/builds/b2/libsimcam.dylib"}
	for _, c := range []struct {
		name                string
		current, ownBooting bool   // before the arm: a `current` on build b0; BootUnarmed's record of this process
		earlier             bool   // before the arm: a launchd arm on build b1 (with a source the re-arm drops)
		unsetErrKey         string // an unset that fails during the rollback
		wantRecord          bool
		wantBuild           string
		wantVars            []string // names the record must list (a subset check)
	}{
		{name: "script-armed: a current, no record", current: true, wantRecord: true, wantBuild: "b0",
			wantVars: []string{"DYLD_INSERT_LIBRARIES", "SIMCAM_SIM_DIR", "SIMCAM_FIG_SOURCES", "M2_FAKE"}},
		{name: "own boot-in-progress record, no current", ownBooting: true},
		{name: "own boot-in-progress record, a current", ownBooting: true, current: true, wantRecord: true, wantBuild: "b0",
			wantVars: []string{"DYLD_INSERT_LIBRARIES", "SIMCAM_SIM_DIR"}},
		{name: "no record, an unset fails", unsetErrKey: "SIMCAM_CHAIN", wantRecord: true, wantBuild: "b2",
			wantVars: []string{"SIMCAM_CHAIN", "DYLD_INSERT_LIBRARIES"}},
		// SIMCAM_LAZY is a name only the failed arm adds: the earlier arm's record doesn't list it
		{name: "launchd record, an unset fails", earlier: true, unsetErrKey: "SIMCAM_LAZY", wantRecord: true, wantBuild: "b1",
			wantVars: []string{"SIMCAM_LAZY", "SIMCAM_SOURCE", "DYLD_INSERT_LIBRARIES"}},
	} {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		f := &fakeSim{env: map[string]string{}}
		os.MkdirAll(home.Sim("U"), 0o755)
		if c.current {
			if err := home.WriteCurrent("U", "/h/builds/b0/libsimcamloader.dylib"); err != nil {
				t.Fatal(err)
			}
		}
		if c.earlier {
			s1 := set(t)
			if err := ArmLaunchd(f, "U", s1, Vars("U", s1, Options{Extra: map[string]string{"SIMCAM_SOURCE": "scene"}})); err != nil {
				t.Fatal(err)
			}
		}
		if c.ownBooting {
			booting := Record{Mode: Launchd, Build: "b2", At: time.Now().Unix(), Booting: true, Pid: os.Getpid()}
			if err := WriteRecord("U", booting); err != nil {
				t.Fatal(err)
			}
		}
		f.failKey, f.unsetErrKey = "DYLD_INSERT_LIBRARIES", c.unsetErrKey
		vars := Vars("U", b2, Options{Extra: map[string]string{"SIMCAM_LAZY": "0"}})
		if err := ArmLaunchd(f, "U", b2, vars); err == nil || !strings.Contains(err.Error(), "rolled back") {
			t.Fatalf("%s: err %v", c.name, err)
		}
		r, err := ReadRecord("U")
		if !c.wantRecord {
			if err == nil {
				t.Errorf("%s: a record is left: %+v", c.name, r)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: no record; want a launchd record for build %s", c.name, c.wantBuild)
			continue
		}
		if r.Mode != Launchd || r.Build != c.wantBuild || r.Booting || InProgress(r) {
			t.Errorf("%s: record %+v, want a finished launchd record for build %s", c.name, r, c.wantBuild)
		}
		for _, n := range c.wantVars {
			if !slices.Contains(r.Vars, n) {
				t.Errorf("%s: the record doesn't name %s: %v", c.name, n, r.Vars)
			}
		}
	}
}

// A boot-mode record from an earlier boot (the sim was rebooted unarmed since) is not special in a rollback: when one
// of the rollback's unsets fails, the record must name what is left, or `down` (which only unsets for a launchd
// record) never clears it. An env record comes back when the rollback is clean. After the rollback, `down` leaves no
// stuntcam variable set.
func TestLaunchdArmRollbackOverBootAndEnvRecords(t *testing.T) {
	b2 := artifacts.Set{Stub: "/h/builds/s2/libsimcamstub.dylib", Loader: "/h/builds/b2/libsimcamloader.dylib",
		Engine: "/h/builds/b2/libsimcam.dylib"}
	for _, c := range []struct {
		name        string
		before      Record
		unsetErrKey string
		wantMode    Mode
		wantSame    bool // arm.json byte for byte as before
	}{
		{name: "boot record, clean", before: Record{Mode: Boot, Build: "b0", At: 42, Vars: []string{"DYLD_INSERT_LIBRARIES"}},
			wantMode: Boot, wantSame: true},
		{name: "boot record, an unset fails", before: Record{Mode: Boot, Build: "b0", At: 42,
			Vars: []string{"DYLD_INSERT_LIBRARIES"}}, unsetErrKey: "SIMCAM_CHAIN", wantMode: Launchd},
		{name: "env record, clean", before: Record{Mode: Env, Build: "b0", At: 42}, wantMode: Env, wantSame: true},
		{name: "env record, an unset fails", before: Record{Mode: Env, Build: "b0", At: 42}, unsetErrKey: "SIMCAM_CHAIN",
			wantMode: Launchd},
	} {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		f := &fakeSim{env: map[string]string{}}
		os.MkdirAll(home.Sim("U"), 0o755)
		if err := home.WriteCurrent("U", "/h/builds/b0/libsimcamloader.dylib"); err != nil {
			t.Fatal(err)
		}
		if err := WriteRecord("U", c.before); err != nil {
			t.Fatal(err)
		}
		rec, _ := os.ReadFile(recordPath("U"))
		f.failKey, f.unsetErrKey = "DYLD_INSERT_LIBRARIES", c.unsetErrKey
		if err := ArmLaunchd(f, "U", b2, Vars("U", b2, Options{})); err == nil || !strings.Contains(err.Error(), "rolled back") {
			t.Fatalf("%s: err %v", c.name, err)
		}
		after, _ := os.ReadFile(recordPath("U"))
		r, err := ReadRecord("U")
		if err != nil || r.Mode != c.wantMode || r.Build != "b0" || (c.wantSame && string(after) != string(rec)) {
			t.Errorf("%s: record %s (%v), want mode %s for build b0 (unchanged: %v)", c.name, after, err, c.wantMode, c.wantSame)
		}
		if c.unsetErrKey != "" && !slices.Contains(r.Vars, c.unsetErrKey) {
			t.Errorf("%s: the record doesn't name %s, which is still set: %v", c.name, c.unsetErrKey, r.Vars)
		}
		if _, left := f.env[c.unsetErrKey]; c.unsetErrKey != "" && !left {
			t.Fatalf("%s: %s is not set after the rollback: the case tests nothing", c.name, c.unsetErrKey)
		}
		// the unset works at the next try: `down` must then leave nothing of stuntcam's
		f.unsetErrKey = ""
		if err := Disarm(f, "U", true); err != nil {
			t.Fatalf("%s: down: %v", c.name, err)
		}
		if len(f.env) != 0 {
			t.Errorf("%s: after down: %v", c.name, f.env)
		}
	}
}

func TestLaunchdArmAbortsOnUnreadableEnv(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	f := &fakeSim{env: map[string]string{"DYLD_INSERT_LIBRARIES": "/t3/serve-sim.dylib"}, getErr: true}
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err == nil {
		t.Fatal("armed although the current value couldn't be read")
	}
	if len(f.order) != 0 || f.env["DYLD_INSERT_LIBRARIES"] != "/t3/serve-sim.dylib" {
		t.Fatalf("changed something: %v %v", f.order, f.env)
	}
	if _, err := os.Stat(home.CurrentFile("U")); err == nil {
		t.Fatal("current written")
	}
	if err := Disarm(f, "U", true); err != nil { // no record: nothing to read
		t.Fatal(err)
	}
}

func TestDisarmShutDownLaunchdSim(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	f := &fakeSim{env: map[string]string{}}
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err != nil {
		t.Fatal(err)
	}
	f.getErr = true // a shut-down sim: spawn fails
	if err := Disarm(f, "U", false); err != nil {
		t.Fatal(err)
	}
	if _, err := ReadRecord("U"); err == nil {
		t.Fatal("record kept")
	}
}

func TestDownMarker(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	f := &fakeSim{env: map[string]string{}}
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err != nil {
		t.Fatal(err)
	}
	if err := Disarm(f, "U", true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(DownMarker("U")); err != nil {
		t.Fatal("down left no marker (the login service would re-arm the sim)")
	}
	if err := ArmBoot(f, "U", s, Vars("U", s, Options{})); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(DownMarker("U")); err == nil {
		t.Fatal("an arm kept the down marker")
	}
}

func TestArmBootRecords(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	f := &fakeSim{env: map[string]string{}}
	if err := ArmBoot(f, "U", s, Vars("U", s, Options{})); err != nil {
		t.Fatal(err)
	}
	r, _ := ReadRecord("U")
	if r.Booting || InProgress(r) || r.Mode != Boot {
		t.Fatalf("finished boot still in progress: %+v", r)
	}
	// a failed boot puts the previous record back
	f.bootErr = true
	WriteRecord("U", Record{Mode: Launchd, Build: "old", At: 1})
	if err := ArmBoot(f, "U", s, Vars("U", s, Options{})); err == nil {
		t.Fatal("no error")
	}
	if r, _ := ReadRecord("U"); r.Mode != Launchd || r.Build != "old" {
		t.Fatalf("record after a failed boot: %+v", r)
	}
	// and removes it when there was none
	os.Remove(filepath.Join(home.Sim("V"), "arm.json"))
	if err := ArmBoot(f, "V", s, Vars("V", s, Options{})); err == nil {
		t.Fatal("no error")
	}
	if _, err := ReadRecord("V"); err == nil {
		t.Fatal("a failed first boot left a record")
	}
	now := time.Now().Unix()
	if !InProgress(Record{Booting: true, At: now, Pid: os.Getpid()}) {
		t.Fatal("a live boot isn't in progress")
	}
	for _, r := range []Record{{Booting: true, At: now}, {Booting: true, At: now, Pid: 999999}, {Booting: true, At: now - 300, Pid: os.Getpid()}, {Booting: true, At: now + 600, Pid: os.Getpid()}} {
		if InProgress(r) {
			t.Errorf("stale or ownerless boot in progress: %+v", r)
		}
	}
	// a failed boot puts current and the down marker back too
	f.bootErr = true
	home.WriteCurrent("W", "/h/builds/b0/libsimcamloader.dylib")
	os.WriteFile(DownMarker("W"), []byte("1\n"), 0o644)
	if err := ArmBoot(f, "W", s, Vars("W", s, Options{})); err == nil {
		t.Fatal("no error")
	}
	if _, l, _ := home.ReadCurrent("W"); l != "/h/builds/b0/libsimcamloader.dylib" {
		t.Errorf("current after a failed boot: %q", l)
	}
	if _, err := os.Stat(DownMarker("W")); err != nil {
		t.Error("down marker lost by a failed boot")
	}
}

// config.json keeps & < > unescaped (SetEscapeHTML(false)), so paths with & read back as written
func TestWriteConfigKeepsAmpersand(t *testing.T) {
	h := filepath.Join(t.TempDir(), "stuntcam e30 & <co>")
	t.Setenv("SIMCAM_HOME", h)
	rd := filepath.Join(h, "restore-rules", "0123456789ab")
	if err := WriteConfig("U9", xcode.Info{Version: "27.0", Build: "27A266a", DeveloperDir: "/Applications/Xcode & Co.app/Contents/Developer"}, rd, "up", []string{"ASAN_OPTIONS=a=1&b=2"}); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(home.ConfigFile("U9"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(b), `\u00`) || !strings.Contains(string(b), `"rules_dir": "`+rd+`"`) || !strings.Contains(string(b), "Xcode & Co.app") {
		t.Fatalf("%s", b)
	}
	c, err := ReadConfig("U9")
	if err != nil || c.RulesDir != rd || c.Build != "27A266a" || c.Proto != home.Proto || len(c.BootSignals) != 1 {
		t.Fatalf("%+v %v", c, err)
	}
}

func TestSignals(t *testing.T) {
	env := []string{"HOME=/h", "TSAN_OPTIONS=x", "IDB_COMPANION=1", "ASAN_OPTIONS=detect_leaks=0", "MALLOC_PROTECT_BEFORE=1", "MallocGuardEdges=1", "ASAN_OPTIONSX=1", "IDB"}
	if got := Signals(env); strings.Join(got, " ") != "ASAN_OPTIONS=detect_leaks=0 IDB_COMPANION=1 MALLOC_PROTECT_BEFORE=1 TSAN_OPTIONS=x" {
		t.Fatal(got)
	}
	if got := ChildSignals([]string{"SIMCTL_CHILD_IDB_X=1", "IDB_Y=1", "SIMCTL_CHILD_SIMCAM_LAZY=0"}); len(got) != 1 || got[0] != "IDB_X=1" {
		t.Fatal(got)
	}
	if got := Signals(nil); got == nil || len(got) != 0 {
		t.Fatal("nil")
	}
}

// rules_dir "" must be written (the loader reads "" as "built-in rules"; a missing key means the legacy flat dir), and a
// config.json the loader can't hold drops the boot signals rather than becoming unreadable
func TestWriteConfigEmptyRulesDirAndSize(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if err := WriteConfig("UA", xcode.Info{Build: "27A266a"}, "", "up", nil); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(home.ConfigFile("UA")); !strings.Contains(string(b), `"rules_dir": ""`) {
		t.Fatalf("%s", b)
	}
	big := []string{"ASAN_OPTIONS=" + strings.Repeat("x", MaxConfigSize)}
	if err := WriteConfig("UB", xcode.Info{Build: "27A266a"}, "/r", "up", big); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(home.ConfigFile("UB"))
	if c, err := ReadConfig("UB"); err != nil || len(b) > MaxConfigSize || len(c.BootSignals) != 0 || c.RulesDir != "/r" {
		t.Fatalf("%d bytes, %+v %v", len(b), c, err)
	}
	if err := SetRulesDir("UB", "/r2"); err != nil {
		t.Fatal(err)
	}
	if c, _ := ReadConfig("UB"); c.RulesDir != "/r2" || c.Build != "27A266a" {
		t.Fatalf("%+v", c)
	}
	if err := SetRulesDir("UC", "/r2"); err == nil {
		t.Fatal("SetRulesDir without a config.json")
	}
}

// a re-arm unsets what an earlier launchd arm set and this one doesn't, records both, and sets DYLD_INSERT_LIBRARIES last
func TestLaunchdArmDropsEarlierVars(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	f := &fakeSim{env: map[string]string{}}
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{Extra: map[string]string{"SIMCAM_SOURCE": "scene"}})); err != nil {
		t.Fatal(err)
	}
	f.order = nil
	if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{})); err != nil {
		t.Fatal(err)
	}
	if _, ok := f.env["SIMCAM_SOURCE"]; ok {
		t.Fatal("SIMCAM_SOURCE from the earlier arm is still set")
	}
	if last := f.order[len(f.order)-1]; last != "DYLD_INSERT_LIBRARIES" {
		t.Fatalf("set last: %s (%v)", last, f.order)
	}
	r, _ := ReadRecord("U")
	if got := strings.Join(r.Vars, " "); !strings.Contains(got, "SIMCAM_SOURCE") || !strings.HasPrefix(got, "DYLD_INSERT_LIBRARIES ") {
		t.Fatalf("record: %v", r.Vars)
	}
}

// a failed launchd arm of a sim that another tool also inserts into, and that already had a `current`: the rollback
// writes the old `current` back, leaves the other tool's DYLD_INSERT_LIBRARIES entry (without stuntcam's stub) and
// unsets the settings, whether a setting or DYLD_INSERT_LIBRARIES itself didn't stick
func TestLaunchdArmRollbackRestoresCurrentAndOtherTools(t *testing.T) {
	for _, failKey := range []string{"SIMCAM_CHAIN", "DYLD_INSERT_LIBRARIES"} {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		s := set(t)
		const before = "/x/libother.dylib:/old/builds/s0/libsimcamstub.dylib"
		f := &fakeSim{env: map[string]string{"DYLD_INSERT_LIBRARIES": before}, failKey: failKey}
		os.MkdirAll(home.Sim("U"), 0o755)
		oldCurrent := []byte("1 /old/builds/b0/libsimcamloader.dylib\n")
		if err := os.WriteFile(home.CurrentFile("U"), oldCurrent, 0o644); err != nil {
			t.Fatal(err)
		}
		err := ArmLaunchd(f, "U", s, Vars("U", s, Options{}))
		if err == nil || !strings.Contains(err.Error(), "launchctl setenv "+failKey+" did not stick") ||
			!strings.Contains(err.Error(), "rolled back") {
			t.Fatalf("%s: err %v", failKey, err)
		}
		if c, _ := os.ReadFile(home.CurrentFile("U")); string(c) != string(oldCurrent) {
			t.Errorf("%s: current not put back: %q", failKey, c)
		}
		// the fake never lets failKey stick, so when DYLD_INSERT_LIBRARIES is the one that fails it still reads as before
		wantInsert := "/x/libother.dylib"
		if failKey == "DYLD_INSERT_LIBRARIES" {
			wantInsert = before
		}
		if got := f.env["DYLD_INSERT_LIBRARIES"]; got != wantInsert {
			t.Errorf("%s: DYLD_INSERT_LIBRARIES %q, want %q", failKey, got, wantInsert)
		}
		if len(f.env) != 1 {
			t.Errorf("%s: variables left: %v", failKey, f.env)
		}
	}
}

// a failed launchd arm of a sim with a boot record from an earlier boot puts that record and its down marker back, and
// says it rolled back
func TestLaunchdArmRollbackKeepsBootRecordAndDown(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	s := set(t)
	f := &fakeSim{env: map[string]string{}, failKey: "SIMCAM_CHAIN"}
	os.MkdirAll(home.Sim("U"), 0o755)
	boot := Record{Mode: Boot, Build: "b0", At: 42, Vars: []string{"DYLD_INSERT_LIBRARIES"}}
	if err := WriteRecord("U", boot); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(recordPath("U"))
	os.WriteFile(DownMarker("U"), []byte("1\n"), 0o644)
	err := ArmLaunchd(f, "U", s, Vars("U", s, Options{}))
	want := "launchctl setenv SIMCAM_CHAIN did not stick (reads \"\"); rolled back (other tools' DYLD_INSERT_LIBRARIES " +
		"entries kept, stuntcam's removed)"
	if err == nil || err.Error() != want {
		t.Fatalf("err %v", err)
	}
	if after, _ := os.ReadFile(recordPath("U")); string(after) != string(before) {
		t.Fatalf("boot record not put back: %s", after)
	}
	if _, err := os.Stat(DownMarker("U")); err != nil {
		t.Fatal("down marker not put back")
	}
	if _, err := os.Stat(home.CurrentFile("U")); err == nil {
		t.Fatal("current left behind")
	}
	if len(f.env) != 0 {
		t.Fatalf("variables left: %v", f.env)
	}
}

// a failed launchd arm's rollback leaves arm.json as it found it: none stays none, a boot record and an earlier launchd
// record are put back byte for byte. After the rollback of a re-arm, the earlier arm's `current` is back and none of its
// variables is set, which is what its record says once `status` finds the stub gone; `down` then still knows its names.
func TestLaunchdArmRollbackPutsBackTheRecordItFound(t *testing.T) {
	for _, before := range []string{"none", "boot", "launchd"} {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		s := set(t)
		f := &fakeSim{env: map[string]string{}}
		os.MkdirAll(home.Sim("U"), 0o755)
		switch before {
		case "boot":
			if err := WriteRecord("U", Record{Mode: Boot, Build: "b0", At: 42, Vars: []string{"DYLD_INSERT_LIBRARIES"}}); err != nil {
				t.Fatal(err)
			}
		case "launchd": // an earlier launchd arm with a source; the re-arm drops it
			if err := ArmLaunchd(f, "U", s, Vars("U", s, Options{Extra: map[string]string{"SIMCAM_SOURCE": "scene"}})); err != nil {
				t.Fatal(err)
			}
		}
		rec, recErr := os.ReadFile(recordPath("U"))
		cur, curErr := os.ReadFile(home.CurrentFile("U"))
		// an arm of another build whose stub never sticks
		s2 := artifacts.Set{Stub: "/h/builds/s2/libsimcamstub.dylib", Loader: "/h/builds/b2/libsimcamloader.dylib",
			Engine: "/h/builds/b2/libsimcam.dylib"}
		f.failKey = "DYLD_INSERT_LIBRARIES"
		if err := ArmLaunchd(f, "U", s2, Vars("U", s2, Options{})); err == nil || !strings.Contains(err.Error(), "rolled back") {
			t.Fatalf("%s: err %v", before, err)
		}
		after, afterErr := os.ReadFile(recordPath("U"))
		switch {
		case recErr != nil && afterErr == nil:
			t.Errorf("%s: the failed arm's record is left: %s", before, after)
		case recErr == nil && string(after) != string(rec):
			t.Errorf("%s: record not put back:\n%s\nwant\n%s", before, after, rec)
		}
		if c, err := os.ReadFile(home.CurrentFile("U")); (err == nil) != (curErr == nil) || string(c) != string(cur) {
			t.Errorf("%s: current %q, want %q", before, c, cur)
		}
		if len(f.env) != 0 {
			t.Errorf("%s: variables left: %v", before, f.env)
		}
	}
}
