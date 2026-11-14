// SPDX-License-Identifier: Apache-2.0
package arm

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

type fakeSim struct {
	mu      sync.Mutex // ArmLaunchd sets variables concurrently
	env     map[string]string
	booted  []string
	flaky   int    // Setenv calls that silently don't stick
	failKey string // Setenv of this key never sticks
	getErr  bool   // Getenv fails
	bootErr bool   // Boot fails
	order   []string
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
	// a later down still knows every name, and the record is cumulative
	r, _ := ReadRecord("U")
	if !strings.Contains(strings.Join(r.Vars, " "), "SIMCAM_CHAIN") {
		t.Errorf("record: %v", r.Vars)
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

// L5 (w4 e30): config.json keeps & < > as they are (json.Marshal wrote &, which the loader's jstr didn't decode)
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
// config.json the loader can't hold drops the boot signals rather than becoming unreadable (review r2 L-d, nit)
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
