// SPDX-License-Identifier: Apache-2.0
package webcam

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestCamHashVectors(t *testing.T) { // docs/webcam-ring.md §1
	if h := CamHash("", ""); h != "8601b7df" {
		t.Fatal(h)
	}
	if h := CamHash("/h", "cam"); h != "fc04bcb5" {
		t.Fatal(h)
	}
	if n := ShmName(4294967295, "fc04bcb5", 65535); len(n) != 31 {
		t.Fatal(n, len(n))
	}
}

func TestEngineRingsMatchEngine(t *testing.T) { // the engine's accepted set is this package's EngineRings
	b, err := os.ReadFile("../../../engine/src/providers/webcam.inc")
	if err != nil {
		t.Skip("no engine source next to the CLI")
	}
	want := "static const uint32_t kWcRings[] = {"
	for i, r := range EngineRings {
		if i > 0 {
			want += ", "
		}
		want += fmt.Sprint(r)
	}
	if !strings.Contains(string(b), want+"};") {
		t.Fatalf("engine/src/providers/webcam.inc doesn't declare %s}", want)
	}
}

const pidOK = "stuntcam-webcam 1\npid=4321\nsha=0123456789ab\nrings=1\nauth=prompting\nstate=prompting\ncamera=0x1420000005ac8600\nstarted=1794790000\n"

func TestParsePid(t *testing.T) {
	p, err := ParsePid([]byte(pidOK))
	if err != nil || p.Pid != 4321 || p.Sha != "0123456789ab" || len(p.Rings) != 1 || p.Auth != "prompting" || p.Camera != "0x1420000005ac8600" || p.Started != 1794790000 {
		t.Fatal(p, err)
	}
	if _, err := ParsePid([]byte(pidOK + "future=1\n")); err != nil {
		t.Fatal("unknown keys are ignored:", err)
	}
	bad := []string{
		strings.Replace(pidOK, "stuntcam-webcam 1", "stuntcam-webcam 2", 1),
		strings.TrimSuffix(pidOK, "\n"),
		pidOK + "pid=4322\n",
		strings.Replace(pidOK, "pid=4321", "pid=04321", 1),
		strings.Replace(pidOK, "pid=4321", "pid=0", 1),
		strings.Replace(pidOK, "sha=0123456789ab", "sha=0123456789AB", 1),
		strings.Replace(pidOK, "rings=1", "rings=1,0", 1),
		strings.Replace(pidOK, "rings=1", "rings=1,2,3,4,5,6,7,8,9", 1),
		strings.Replace(pidOK, "auth=prompting", "auth=maybe", 1),
		strings.Replace(pidOK, "state=prompting", "state=9", 1),
		strings.Replace(pidOK, "camera=0x1420000005ac8600", "camera=a\x01b", 1),
		strings.Replace(pidOK, "auth=prompting\n", "", 1),
		strings.Replace(pidOK, "started=1794790000", "started=-1", 1),
		pidOK + "garbage\n",
	}
	for _, b := range bad {
		if _, err := ParsePid([]byte(b)); err == nil {
			t.Errorf("accepted %q", b)
		}
	}
}

func TestSharedRing(t *testing.T) {
	if SharedRing([]int{1, 2}, []int{1}) != 1 || SharedRing([]int{2}, []int{1}) != 0 || SharedRing([]int{1, 2}, []int{2, 1}) != 2 {
		t.Fatal()
	}
}

const list = "stuntcam-cameras 1\n" +
	"BUILTIN-1\t1\t0\tAVCaptureDeviceTypeBuiltInWideAngleCamera\tMacBook Pro Camera\n" +
	"0x21000003564fefe\t0\t0\tAVCaptureDeviceTypeExternal\tOBSBOT Meet SE StreamCamera\n" +
	"PHONE-1\t0\t1\tAVCaptureDeviceTypeContinuityCamera\tPhone Camera\n" +
	"DESK-2\t0\t0\tAVCaptureDeviceTypeDeskViewCamera\tMacBook Pro Desk View Camera\n"

func TestResolve(t *testing.T) {
	cs, err := ParseList([]byte(list))
	if err != nil || len(cs) != 4 {
		t.Fatal(cs, err)
	}
	for name, want := range map[string]string{
		"":                            "BUILTIN-1",
		"BUILTIN-1":                   "BUILTIN-1",
		"0x21000003564fefe":           "0x21000003564fefe",
		"obsbot meet se streamcamera": "0x21000003564fefe", // name, case-insensitive
		"OBSBOT":                      "0x21000003564fefe", // the one name containing it
		"phone camera":                "PHONE-1",           // a Continuity Camera only when named
		"desk view":                   "DESK-2",
		"synthetic":                   SyntheticID,
	} {
		c, err := Resolve(cs, name)
		if err != nil || c.ID != want {
			t.Errorf("%q: %v %v", name, c, err)
		}
	}
	if _, err := Resolve(cs, "camera"); err == nil || !strings.Contains(err.Error(), "matches 4 cameras") {
		t.Error(err)
	}
	if _, err := Resolve(cs, "nope"); err == nil || !strings.Contains(err.Error(), "BUILTIN-1") {
		t.Error("the error lists the cameras:", err)
	}
	if _, err := Resolve(cs[2:], ""); err == nil {
		t.Error("no default among a Continuity Camera and Desk View")
	}
	if _, err := ParseList([]byte("stuntcam-cameras 1\nX\t2\t0\tT\tN\n")); err == nil {
		t.Error()
	}
}

func TestIsHelperPath(t *testing.T) {
	h := t.TempDir()
	ok := filepath.Join(h, "helper", "0123456789ab", AppName, ExeRel)
	if !IsHelperPath(h, ok) {
		t.Fatal(ok)
	}
	for _, p := range []string{"", "/bin/sh", filepath.Join(h, "helper", "xyz", AppName, ExeRel), filepath.Join(h, "helper", "0123456789ab", "Other.app", ExeRel),
		filepath.Join(t.TempDir(), "helper", "0123456789ab", AppName, ExeRel)} {
		if IsHelperPath(h, p) {
			t.Error(p)
		}
	}
}

// fake is a Mac with a helper the test scripts: Open "starts" it (writes its pid file), Term "stops" it.
type fake struct {
	t       *testing.T
	e       *Env
	app     string
	now     time.Time
	alive   map[int]bool
	nextPid int
	opens   [][]string
	terms   []int
	out     bytes.Buffer
	// what the next started helper does
	rings      string
	auth       []string // auth values over successive polls (the last repeats)
	noStart    bool
	notHelper  map[int]bool
	shmOwner   int
	shmErr     error
	sticky     bool // ignores SIGTERM
	authPolls  int
	authOfPid  map[int][]string
	listOutput string
}

func newFake(t *testing.T) *fake {
	f := &fake{t: t, now: time.Unix(1794790000, 0), alive: map[int]bool{}, nextPid: 100, rings: "1", auth: []string{"authorized"},
		notHelper: map[int]bool{}, shmOwner: -1, shmErr: syscall.ENOENT, authOfPid: map[int][]string{}}
	home := t.TempDir()
	f.app = filepath.Join(home, "helper", "aaaaaaaaaaaa", AppName)
	os.MkdirAll(filepath.Join(f.app, "Contents", "MacOS"), 0o755)
	os.WriteFile(filepath.Join(f.app, ExeRel), []byte("x"), 0o755)
	f.e = &Env{
		Home: home, Rings: []int{1}, Uid: 501,
		Open:      f.open,
		Verify:    func(string) error { return nil },
		Alive:     func(pid int) bool { return f.alive[pid] },
		Term:      f.term,
		IsHelper:  func(pid int) bool { return !f.notHelper[pid] },
		ShmOwner:  func(string) (int, error) { return f.shmOwner, f.shmErr },
		Now:       func() time.Time { return f.now },
		Sleep:     func(d time.Duration) { f.now = f.now.Add(d); f.tick() },
		Out:       &f.out,
		StartWait: 5 * time.Second, PermWait: 60 * time.Second, StopWait: 5 * time.Second, Poll: 250 * time.Millisecond,
	}
	return f
}

func (f *fake) writePid(pid int, cam, camera, auth string) {
	state := map[string]string{"authorized": "idle", "prompting": "prompting", "denied": "denied", "notDetermined": "starting"}[auth]
	s := fmt.Sprintf("stuntcam-webcam 1\npid=%d\nsha=aaaaaaaaaaaa\nrings=%s\nauth=%s\nstate=%s\ncamera=%s\nstarted=%d\n", pid, f.rings, auth, state, camera, f.now.Unix())
	os.MkdirAll(Dir(f.e.Home), 0o700)
	os.WriteFile(PidPath(f.e.Home, cam), []byte(s), 0o600)
}

func (f *fake) open(app string, args []string) error {
	f.opens = append(f.opens, append([]string{app}, args...))
	if len(args) >= 4 && args[2] == "--list" {
		os.WriteFile(args[3], []byte(f.listOutput), 0o600)
		return nil
	}
	if f.noStart {
		return nil
	}
	camera := SyntheticID
	for i, a := range args {
		if a == "--camera" {
			camera = args[i+1]
		}
	}
	pid := f.nextPid
	f.nextPid++
	f.alive[pid] = true
	f.authOfPid[pid] = append([]string{}, f.auth...)
	f.writePid(pid, CamHash(f.e.Home, camera), camera, f.authOfPid[pid][0])
	return nil
}

// tick advances each live helper's auth script by one poll; a denied helper exits 3 s later (the pid file stays).
func (f *fake) tick() {
	for _, cam := range PidFiles(f.e.Home) {
		p, live, err := f.e.ReadPid(cam)
		if err != nil || !live {
			continue
		}
		a := f.authOfPid[p.Pid]
		if len(a) > 1 {
			f.authOfPid[p.Pid] = a[1:]
			f.writePid(p.Pid, cam, p.Camera, a[1])
		}
		if p.Auth == "denied" {
			f.alive[p.Pid] = false
		}
	}
}

func (f *fake) term(pid int) error {
	f.terms = append(f.terms, pid)
	if f.sticky {
		return nil
	}
	f.alive[pid] = false
	for _, cam := range PidFiles(f.e.Home) { // a helper removes its pid file as it exits
		if p, _, err := f.e.ReadPid(cam); err == nil && p.Pid == pid {
			os.Remove(PidPath(f.e.Home, cam))
		}
	}
	return nil
}

var cam1 = Camera{ID: "BUILTIN-1", Name: "MacBook Pro Camera"}

func TestEnsureStartsWithOpen(t *testing.T) {
	f := newFake(t)
	p, err := f.e.Ensure(f.app, cam1)
	if err != nil || p.Pid != 100 || p.Auth != "authorized" {
		t.Fatal(p, err, f.out.String())
	}
	want := []string{f.app, "--home", f.e.Home, "--camera", "BUILTIN-1"}
	if len(f.opens) != 1 || strings.Join(f.opens[0], "|") != strings.Join(want, "|") {
		t.Fatal(f.opens)
	}
	// a second source on another sim reuses it: no second open
	if _, err := f.e.Ensure(f.app, cam1); err != nil || len(f.opens) != 1 || !strings.Contains(f.out.String(), "already running") {
		t.Fatal(err, f.opens, f.out.String())
	}
}

func TestEnsureSyntheticNoPermissionWait(t *testing.T) {
	f := newFake(t)
	f.auth = []string{"notDetermined"}
	p, err := f.e.Ensure(f.app, Camera{ID: SyntheticID})
	if err != nil || p.Camera != SyntheticID {
		t.Fatal(p, err)
	}
	if a := f.opens[0]; a[len(a)-1] != "--synthetic" {
		t.Fatal(a)
	}
}

func TestEnsurePromptThenAllowed(t *testing.T) {
	f := newFake(t)
	f.auth = []string{"notDetermined", "prompting", "prompting", "prompting", "authorized"}
	t0 := f.now
	p, err := f.e.Ensure(f.app, cam1)
	if err != nil || p.Auth != "authorized" {
		t.Fatal(p, err)
	}
	o := f.out.String()
	if strings.Count(o, "allow camera access for Stuntcam Camera in the dialog") != 1 || !strings.Contains(o, "camera access allowed") {
		t.Fatal(o)
	}
	if f.now.Sub(t0) > 2*time.Second {
		t.Fatal("waited too long", f.now.Sub(t0))
	}
}

func TestEnsurePromptTimesOut(t *testing.T) {
	f := newFake(t)
	f.auth = []string{"prompting"}
	t0 := f.now
	_, err := f.e.Ensure(f.app, cam1)
	if !errors.Is(err, ErrNoAnswer) {
		t.Fatal(err)
	}
	if d := f.now.Sub(t0); d < 60*time.Second || d > 61*time.Second {
		t.Fatal("the wait is bounded at 60 s:", d)
	}
	if len(f.terms) != 0 {
		t.Fatal("the helper is left to its prompt")
	}
}

func TestEnsureDenied(t *testing.T) {
	f := newFake(t)
	f.auth = []string{"prompting", "denied"}
	_, err := f.e.Ensure(f.app, cam1)
	if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "System Settings › Privacy & Security › Camera › Stuntcam Camera") {
		t.Fatal(err)
	}
	// right away: the denied helper is still exiting; the next source waits for it, then starts a new one
	f.alive[100] = true
	f.auth = []string{"denied"}
	exitAt := f.now.Add(2 * time.Second)
	f.e.Alive = func(pid int) bool { return f.alive[pid] && !(pid == 100 && !f.now.Before(exitAt)) }
	if _, err := f.e.Ensure(f.app, cam1); !errors.Is(err, ErrDenied) || len(f.opens) != 2 {
		t.Fatal(err, f.opens)
	}
}

func TestEnsureReplacesIncompatibleHelper(t *testing.T) {
	f := newFake(t)
	f.rings = "2"
	if _, err := f.e.Ensure(f.app, cam1); err == nil || !strings.Contains(err.Error(), "update stuntcam") {
		t.Fatal("a helper serving only v2 is reported:", err)
	}
	// the v2 helper is running; this stuntcam's helper serves v1: it is stopped and replaced
	f.rings = "1"
	p, err := f.e.Ensure(f.app, cam1)
	if err != nil || p.Pid != 101 || len(f.terms) != 1 || f.terms[0] != 100 || !strings.Contains(f.out.String(), "serves ring v2, this stuntcam's engine reads v1: replacing it") {
		t.Fatal(p, err, f.terms, f.out.String())
	}
}

func TestEnsureKeepsHelperServingTwoVersions(t *testing.T) {
	f := newFake(t)
	f.rings = "1,2"
	if _, err := f.e.Ensure(f.app, cam1); err != nil {
		t.Fatal(err)
	}
	if _, err := f.e.Ensure(f.app, cam1); err != nil || len(f.terms) != 0 || len(f.opens) != 1 {
		t.Fatal(err, f.terms)
	}
}

func TestEnsureNoStartOwnedByOtherUser(t *testing.T) {
	f := newFake(t)
	f.noStart = true
	f.shmOwner, f.shmErr = 502, nil
	t0 := f.now
	_, err := f.e.Ensure(f.app, cam1)
	if err == nil || !strings.Contains(err.Error(), "owned by uid 502") || !strings.Contains(err.Error(), "/scw.501.") {
		t.Fatal(err)
	}
	if d := f.now.Sub(t0); d < 5*time.Second || d > 6*time.Second {
		t.Fatal("waits 5 s for the pid file:", d)
	}
	f.shmOwner, f.shmErr = -1, syscall.EACCES
	if _, err := f.e.Ensure(f.app, cam1); err == nil || !strings.Contains(err.Error(), "owned by another user") {
		t.Fatal(err)
	}
	f.shmErr = syscall.ENOENT
	os.MkdirAll(filepath.Join(f.e.Home, "logs"), 0o700)
	os.WriteFile(LogPath(f.e.Home, CamHash(f.e.Home, cam1.ID)), []byte("1.0 [helper 9] start\n1.1 [helper 9] another helper holds x.lock (Resource busy); exiting\n"), 0o600)
	if _, err := f.e.Ensure(f.app, cam1); err == nil || !strings.Contains(err.Error(), "another helper holds") {
		t.Fatal(err)
	}
}

func TestEnsureRefusesBadApp(t *testing.T) {
	f := newFake(t)
	if _, err := f.e.Ensure("", cam1); err == nil {
		t.Fatal()
	}
	if _, err := f.e.Ensure("relative/Stuntcam Camera.app", cam1); err == nil {
		t.Fatal()
	}
	f.e.Verify = func(string) error { return errors.New("invalid signature") }
	if _, err := f.e.Ensure(f.app, cam1); err == nil || !strings.Contains(err.Error(), "invalid signature") || len(f.opens) != 0 {
		t.Fatal(err, f.opens)
	}
	// a built helper outside $SIMCAM_HOME/helper/<sha>/ (another SIMCAM_HOME, $STUNTCAM_HELPER) would never count as
	// live: refused before it is launched
	other := filepath.Join(t.TempDir(), "helper", "bbbbbbbbbbbb", AppName)
	os.MkdirAll(filepath.Join(other, "Contents", "MacOS"), 0o755)
	os.WriteFile(filepath.Join(other, ExeRel), []byte("x"), 0o755)
	f.e.Verify = func(string) error { return nil }
	if _, err := f.e.Ensure(other, cam1); err == nil || !strings.Contains(err.Error(), "SIMCAM_HOME") || len(f.opens) != 0 {
		t.Fatal(err, f.opens)
	}
}

func TestList(t *testing.T) {
	f := newFake(t)
	f.listOutput = list
	cs, err := f.e.List(f.app)
	if err != nil || len(cs) != 4 || f.opens[0][3] != "--list" {
		t.Fatal(cs, err, f.opens)
	}
	if _, err := os.Stat(f.opens[0][4]); !os.IsNotExist(err) {
		t.Fatal("the list file is removed")
	}
	f.listOutput = ""
	f.e.Open = func(string, []string) error { return nil }
	if _, err := f.e.List(f.app); err == nil {
		t.Fatal("no list in 5 s")
	}
}

func TestStop(t *testing.T) {
	f := newFake(t)
	cam := CamHash(f.e.Home, cam1.ID)
	if err := f.e.Stop(cam); err != nil {
		t.Fatal("no pid file: nothing to stop", err)
	}
	f.e.Ensure(f.app, cam1)
	f.notHelper[100] = true // the helper crashed and pid 100 now runs something else: nothing to stop
	if err := f.e.Stop(cam); err != nil || len(f.terms) != 0 {
		t.Fatal("a pid that isn't a helper is never signalled", err)
	}
	f.notHelper[100] = false
	f.sticky = true
	if err := f.e.Stop(cam); err == nil || !strings.Contains(err.Error(), "still running") {
		t.Fatal(err)
	}
	f.sticky = false
	if err := f.e.Stop(cam); err != nil || f.alive[100] {
		t.Fatal(err)
	}
}

func TestSessions(t *testing.T) {
	f := newFake(t)
	inUse := map[string]bool{"A": true, "B": true}
	use := func(u string) bool { return inUse[u] }
	f.e.Ensure(f.app, cam1)
	if err := f.e.Switch("A", []string{cam1.ID}, use); err != nil {
		t.Fatal(err)
	}
	if err := f.e.Switch("B", []string{cam1.ID}, use); err != nil {
		t.Fatal(err)
	}
	// A leaves: B still uses the camera, the helper stays
	if err := f.e.Switch("A", nil, use); err != nil || len(f.terms) != 0 || ReadSession(f.e.Home, "A") != nil {
		t.Fatal(err, f.terms)
	}
	if !strings.Contains(f.out.String(), "1 other simulator(s) still use it (B)") {
		t.Fatal(f.out.String())
	}
	// B switches to another camera: the last user of cam1 left, its helper stops
	if err := f.e.Switch("B", []string{SyntheticID}, use); err != nil || len(f.terms) != 1 || f.terms[0] != 100 {
		t.Fatal(err, f.terms)
	}
	if s := ReadSession(f.e.Home, "B"); len(s) != 1 || s[0] != SyntheticID {
		t.Fatal(s)
	}
	// a stale session (a sim that no longer has webcam selected) doesn't keep a helper running
	f.e.Ensure(f.app, cam1)
	f.e.Switch("A", []string{cam1.ID}, use)
	f.e.Switch("C", []string{cam1.ID}, use) // C isn't in use
	if err := f.e.Switch("A", nil, use); err != nil || len(f.terms) != 2 {
		t.Fatal(err, f.terms)
	}
}

func TestGC(t *testing.T) {
	f := newFake(t)
	f.e.Ensure(f.app, cam1) // live helper aaaaaaaaaaaa, pid 100
	old := f.now.Add(-48 * time.Hour)
	for _, s := range []string{"bbbbbbbbbbbb", "cccccccccccc", "dddddddddddd"} {
		d := filepath.Join(HelperRoot(f.e.Home), s)
		os.MkdirAll(d, 0o755)
		os.Chtimes(d, old, old)
	}
	os.Chtimes(filepath.Dir(f.app), old, old)
	os.Chtimes(filepath.Join(HelperRoot(f.e.Home), "dddddddddddd"), f.now, f.now) // touched today
	// a dead helper's pid file and ring, and one whose name a new helper already holds
	dead := CamHash(f.e.Home, "DEAD")
	os.WriteFile(PidPath(f.e.Home, dead), []byte(strings.Replace(pidOK, "pid=4321", "pid=77", 1)), 0o600)
	os.Chtimes(PidPath(f.e.Home, dead), old, old)
	os.WriteFile(filepath.Join(Dir(f.e.Home), dead+".lock"), nil, 0o600)
	var unlinked []string
	producer := func(name string) int {
		if strings.Contains(name, dead) {
			return 77
		}
		return 100
	}
	var removed []string
	f.e.GC(map[string]bool{"cccccccccccc": true}, false, func(n string) error { unlinked = append(unlinked, n); return nil }, producer,
		func(what, p string) { removed = append(removed, filepath.Base(p)); os.RemoveAll(p) })
	if strings.Join(removed, ",") != "bbbbbbbbbbbb" {
		t.Fatal("removes only an old, unkept, not-live sha:", removed)
	}
	if len(unlinked) != 1 || unlinked[0] != ShmName(501, dead, 1) {
		t.Fatal(unlinked)
	}
	if _, err := os.Stat(PidPath(f.e.Home, dead)); !os.IsNotExist(err) {
		t.Fatal("dead pid file stays")
	}
	if _, err := os.Stat(filepath.Join(Dir(f.e.Home), dead+".lock")); err != nil {
		t.Fatal("a .lock file is never removed")
	}
	if _, err := os.Stat(PidPath(f.e.Home, CamHash(f.e.Home, cam1.ID))); err != nil {
		t.Fatal("a live helper's pid file stays")
	}
	// the ring of a dead helper whose name now holds a newer helper's object is left alone
	os.WriteFile(PidPath(f.e.Home, dead), []byte(strings.Replace(pidOK, "pid=4321", "pid=78", 1)), 0o600)
	os.Chtimes(PidPath(f.e.Home, dead), old, old)
	unlinked = nil
	f.e.GC(nil, false, func(n string) error { unlinked = append(unlinked, n); return nil }, producer, func(string, string) {})
	if len(unlinked) != 0 {
		t.Fatal(unlinked)
	}
}

// After a crash the pid file stays and its pid can be reused: a live pid that isn't a helper is not "already running".
func TestEnsureReusedPid(t *testing.T) {
	f := newFake(t)
	cam := CamHash(f.e.Home, cam1.ID)
	f.e.Ensure(f.app, cam1) // pid 100, authorized
	f.notHelper[100] = true // it crashed; pid 100 is now another process
	if _, live, err := f.e.ReadPid(cam); err != nil || live {
		t.Fatal("a reused pid isn't a live helper", live, err)
	}
	if live := f.e.LiveShas(); len(live) != 0 {
		t.Fatal("a reused pid names no live sha", live)
	}
	f.auth = []string{"prompting", "authorized"}
	f.out.Reset()
	p, err := f.e.Ensure(f.app, cam1)
	if err != nil || p.Pid != 101 || len(f.opens) != 2 || strings.Contains(f.out.String(), "already running") {
		t.Fatal("a new helper is started", p, err, f.opens, f.out.String())
	}
	if len(f.terms) != 0 {
		t.Fatal("the process at the reused pid is never signalled", f.terms)
	}
	// gc removes the crashed helper's pid file even though its pid is alive (as something else)
	f.e.Stop(cam)
	f.writePid(100, cam, cam1.ID, "authorized")
	old := f.now.Add(-time.Hour)
	os.Chtimes(PidPath(f.e.Home, cam), old, old)
	f.e.GC(nil, false, func(string) error { return nil }, func(string) int { return 0 }, func(string, string) {})
	if _, err := os.Stat(PidPath(f.e.Home, cam)); !os.IsNotExist(err) {
		t.Fatal("the crashed helper's pid file stays", err)
	}
}

// GC races a helper that is starting: a fresh pid file is left, and one rewritten between the read and the remove
// is put back.
func TestGCRace(t *testing.T) {
	f := newFake(t)
	cam := CamHash(f.e.Home, "DEAD")
	pp := PidPath(f.e.Home, cam)
	os.MkdirAll(Dir(f.e.Home), 0o700)
	dead := []byte(strings.Replace(pidOK, "pid=4321", "pid=77", 1))
	os.WriteFile(pp, dead, 0o600)
	os.Chtimes(pp, f.now.Add(-5*time.Second), f.now.Add(-5*time.Second))
	gc := func() {
		f.e.GC(nil, false, func(string) error { return nil }, func(string) int { return 0 }, func(string, string) {})
	}
	gc()
	if _, err := os.Stat(pp); err != nil {
		t.Fatal("a pid file written in the last 10 s stays", err)
	}
	old := f.now.Add(-time.Hour)
	os.Chtimes(pp, old, old)
	// a starting helper replaces the file after GC read it: what GC set aside isn't what it read, so it goes back
	fresh := []byte(strings.Replace(pidOK, "pid=4321", "pid=88", 1))
	os.WriteFile(pp, fresh, 0o600)
	os.Chtimes(pp, old, old)
	if f.e.removeIfUnchanged(pp, dead) {
		t.Fatal("removed a rewritten pid file")
	}
	if b, err := os.ReadFile(pp); err != nil || string(b) != string(fresh) {
		t.Fatal("the rewritten pid file is put back", string(b), err)
	}
	es, _ := os.ReadDir(Dir(f.e.Home))
	for _, e := range es {
		if strings.Contains(e.Name(), asideMark) {
			t.Fatal("nothing is left aside", e.Name())
		}
	}
	gc()
	if _, err := os.Stat(pp); !os.IsNotExist(err) {
		t.Fatal("an old dead pid file goes", err)
	}
}

// A helper that answers `--list` after List gave up leaves its file: GC removes it once it's a minute old.
func TestGCStaleList(t *testing.T) {
	f := newFake(t)
	os.MkdirAll(Dir(f.e.Home), 0o700)
	stale, fresh := filepath.Join(Dir(f.e.Home), "cameras-1-1"), filepath.Join(Dir(f.e.Home), "cameras-2-2")
	aside := PidPath(f.e.Home, CamHash(f.e.Home, "X")) + asideMark + "9"
	for _, p := range []string{stale, fresh, aside} {
		os.WriteFile(p, []byte("x"), 0o600)
	}
	old := f.now.Add(-2 * time.Minute)
	os.Chtimes(stale, old, old)
	os.Chtimes(aside, old, old)
	os.Chtimes(fresh, f.now, f.now)
	f.e.GC(nil, true, nil, nil, nil)
	if _, err := os.Stat(stale); err != nil {
		t.Fatal("dry run removes nothing")
	}
	f.e.GC(nil, false, nil, nil, nil)
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Fatal("a stale list answer goes")
	}
	if _, err := os.Stat(aside); !os.IsNotExist(err) {
		t.Fatal("a set-aside pid file goes")
	}
	if _, err := os.Stat(fresh); err != nil {
		t.Fatal("a list answer in flight stays")
	}
}
