// SPDX-License-Identifier: Apache-2.0
package main

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/verify"
	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

func TestStageSource(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if s, err := stageSource("scene"); err != nil || s != "scene" {
		t.Fatal(s, err)
	}
	for _, bad := range []string{"", "picture:x", "image:", "image:/nonexistent.png", "webcam"} {
		if _, err := stageSource(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
	img := filepath.Join(t.TempDir(), "a.png")
	os.WriteFile(img, []byte("png bytes"), 0o644)
	s, err := stageSource("image:" + img)
	if err != nil || !strings.HasPrefix(s, "image:"+home.Data()+"/") || !strings.HasSuffix(s, "/a.png") {
		t.Fatalf("staged %q %v", s, err)
	}
}

// a sim that isn't booted: each key's error says how to get its setting to the sim's apps
func TestControllableNotRunning(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if err := home.WriteCurrent("U", "/h/builds/b/libsimcamloader.dylib"); err != nil {
		t.Fatal(err)
	}
	if err := arm.WriteRecord("U", arm.Record{Mode: arm.Env, At: time.Now().Unix()}); err != nil {
		t.Fatal(err)
	}
	off := simctl.Device{UDID: "U", Name: "iPhone", State: "Shutdown"}
	for key, want := range map[string]string{
		"source":      "iPhone isn't running; `stuntcam up --source …` sets the picture when it boots",
		"orientation": "iPhone isn't running: `stuntcam up` boots it armed, then `stuntcam orient`",
		"event":       "iPhone isn't running: `stuntcam up` boots it armed, then `stuntcam `",
	} {
		if err := controllable(off, key); err == nil || err.Error() != want {
			t.Errorf("%s: %v, want %s", key, err, want)
		}
	}
}

func TestSetControl(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	now := time.Now()
	d := simctl.Device{UDID: "U", Name: "iPhone", State: "Booted", LastBooted: now.Add(-time.Minute)}
	if err := setControl(d, "source", "scene"); err == nil {
		t.Fatal("set control on an unarmed sim")
	}
	home.WriteCurrent("U", "/h/builds/b/libsimcamloader.dylib")
	if err := setControl(d, "source", "scene"); err == nil {
		t.Fatal("no record (armed by the scripts): nothing will read the file")
	}
	arm.WriteRecord("U", arm.Record{Mode: arm.Launchd, Vars: []string{"DYLD_INSERT_LIBRARIES"}})
	if err := setControl(d, "source", "scene"); err == nil || !strings.Contains(err.Error(), "up") {
		t.Fatalf("an arm without SIMCAM_CONTROL must ask for a re-arm: %v", err)
	}
	arm.WriteRecord("U", arm.Record{Mode: arm.Boot, At: now.Add(-time.Hour).Unix(), Vars: []string{"SIMCAM_CONTROL"}})
	if err := setControl(d, "source", "scene"); err == nil || !strings.Contains(err.Error(), "--reboot") {
		t.Fatalf("a boot record from an earlier boot: %v", err)
	}
	off := d
	off.State = "Shutdown"
	arm.WriteRecord("U", arm.Record{Mode: arm.Env, At: now.Unix()})
	if err := setControl(off, "source", "scene"); err == nil {
		t.Fatal("a shut-down sim")
	}
	if err := setControl(d, "source", "scene"); err != nil {
		t.Fatalf("env-armed sim: %v", err)
	}
	arm.WriteRecord("U", arm.Record{Mode: arm.Launchd, Vars: []string{"DYLD_INSERT_LIBRARIES", "SIMCAM_CONTROL"}})
	// launchd mode asks the sim: a fake xcrun answers `launchctl getenv SIMCAM_CONTROL` with $FAKE_CONTROL
	bin := t.TempDir()
	os.WriteFile(filepath.Join(bin, "xcrun"), []byte("#!/bin/sh\ncase \"$*\" in *'spawn U launchctl getenv SIMCAM_CONTROL') printf '%s\\n' \"$FAKE_CONTROL\";; *) exit 3;; esac\n"), 0o755)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("FAKE_CONTROL", "")
	if err := setControl(d, "source", "scene"); err == nil || !strings.Contains(err.Error(), "older stuntcam") {
		t.Fatalf("the record names SIMCAM_CONTROL but the sim's launchd has none (an older stuntcam re-armed it): %v", err)
	}
	t.Setenv("FAKE_CONTROL", home.ControlFile("U"))
	os.WriteFile(home.ControlFile("U"), []byte("event=interrupt@1\nsource=scene\n"), 0o644)
	if err := setControl(d, "source", "image:/x.png"); err != nil {
		t.Fatal(err)
	}
	if err := setControl(d, "orientation", "landscapeLeft"); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(home.ControlFile("U"))
	if got := string(b); got != "event=interrupt@1\nsource=image:/x.png\norientation=landscapeLeft\n" {
		t.Fatalf("control file:\n%s", got)
	}
}

func TestParseRejectsOptionsAfterArguments(t *testing.T) {
	if err := cmdSource([]string{"scene", "--udid", "X"}); err == nil {
		t.Fatal("swap-style trap: --udid after the argument must be refused")
	}
	if err := cmdOrient([]string{"sideways"}); err == nil {
		t.Fatal("bad orientation accepted")
	}
}

func TestDestinationIDsUpperCase(t *testing.T) { // simctl's UDIDs are upper case; a lower-case id= must still match them
	got := destinationIDs([]string{"xcodebuild", "-destination", "id=0a4c42a4-d406", "-destination", "id=0A4C42A4-D406"})
	if len(got) != 1 || got[0] != "0A4C42A4-D406" {
		t.Fatalf("got %v", got)
	}
}

func TestDestinationIDs(t *testing.T) {
	got := destinationIDs([]string{"xcodebuild", "test", "-destination", "platform=iOS Simulator,id=A-1", "-destination", "id=B-2", "-scheme", "id=C"})
	if !reflect.DeepEqual(got, []string{"A-1", "B-2"}) {
		t.Fatal(got)
	}
	if got := destinationIDs([]string{"xcodebuild", "-destination", "platform=iOS Simulator,name=iPhone 17 Pro"}); got != nil {
		t.Fatal(got)
	}
}

// boot mode is read from launchd_sim's environment: status says boot-armed even with no arm record, and
// booted-unarmed after a plain reboot of a sim whose record says boot mode
func TestStatusFromBootEnv(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	old := bootEnv
	defer func() { bootEnv = old }()
	now := time.Now()
	d := simctl.Device{UDID: "U1", Name: "iPhone", State: "Booted", LastBooted: now.Add(-time.Minute)}
	stub := "/h/builds/09908f5dbe40/libsimcamstub.dylib"
	armedEnv := map[string]string{"DYLD_INSERT_LIBRARIES": stub, "SIMCAM_SIM_DIR": home.Sim("U1")}
	bootEnv = func(string) (map[string]string, error) { return armedEnv, nil }
	if err := home.WriteCurrent("U1", "/h/builds/c253a7ced041/libsimcamloader.dylib"); err != nil {
		t.Fatal(err)
	}
	// armed by scripts/boot.sh: no record, but the stub is in the boot environment
	if s := statusOf(d); s.Mode != "boot" || s.BootEnv != "stub" || s.Build != "c253a7ced041" || !strings.Contains(s.Note, "outside") {
		t.Fatalf("scripts boot: %+v", s)
	}
	// armed by `up`: record, config
	arm.WriteRecord("U1", arm.Record{Mode: arm.Boot, Build: "c253a7ced041", At: now.Unix()}) // after LastBooted: same boot by the timestamps
	arm.WriteConfig("U1", xcodeInfo("27A266a"), "/h/restore-rules/0123456789ab", "up", nil)
	if s := statusOf(d); s.Mode != "boot" || s.Note != "" || s.Xcode != "27A266a" || s.ArmedAt == 0 {
		t.Fatalf("up boot: %+v", s)
	}
	// rebooted without stuntcam (the record's timestamps alone would say "same boot" here: LastBooted is before At+5, as when
	// simctl reports a stale boot time)
	bootEnv = func(string) (map[string]string, error) { return map[string]string{"DYLD_ROOT_PATH": "/rt"}, nil }
	if s := statusOf(d); s.Mode != "booted-unarmed" || s.BootEnv != "no stub" || !strings.Contains(s.Note, "rebooted") {
		t.Fatalf("plain reboot: %+v", s)
	}
	// `down`: current removed, the boot env still has the stub
	bootEnv = func(string) (map[string]string, error) { return armedEnv, nil }
	os.Remove(home.CurrentFile("U1"))
	if s := statusOf(d); s.Mode != "booted-unarmed" || !strings.Contains(s.Note, "down") {
		t.Fatalf("down: %+v", s)
	}
	// booted by `stuntcam exec`'s command: another sim dir
	run := home.Sim("exec-1-2")
	home.WriteCurrent("exec-1-2", "/h/builds/aaaaaaaaaaaa/libsimcamloader.dylib")
	bootEnv = func(string) (map[string]string, error) {
		return map[string]string{"DYLD_INSERT_LIBRARIES": "/x/libother.dylib:" + stub, "SIMCAM_SIM_DIR": run}, nil
	}
	if s := statusOf(d); s.Mode != "boot" || s.Build != "aaaaaaaaaaaa" || !strings.Contains(s.Note, run) {
		t.Fatalf("exec: %+v", s)
	}
	// armed by a stuntcam with another SIMCAM_HOME
	bootEnv = func(string) (map[string]string, error) {
		return map[string]string{"DYLD_INSERT_LIBRARIES": stub, "SIMCAM_SIM_DIR": "/other/home/sims/U1"}, nil
	}
	if s := statusOf(d); s.Mode != "boot" || !strings.Contains(s.Note, "another stuntcam home") {
		t.Fatalf("other home: %+v", s)
	}
	if dir := foreignBootArm(d); dir != "/other/home/sims/U1" || !strings.Contains(foreignBootArmMsg(d, dir), "--reboot") || !strings.Contains(foreignBootArmMsg(d, dir), "another stuntcam home") {
		t.Fatalf("up must refuse a sim armed under another home: %q", dir)
	}
	// the stub without SIMCAM_SIM_DIR is not "another directory"
	bootEnv = func(string) (map[string]string, error) { return map[string]string{"DYLD_INSERT_LIBRARIES": stub}, nil }
	if s := statusOf(d); strings.Contains(s.Note, "another") || foreignBootArm(d) != "" {
		t.Fatalf("no sim dir: %+v", s)
	}
	// armed from this home: up goes ahead
	bootEnv = func(string) (map[string]string, error) { return armedEnv, nil }
	if dir := foreignBootArm(d); dir != "" {
		t.Fatalf("own home: %q", dir)
	}
	// unreadable boot env: the record decides
	bootEnv = func(string) (map[string]string, error) { return nil, os.ErrPermission }
	home.WriteCurrent("U1", "/h/builds/c253a7ced041/libsimcamloader.dylib")
	if s := statusOf(d); s.Mode != "boot" || !strings.HasPrefix(s.BootEnv, "unreadable") {
		t.Fatalf("unreadable: %+v", s)
	}
}

func xcodeInfo(build string) xcode.Info {
	return xcode.Info{Version: "27.0", Build: build, DeveloperDir: "/Applications/Xcode.app/Contents/Developer"}
}

func TestXcodeWarnings(t *testing.T) {
	old := selectedXcode
	defer func() { selectedXcode = old }()
	selectedXcode = func() (string, error) { return "/Applications/Xcode.app/Contents/Developer", nil }
	one := []xcode.Info{xcodeInfo("27A266a")}
	act := xcodeInfo("27A266a")
	if w := xcodeWarnings(act, nil, one, "", []string{"/Applications/Xcode.app"}); len(w) != 0 {
		t.Fatalf("one Xcode, running, no DEVELOPER_DIR: %+v", w)
	}
	// DEVELOPER_DIR naming the .app is the same Xcode as xcode-select's Contents/Developer
	for _, dd := range []string{"/Applications/Xcode.app", "/Applications/Xcode.app/", "/Applications/Xcode.app/Contents/Developer/"} {
		if w := xcodeWarnings(act, nil, one, dd, nil); len(w) != 0 {
			t.Fatalf("DEVELOPER_DIR=%s: %+v", dd, w)
		}
	}
	if w := xcodeWarnings(act, nil, one, "/x/Xcode-27.1.app", nil); len(w) != 1 || !w[0].Warn || !strings.Contains(w[0].Msg, "DEVELOPER_DIR=/x/Xcode-27.1.app/Contents/Developer differs") {
		t.Fatalf("real mismatch: %+v", w)
	}
	two := append(one, xcode.Info{Version: "27.1", Build: "27A9275", DeveloperDir: "/Users/x/xcodes/Xcode-27.1.app/Contents/Developer"})
	// several Xcodes alone is information
	if w := xcodeWarnings(act, nil, two, "", []string{"/Applications/Xcode.app"}); len(w) != 1 || w[0].Warn || !strings.Contains(w[0].Msg, "several Xcodes") {
		t.Fatalf("two, no mismatch: %+v", w)
	}
	w := xcodeWarnings(act, nil, two, "", []string{"/Users/x/xcodes/Xcode-27.1.app"})
	if len(w) != 2 || !w[0].Warn || !strings.Contains(w[0].Msg, "several Xcodes") || !strings.Contains(w[0].Msg, "Xcode-27.1.app (27.1 27A9275)") || !strings.Contains(w[1].Msg, "running Xcode /Users/x/xcodes/Xcode-27.1.app") {
		t.Fatalf("%+v", w)
	}
	// an Xcode on /Volumes (a disk image, a backup) doesn't count, unless it is the one in use
	vol := append(one, xcode.Info{Version: "26.6", Build: "17F113", DeveloperDir: "/Volumes/Backup/Applications/Xcode.app/Contents/Developer"})
	if w := xcodeWarnings(act, nil, vol, "", nil); len(w) != 0 {
		t.Fatalf("/Volumes: %+v", w)
	}
	if w := xcodeWarnings(vol[1], nil, vol, "", nil); len(w) != 1 || w[0].Warn {
		t.Fatalf("/Volumes Xcode in use: %+v", w)
	}
}

// an Xcode that can't be identified keeps the existing record instead of writing an empty build
func TestRecordXcodeKeepsRecordOnFailure(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	oa, ob := xcodeActive, bootEnv
	defer func() { xcodeActive, bootEnv = oa, ob }()
	bootEnv = func(string) (map[string]string, error) { return nil, os.ErrNotExist }
	xcodeActive = func() (xcode.Info, error) { return xcodeInfo("27A266a"), nil }
	t.Setenv("SIMCTL_CHILD_ASAN_OPTIONS", "detect_leaks=0")
	t.Setenv("SIMCTL_CHILD_SIMCAM_LAZY", "0")
	recordXcode("U3", "/h/rr/0123456789ab", "up", noXcodeSummary, signalsFromThisProcess)
	c, err := arm.ReadConfig("U3")
	if err != nil || c.Build != "27A266a" || c.RulesDir != "/h/rr/0123456789ab" || !reflect.DeepEqual(c.BootSignals, []string{"ASAN_OPTIONS=detect_leaks=0"}) {
		t.Fatalf("%+v %v", c, err)
	}
	xcodeActive = func() (xcode.Info, error) { return xcode.Info{}, errors.New("xcodebuild timed out after 20s") }
	recordXcode("U3", "/h/rr/ffffffffffff", "swap", noXcodeSummary, signalsFromLaunchdSim)
	// the Xcode is kept; the rules dir, boot signals and source are this arm's
	if c2, _ := arm.ReadConfig("U3"); c2.Build != "27A266a" || c2.RulesDir != "/h/rr/ffffffffffff" || c2.Source != "swap" || len(c2.BootSignals) != 0 {
		t.Fatalf("record not kept/updated: %+v", c2)
	}
	recordXcode("U4", "", "up", noXcodeSummary, signalsFromLaunchdSim) // no record yet: one is written, without a build (the loader says unverified)
	if c3, err := arm.ReadConfig("U4"); err != nil || c3.Build != "" {
		t.Fatalf("%+v %v", c3, err)
	}
	// not booting: the signals come from the booted sim's launchd_sim
	xcodeActive = func() (xcode.Info, error) { return xcodeInfo("27A266a"), nil }
	bootEnv = func(string) (map[string]string, error) {
		return map[string]string{"IDB_COMPANION": "1", "DYLD_INSERT_LIBRARIES": "/x/libsimcamstub.dylib", "HOME": "/h"}, nil
	}
	recordXcode("U5", "", "env", noXcodeSummary, signalsFromLaunchdSim)
	if c5, _ := arm.ReadConfig("U5"); !reflect.DeepEqual(c5.BootSignals, []string{"IDB_COMPANION=1"}) {
		t.Fatalf("%+v", c5)
	}
}

func TestRecordWarning(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	old := sourceRulesDir // never the checkout's own build (a built checkout would make the stale-rules warning fire)
	defer func() { sourceRulesDir = old }()
	sourceRulesDir = func() string { return filepath.Join(t.TempDir(), "no-rules") }
	d := simctl.Device{UDID: "U2", Name: "iPhone"}
	if w := recordWarning(d, simStatus{}, xcodeInfo("27A266a"), nil); !strings.Contains(w, "no Xcode recorded") {
		t.Fatal(w)
	}
	arm.WriteConfig("U2", xcodeInfo("27A266a"), "/h/restore-rules/0123456789ab", "up", nil)
	if w := recordWarning(d, simStatus{}, xcodeInfo("27A266a"), nil); w != "" {
		t.Fatal(w)
	}
	dotApp := xcodeInfo("27A266a")
	dotApp.DeveloperDir = "/Applications/Xcode.app" // the same Xcode
	if w := recordWarning(d, simStatus{}, dotApp, nil); w != "" {
		t.Fatal(w)
	}
	other := xcode.Info{Version: "27.1", Build: "27A9275", DeveloperDir: "/x/Xcode-27.1.app/Contents/Developer"}
	if w := recordWarning(d, simStatus{}, other, nil); !strings.Contains(w, "armed for Xcode 27A266a") || !strings.Contains(w, "in use is 27A9275") {
		t.Fatal(w)
	}
	// a sim armed with another directory: its config.json there decides
	other2 := filepath.Join(t.TempDir(), "sims", "U2")
	os.MkdirAll(other2, 0o755)
	os.WriteFile(filepath.Join(other2, "config.json"), []byte(`{"proto":1,"xcode_build":"27A266a","developer_dir":"/Applications/Xcode.app/Contents/Developer"}`), 0o644)
	if w := recordWarning(d, simStatus{SimDir: other2}, xcodeInfo("27A266a"), nil); w != "" {
		t.Fatal(w)
	}
	if w := recordWarning(d, simStatus{SimDir: filepath.Join(t.TempDir(), "none")}, xcodeInfo("27A266a"), nil); !strings.Contains(w, "no Xcode recorded") {
		t.Fatal(w)
	}
	c, err := arm.ReadConfig("U2")
	if err != nil || c.Proto != 1 || c.Build != "27A266a" || c.RulesDir != "/h/restore-rules/0123456789ab" || c.Source != "up" {
		t.Fatalf("%+v %v", c, err)
	}
}

// the sim's own staged rules lack the recorded build, but this build's rules have it
func TestRecordWarningStaleSimRules(t *testing.T) {
	h := t.TempDir()
	t.Setenv("SIMCAM_HOME", h)
	old := sourceRulesDir
	defer func() { sourceRulesDir = old }()
	src, simr := filepath.Join(h, "src-rules"), filepath.Join(h, "restore-rules", "aaaaaaaaaaaa")
	for _, d := range []string{src, simr} {
		os.MkdirAll(d, 0o755)
		os.WriteFile(filepath.Join(d, "27A266a.json"), []byte(`{"proto":1,"rules":[]}`), 0o644)
	}
	os.WriteFile(filepath.Join(src, "27B5.json"), []byte(`{"proto":1,"rules":[]}`), 0o644)
	sourceRulesDir = func() string { return src }
	d := simctl.Device{UDID: "U7", Name: "iPhone"}
	arm.WriteConfig("U7", xcodeInfo("27B5"), simr, "up", nil)
	if w := recordWarning(d, simStatus{}, xcodeInfo("27B5"), nil); !strings.Contains(w, "have none for Xcode 27B5") || !strings.Contains(w, "stuntcam swap") {
		t.Fatal(w)
	}
	if err := arm.SetRulesDir("U7", src); err != nil { // what `swap` does
		t.Fatal(err)
	}
	if c, _ := arm.ReadConfig("U7"); c.RulesDir != src || c.Build != "27B5" || c.Source != "up" {
		t.Fatalf("%+v", c)
	}
	if w := recordWarning(d, simStatus{}, xcodeInfo("27B5"), nil); w != "" {
		t.Fatal(w)
	}
	// rules_dir "" is the built-in rules: verified for 27A266a, no warning
	arm.WriteConfig("U8", xcodeInfo("27A266a"), "", "up", nil)
	if w := recordWarning(simctl.Device{UDID: "U8", Name: "x"}, simStatus{}, xcodeInfo("27A266a"), nil); w != "" {
		t.Fatal(w)
	}
}

func TestWillRebootBoot(t *testing.T) {
	for _, c := range []struct {
		m      arm.Mode
		reboot bool
		want   bool
	}{{"", false, false}, {"", true, true}, {arm.Boot, false, true}, {arm.Boot, true, true}, {arm.Launchd, false, false}, {arm.Launchd, true, false}} {
		if got := willRebootBoot(c.m, c.reboot); got != c.want {
			t.Errorf("willRebootBoot(%q, %v) = %v", c.m, c.reboot, got)
		}
	}
}

func TestParseRunningXcodes(t *testing.T) {
	ps := "/Applications/Xcode.app/Contents/MacOS/Xcode\n/Applications/Xcode.app/Contents/SharedFrameworks/X.framework/xcodebuild\n" +
		"/Users/x/xcodes/Xcode 27.1.app/Contents/MacOS/Xcode -ApplePersistenceIgnoreState YES\n/Applications/Xcode.app/Contents/MacOS/XcodeHelper\n" +
		"/Applications/Xcode.app/Contents/MacOS/Xcode\n"
	want := []string{"/Applications/Xcode.app", "/Users/x/xcodes/Xcode 27.1.app"}
	if got := parseRunningXcodes(ps); !reflect.DeepEqual(got, want) {
		t.Fatalf("%q", got)
	}
}

// parseRunningXcodesOffsets is parseRunningXcodes as it was written with bare offsets (25 and 4: the lengths of
// ".app/Contents/MacOS/Xcode" and ".app"), kept to check that the strings.Cut version returns the same lists.
func parseRunningXcodesOffsets(ps string) []string {
	var apps []string
	seen := map[string]bool{}
	for _, l := range strings.Split(ps, "\n") {
		i := strings.Index(l, ".app/Contents/MacOS/Xcode")
		if i < 0 || (len(l) > i+25 && l[i+25] != ' ') {
			continue
		}
		app := strings.TrimSpace(l[:i+4])
		if !seen[app] {
			seen[app] = true
			apps = append(apps, app)
		}
	}
	return apps
}

func FuzzParseRunningXcodesMatchesOffsets(f *testing.F) {
	for _, seed := range []string{
		"",
		"\n\n",
		"/Applications/Xcode.app/Contents/MacOS/Xcode",
		"/Applications/Xcode.app/Contents/MacOS/Xcode ",
		"/Applications/Xcode.app/Contents/MacOS/XcodeHelper",
		"/Applications/Xcode.app/Contents/MacOS/Xcode\t-psn",
		"  /Applications/Xcode.app/Contents/MacOS/Xcode -ApplePersistenceIgnoreState YES",
		".app/Contents/MacOS/Xcode",
		"/A/Xcode.app/Contents/MacOS/XcodeX /B/Xcode.app/Contents/MacOS/Xcode",
		"/A/Xcode.app/Contents/MacOS/Xcode /B/Xcode.app/Contents/MacOS/Xcode\n/A/Xcode.app/Contents/MacOS/Xcode",
		"/Users/x/xcodes/Xcode 27.1.app/Contents/MacOS/Xcode\n/Applications/Xcode-beta.app/Contents/MacOS/Xcode",
	} {
		f.Add(seed)
	}
	f.Fuzz(func(t *testing.T, ps string) {
		if got, want := parseRunningXcodes(ps), parseRunningXcodesOffsets(ps); !reflect.DeepEqual(got, want) {
			t.Fatalf("parseRunningXcodes(%q) = %q, the offset version gives %q", ps, got, want)
		}
	})
}

// captureStdout runs f with os.Stdout redirected and returns what it printed.
func captureStdout(t *testing.T, f func() error) (string, error) {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	old := os.Stdout
	os.Stdout = w
	ch := make(chan string)
	go func() { b, _ := io.ReadAll(r); ch <- string(b) }()
	ferr := f()
	w.Close()
	os.Stdout = old
	return <-ch, ferr
}

// The container-tmp fallback: status, sentinel ls and clear reach <data>/tmp/simcam-state/ through a fake
// simctl (U booted: dev.x installed, dev.gone not; S shut down).
func TestSentinelFallback(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	old := bootEnv
	defer func() { bootEnv = old }()
	bootEnv = func(string) (map[string]string, error) { return map[string]string{}, nil }
	c := filepath.Join(t.TempDir(), "U", "data", "Containers", "Data", "Application", "A1")
	bin := t.TempDir()
	devs := `{"devices":{"com.apple.CoreSimulator.SimRuntime.iOS-17-5":[{"udid":"U","name":"a","state":"Booted","isAvailable":true},{"udid":"S","name":"b","state":"Shutdown","isAvailable":true}]}}`
	script := "#!/bin/sh\ncase \"$*\" in\n" +
		"'simctl list devices -j') echo '" + devs + "';;\n" +
		"'simctl get_app_container U dev.x data') echo '" + c + "';;\n" +
		"'simctl get_app_container U '*) echo 'An error was encountered processing the command (domain=NSPOSIXErrorDomain, code=2):' >&2; echo 'No such file or directory' >&2; exit 2;;\n" +
		"*) echo \"fake xcrun: $*\" >&2; exit 3;;\nesac\n"
	os.WriteFile(filepath.Join(bin, "xcrun"), []byte(script), 0o755)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	home.WriteCurrent("U", "/h/builds/B1/libsimcamloader.dylib")
	until := time.Now().Add(20 * time.Hour)
	mk := func(p, s string) {
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte(s), 0o644)
		os.Chtimes(p, until, until)
	}
	tmp := filepath.Join(c, "tmp", "simcam-state")
	plant := func() {
		mk(filepath.Join(tmp, "dev.x", "latched-B1"), "activate\n")
		mk(filepath.Join(tmp, "preflight-fail-B1"), "preflight failed\n")
		mk(filepath.Join(home.StateDir("U"), "dev.x", "latched-B1"), "session\n") // a normal latch too
	}
	plant()
	mk(filepath.Join(c, "tmp", "app-file"), "x")
	mk(filepath.Join(home.StateDir("U"), "dev.gone", "events.log"), "1 1 activate - B1 traced=0\n")

	s := statusWithFallback(simctl.Device{UDID: "U", State: "Booted"}, statusOf(simctl.Device{UDID: "U", State: "Booted"}))
	if !strings.Contains(s.Fallback["dev.x"], "marker latched-B1: activate") || !strings.Contains(s.Fallback["dev.x"], "preflight-fail-B1") ||
		s.FallbackDir["dev.x"] != filepath.Join(tmp, "dev.x") || s.FallbackUnchecked["dev.gone"] != "app not installed" ||
		s.LatchEffect["dev.x"] != "no camera and stock CoreMotion" || !strings.Contains(s.Latched["dev.x"], "session") {
		t.Fatalf("status: %+v", s)
	}
	out, err := captureStdout(t, func() error { return cmdStatus(nil) })
	if err != nil || !strings.Contains(out, "in the container-tmp fallback: dev.x (") || !strings.Contains(out, "container-tmp fallback not checked: dev.gone: app not installed") {
		t.Fatalf("status text: %v\n%s", err, out)
	}
	out, err = captureStdout(t, func() error { return cmdSentinel([]string{"ls", "--udid", "U"}) })
	if err != nil || !strings.Contains(out, "container-tmp fallback "+filepath.Join(tmp, "dev.x")+": no events.log") || !strings.Contains(out, "  marker latched-B1: activate until") ||
		!strings.Contains(out, "  marker preflight-fail-B1: preflight failed") {
		t.Fatalf("ls: %v\n%s", err, out)
	}

	// clear: both locations; the not-installed app is cleared in the state dir and said so; the app's own file stays
	out, err = captureStdout(t, func() error { return cmdSentinel([]string{"clear", "--udid", "U"}) })
	if err != nil || !strings.Contains(out, "dev.x: cleared (state dir); container-tmp fallback "+filepath.Join(tmp, "dev.x")+": removed latched-B1, removed preflight-fail-B1; its next launch gets the camera again") ||
		!strings.Contains(out, "dev.gone: cleared (state dir); its next launch gets the camera again") || !strings.Contains(out, "not checked: dev.gone: app not installed") {
		t.Fatalf("clear: %v\n%s", err, out)
	}
	for _, p := range []string{filepath.Join(tmp, "dev.x", "latched-B1"), filepath.Join(tmp, "preflight-fail-B1"), filepath.Join(home.StateDir("U"), "dev.x", "latched-B1")} {
		if _, err := os.Stat(p); err == nil {
			t.Errorf("kept %s", p)
		}
	}
	if _, err := os.Stat(filepath.Join(c, "tmp", "app-file")); err != nil {
		t.Fatal("the app's own file was touched")
	}
	if s := statusWithFallback(simctl.Device{UDID: "U", State: "Booted"}, statusOf(simctl.Device{UDID: "U", State: "Booted"})); len(s.Fallback) != 0 || len(s.Latched) != 0 {
		t.Fatalf("status after clear: %+v", s)
	}

	// state dir blocked (a file where the bundle's dir would be, as scripts/activation.sh does): the fallback is still cleared
	plant()
	os.RemoveAll(filepath.Join(home.StateDir("U"), "dev.x"))
	os.WriteFile(filepath.Join(home.StateDir("U"), "dev.x"), []byte("blocks\n"), 0o644)
	out, err = captureStdout(t, func() error { return cmdSentinel([]string{"clear", "--udid", "U", "--bundle", "dev.x"}) })
	if err != nil || !strings.Contains(out, "dev.x: state dir not cleared: ") || !strings.Contains(out, "removed latched-B1") || strings.Contains(out, "camera again") {
		t.Fatalf("blocked clear: %v\n%s", err, out)
	}
	if _, err := os.Stat(filepath.Join(tmp, "dev.x", "latched-B1")); err == nil {
		t.Fatal("fallback marker kept")
	}

	// a shut-down sim: the state dir is cleared, the fallback is reported unchecked, no error
	mk(filepath.Join(home.StateDir("S"), "dev.x", "latched-B1"), "session\n")
	out, err = captureStdout(t, func() error { return cmdSentinel([]string{"clear", "--udid", "S"}) })
	if err != nil || !strings.Contains(out, "dev.x: cleared (state dir)\n") || !strings.Contains(out, "container-tmp fallback not checked: simulator not booted (") {
		t.Fatalf("shut down: %v\n%s", err, out)
	}
	if s := statusWithFallback(simctl.Device{UDID: "S", State: "Shutdown"}, statusOf(simctl.Device{UDID: "S", State: "Shutdown"})); s.FallbackUnchecked["dev.x"] != "simulator not booted" {
		t.Fatalf("status shut down: %+v", s)
	}
	// nothing could be done (state dir blocked, sim shut down): an error
	os.RemoveAll(filepath.Join(home.StateDir("S"), "dev.x"))
	os.WriteFile(filepath.Join(home.StateDir("S"), "dev.x"), []byte("blocks\n"), 0o644)
	out, err = captureStdout(t, func() error { return cmdSentinel([]string{"clear", "--udid", "S", "--bundle", "dev.x"}) })
	if err == nil || !strings.Contains(err.Error(), "nothing could be cleared for dev.x") {
		t.Fatalf("nothing done: %v\n%s", err, out)
	}
}

func TestUncheckedText(t *testing.T) {
	if s := uncheckedText(map[string]string{"a": "simulator not booted", "b": "simulator not booted"}); s != "simulator not booted" {
		t.Fatal(s)
	}
	if s := uncheckedText(map[string]string{"b": "app not installed", "a": "app not installed", "c": "boom"}); s != "a, b: app not installed; c: boom" {
		t.Fatal(s)
	}
}

// a wedged "Booted" sim: get_app_container times out once, the sim's other bundles are skipped, and a clear exits 1
func TestFallbackTimeoutSkipsSim(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	oldT := fallbackTimeout
	t.Cleanup(func() { fallbackTimeout = oldT })
	fallbackTimeout = 300 * time.Millisecond
	bin := t.TempDir()
	devs := `{"devices":{"com.apple.CoreSimulator.SimRuntime.iOS-26-3":[{"udid":"T","name":"w","state":"Booted","isAvailable":true}]}}`
	script := "#!/bin/sh\ncase \"$*\" in\n" +
		"'simctl list devices -j') echo '" + devs + "';;\n" +
		"'simctl get_app_container T '*) exec sleep 5;;\n" +
		"*) exit 3;;\nesac\n"
	os.WriteFile(filepath.Join(bin, "xcrun"), []byte(script), 0o755)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	for _, b := range []string{"dev.a", "dev.b", "dev.c"} {
		os.MkdirAll(filepath.Join(home.StateDir("T"), b), 0o755)
	}
	d := simctl.Device{UDID: "T", State: "Booted"}
	start := time.Now()
	s := statusWithFallback(d, simStatus{})
	if len(s.FallbackUnchecked) != 3 || !strings.Contains(s.FallbackUnchecked["dev.c"], "timed out") || time.Since(start) > 3*time.Second {
		t.Fatalf("%v after %s", s.FallbackUnchecked, time.Since(start))
	}
	out, err := captureStdout(t, func() error { return cmdSentinel([]string{"clear", "--udid", "T"}) })
	if err == nil || !strings.Contains(err.Error(), "not fully cleared") {
		t.Fatalf("clear on a wedged sim: %v\n%s", err, out)
	}
}

// a pass after a fail its confirming launch didn't repeat says so, naming the fail
func TestRetriedLine(t *testing.T) {
	ff := verify.Result{State: verify.Pass, Verdict: "pass", FailedFirst: "RESULT core=FAIL core:[photo-then-stop: x]", Relaunched: "RESULT core=INCONCLUSIVE (y)"}
	if l := retriedLine(ff); !strings.Contains(l, "FAILED (RESULT core=FAIL core:[photo-then-stop: x])") || !strings.Contains(l, "cached for 1 h only") {
		t.Fatalf("pass after a fail: %q", l)
	}
	rl := verify.Result{State: verify.Pass, Verdict: "pass", Relaunched: "RESULT core=INCONCLUSIVE (y)"}
	if l := retriedLine(rl); !strings.Contains(l, "a second canary launch (the first: RESULT core=INCONCLUSIVE (y))") {
		t.Fatalf("relaunched pass: %q", l)
	}
	ff.NoCache = "SIMCTL_CHILD_SIMCAM_OFF_STOP_WAIT" // with the cache bypassed, the line doesn't claim an hour
	if l := retriedLine(ff); strings.Contains(l, "cached for") || !strings.Contains(l, "not cached (SIMCTL_CHILD_SIMCAM_OFF_STOP_WAIT)") {
		t.Fatalf("pass after a fail, cache bypassed: %q", l)
	}
	rt := verify.Result{State: verify.Pass, Verdict: "pass", Retried: "photo-jpeg: gap 103 ms -> retry ok"}
	if l := retriedLine(rt); !strings.Contains(l, "frame-gap check (photo-jpeg") {
		t.Fatalf("retried pass: %q", l)
	}
}

// trace's arguments are checked before any simctl call
func TestTraceUsage(t *testing.T) {
	for _, a := range [][]string{{}, {"a", "b"}, {"../x"}, {"a/b"}, {".."}, {"b", "--json"}, {"--pid", "x", "b"}} {
		err := cmdTrace(a)
		var u usageError
		if !errors.As(err, &u) {
			t.Errorf("%q: %v, want a usage error", a, err)
		}
	}
}
