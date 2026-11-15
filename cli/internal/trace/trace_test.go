// SPDX-License-Identifier: Apache-2.0
package trace

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"
)

// testdata/canary-263.txt: a real trace (the canary on iOS 26.3): activate, session-start, three photos (3, 4, 5:
// JPEG, 4:3 JPEG, HEVC) and 6 (photo-jpeg's gap-only retry, in a session of its own), the canary's later sessions
func load(t *testing.T) File {
	t.Helper()
	b, err := os.ReadFile("testdata/canary-263.txt")
	if err != nil {
		t.Fatal(err)
	}
	f, err := Parse(b)
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func TestParseReal(t *testing.T) {
	f := load(t)
	if f.Pid != 61256 || f.Bundle != "dev.simcam.spike.m10canary" || f.Build != "b818f3a60d01" || f.Numer != 125 || f.Denom != 3 || f.Recorded != 55 || len(f.Ops) != 55 {
		t.Fatalf("header %+v (%d ops)", f, len(f.Ops))
	}
	if o := f.Ops[3]; o.Name != "slot12" || o.SettingsID != 3 || o.Index != 3 {
		t.Fatalf("op 3 = %+v", o)
	}
	if f.Ops[0].Name != "activate" || f.Ops[0].SettingsID != -1 {
		t.Fatalf("op 0 = %+v", f.Ops[0])
	}
}

func TestParseErrors(t *testing.T) {
	good := "stuntcam-trace 1\npid 7\nbundle b\nbuild x\ntimebase 1 1\nwritten 10 20\nrecorded 1\nop 0 10 20 activate -1 0\nend\n"
	if _, err := Parse([]byte(good)); err != nil {
		t.Fatal(err)
	}
	cases := map[string]error{
		"":                                  ErrNotTrace,
		"hello\n":                           ErrNotTrace,
		strings.TrimSuffix(good, "end\n"):   ErrIncomplete, // a reader never sees this (temp + rename), but a copy cut short might
		"stuntcam-trace 1\npid 7\n":         ErrIncomplete,
		"stuntcam-trace 2\npid 7\nend\n":    ErrNotTrace, // another format version
		"stuntcam-trace 1\nop 1 2 3\nend\n": nil,         // bad op line: an error, checked below
	}
	for in, want := range cases {
		_, err := Parse([]byte(in))
		if want == nil {
			if err == nil {
				t.Errorf("%q: no error", in)
			}
			continue
		}
		if !errors.Is(err, want) {
			t.Errorf("%q: %v, want %v", in, err, want)
		}
	}
}

func TestHostNs(t *testing.T) {
	f := File{Numer: 125, Denom: 3}
	// 11703736252544 ticks * 125/3 = 487655677189333.33 ns: integer math, no float rounding
	if got := f.HostNs(11703736252544); got != 487655677189333 {
		t.Fatalf("HostNs = %d", got)
	}
	if (File{}).HostNs(42) != 42 {
		t.Fatal("zero timebase is 1/1")
	}
}

func TestBuildPhotos(t *testing.T) {
	f := load(t)
	r := Build(f, nil, "U", "sim", nil)
	if len(r.Photos) != 4 {
		t.Fatalf("photos %d", len(r.Photos))
	}
	for _, p := range r.Photos {
		if !reflect.DeepEqual(p.Stages, PhotoStages) || !p.InOrder || !p.Monotonic || !p.Complete || p.Error != 0 {
			t.Errorf("photo %d: %+v", p.SettingsID, p)
		}
		if p.TimestampMinusSlot12Ms == nil || *p.TimestampMinusSlot12Ms < 0 || *p.TimestampMinusSlot12Ms > 50 {
			t.Errorf("photo %d: timestamp - slot12 %v", p.SettingsID, p.TimestampMinusSlot12Ms)
		}
		// the timestamp op's arg is the photo's timestamp, and its mach time is the same instant
		if p.TimestampS == nil || int64(*p.TimestampS*1e9+0.5)/1000 != p.HostNs["timestamp"]/1000 {
			t.Errorf("photo %d: timestamp %v vs host %d", p.SettingsID, p.TimestampS, p.HostNs["timestamp"])
		}
		if p.Ms["shutter"] != 0 || p.Ms["still-complete"] <= p.Ms["slot12"] {
			t.Errorf("photo %d: ms %v", p.SettingsID, p.Ms)
		}
	}
	// delta_ms: set for photo ops (from that photo's shutter), null for the rest
	for _, o := range r.Ops {
		photo := stageIndex(o.Op) >= 0
		if photo != (o.DeltaMs != nil) {
			t.Errorf("op %d %s: delta %v", o.Index, o.Op, o.DeltaMs)
		}
		if o.Op == "shutter" && *o.DeltaMs != 0 {
			t.Errorf("shutter delta %v", *o.DeltaMs)
		}
		if (o.SettingsID == nil) != (o.Op == "activate" || strings.HasPrefix(o.Op, "session-")) {
			t.Errorf("op %d %s: settings %v", o.Index, o.Op, o.SettingsID)
		}
	}
}

func TestBuildOrderAndGaps(t *testing.T) {
	mk := func(names ...string) File {
		f := File{Numer: 1, Denom: 1, Pid: 1}
		for i, n := range names {
			f.Ops = append(f.Ops, Op{Index: uint64(i), WallNs: int64(i), Mach: uint64(1000000 + 1000*i), Name: n, SettingsID: 9})
		}
		return f
	}
	// the old engine (before L68): the timestamp after the encode
	old := Build(mk("shutter", "slot12", "will-capture", "did-capture", "encode-start", "encoded", "timestamp", "still-complete", "did-finish"), nil, "", "", nil)
	if p := old.Photos[0]; p.InOrder || !p.Complete {
		t.Errorf("old order: %+v", p)
	}
	// overwritten by the ring: no shutter -> incomplete, deltas from slot12
	cut := Build(mk("slot12", "will-capture", "timestamp", "did-capture", "encode-start", "encoded", "still-complete"), nil, "", "", nil)
	if p := cut.Photos[0]; p.Complete || !p.InOrder || p.Ms["slot12"] != 0 || p.Ms["still-complete"] != 0.006 {
		t.Errorf("cut: %+v", p)
	}
	// an error completion (latch): the engine still records the grab's timestamp op; the report shows none
	f := mk("shutter", "slot12", "will-capture", "timestamp", "did-capture", "encode-start", "still-complete", "did-finish")
	f.Ops[6].Arg = -16800
	if p := Build(f, nil, "", "", nil).Photos[0]; !p.Complete || p.Error != -16800 || p.TimestampMinusSlot12Ms != nil || p.TimestampS != nil {
		t.Errorf("error photo: %+v", p)
	}
	// time going back
	g := mk("shutter", "slot12", "will-capture")
	g.Ops[2].Mach = 500
	if p := Build(g, nil, "", "", nil).Photos[0]; p.Monotonic {
		t.Errorf("monotonic: %+v", p)
	}
	// a repeated stage
	if p := Build(mk("shutter", "slot12", "slot12"), nil, "", "", nil).Photos[0]; p.InOrder {
		t.Errorf("repeat: %+v", p)
	}
}

// the JSON field names are the documented schema (cli/README.md): a rename must fail here
func TestJSONSchema(t *testing.T) {
	yes := true
	r := Build(load(t), []File{{Pid: 5, Path: "/x/trace-5.txt", Location: "container-tmp", WrittenNs: 1}}, "U", "sim", &yes)
	b, err := json.Marshal(r)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatal(err)
	}
	keys := func(m map[string]any) []string {
		var k []string
		for x := range m {
			k = append(k, x)
		}
		return sortStrings(k)
	}
	if got, want := keys(m), sortStrings([]string{"schema", "udid", "simulator", "bundle", "pid", "running", "build", "file", "location", "written", "written_ns", "recorded", "ring_size", "timebase", "ops", "photos", "others"}); !reflect.DeepEqual(got, want) {
		t.Errorf("top keys %v", got)
	}
	op := m["ops"].([]any)[2].(map[string]any)
	if got, want := keys(op), sortStrings([]string{"index", "op", "wall", "wall_ns", "mach", "host_ns", "settings_id", "arg", "delta_ms"}); !reflect.DeepEqual(got, want) {
		t.Errorf("op keys %v", got)
	}
	ph := m["photos"].([]any)[0].(map[string]any)
	if got, want := keys(ph), sortStrings([]string{"settings_id", "stages", "in_order", "monotonic", "complete", "error", "host_ns", "ms", "timestamp_minus_slot12_ms", "timestamp_s"}); !reflect.DeepEqual(got, want) {
		t.Errorf("photo keys %v", got)
	}
	if m["schema"].(float64) != 1 || m["running"] != true || m["ring_size"].(float64) != 64 {
		t.Errorf("values %v %v %v", m["schema"], m["running"], m["ring_size"])
	}
	if a := m["ops"].([]any)[0].(map[string]any); a["settings_id"] != nil || a["delta_ms"] != nil {
		t.Errorf("activate: %v", a)
	}
	if o := m["others"].([]any)[0].(map[string]any); o["pid"].(float64) != 5 || o["location"] != "container-tmp" {
		t.Errorf("others %v", o)
	}
	// nothing to show still gives arrays, not null
	b, _ = json.Marshal(Build(File{Numer: 1, Denom: 1}, nil, "", "", nil))
	if !strings.Contains(string(b), `"ops":[]`) || !strings.Contains(string(b), `"photos":[]`) || !strings.Contains(string(b), `"others":[]`) || !strings.Contains(string(b), `"running":null`) {
		t.Errorf("empty: %s", b)
	}
}

func sortStrings(s []string) []string {
	for i := 1; i < len(s); i++ {
		for j := i; j > 0 && s[j] < s[j-1]; j-- {
			s[j], s[j-1] = s[j-1], s[j]
		}
	}
	return s
}

func TestHuman(t *testing.T) {
	f := load(t)
	f.Path, f.Location = "/s/trace-61256.txt", "state-dir"
	no := false
	r := Build(f, []File{{Pid: 99, WrittenNs: f.WrittenNs - 1e9}}, "UDID-1", "stuntcam-e48-263", &no)
	var b bytes.Buffer
	Human(&b, r, time.Unix(0, f.WrittenNs).Add(2*time.Second), time.UTC)
	out := b.String()
	for _, want := range []string{
		"dev.simcam.spike.m10canary pid 61256 (not running) on stuntcam-e48-263 (UDID-1), build b818f3a60d01",
		"/s/trace-61256.txt (state dir), written 2026-11-14 11:53:13.946 (2.0 s ago); 55 ops recorded, the last 55 shown",
		"    2  shutter         11:53:01.084      0.000         3",
		"    3  slot12          11:53:01.084      0.265         3",
		"    5  timestamp       11:53:01.085      0.931         3  photo timestamp 487655.677855 s",
		"photos: 4, 4 complete with every stage in order and host times monotonic",
		"  3: slot12 +0.3 ms, timestamp - slot12 0.7 ms, still-complete +269.7 ms, did-finish +270.1 ms\n",
		"older trace: pid 99, written 2026-11-14 11:53:12 (--pid 99)",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in\n%s", want, out)
		}
	}
	if strings.Contains(out, "OUT OF ORDER") || strings.Contains(out, "incomplete") {
		t.Errorf("unexpected note:\n%s", out)
	}
}

func writeTrace(t *testing.T, dir string, pid int, written int64) string {
	t.Helper()
	s := "stuntcam-trace 1\npid " + itoa(pid) + "\nbundle b\nbuild x\ntimebase 1 1\nwritten " + itoa(int(written)) + " 0\nrecorded 0\nend\n"
	p := filepath.Join(dir, "trace-"+itoa(pid)+".txt")
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func itoa(i int) string { return strconv.Itoa(i) }

func TestFindPick(t *testing.T) {
	sd, ct := t.TempDir(), t.TempDir()
	writeTrace(t, sd, 10, 100)
	writeTrace(t, ct, 11, 300) // the newest is in the fallback (the app couldn't write its state dir)
	writeTrace(t, sd, 12, 200)
	os.WriteFile(filepath.Join(sd, "trace-13.txt"), []byte("stuntcam-trace 1\npid 13\n"), 0o644) // cut short: skipped
	os.WriteFile(filepath.Join(sd, ".trace-14.tmp"), []byte("x"), 0o644)                         // the engine's temp file: ignored
	os.WriteFile(filepath.Join(sd, "events.log"), []byte("x"), 0o644)
	other := t.TempDir()
	tgt := writeTrace(t, other, 15, 900)
	if err := os.Symlink(tgt, filepath.Join(ct, "trace-15.txt")); err != nil { // a link out of the container: not followed
		t.Fatal(err)
	}
	files, skipped := Find([]Location{{sd, "state-dir"}, {ct, "container-tmp"}, {filepath.Join(sd, "missing"), "state-dir"}})
	var pids []int
	for _, f := range files {
		pids = append(pids, f.Pid)
	}
	if !reflect.DeepEqual(pids, []int{11, 12, 10}) || files[0].Location != "container-tmp" || files[1].Location != "state-dir" {
		t.Fatalf("pids %v %+v", pids, files)
	}
	if len(skipped) != 2 || !strings.Contains(strings.Join(skipped, "\n"), "trace-13.txt: "+ErrIncomplete.Error()) || !strings.Contains(strings.Join(skipped, "\n"), "trace-15.txt: not a regular file") {
		t.Fatalf("skipped %q", skipped)
	}
	f, others, err := Pick(files, 0)
	if err != nil || f.Pid != 11 || len(others) != 2 || others[0].Pid != 12 {
		t.Fatalf("newest: %v %+v %v", err, f, others)
	}
	f, others, err = Pick(files, 10)
	if err != nil || f.Pid != 10 || len(others) != 2 || others[0].Pid != 11 {
		t.Fatalf("--pid: %v %+v %v", err, f, others)
	}
	if _, _, err := Pick(files, 77); err == nil || !strings.Contains(err.Error(), "no trace of pid 77 (traces: pid 11, 12, 10)") {
		t.Fatalf("missing pid: %v", err)
	}
	if _, _, err := Pick(nil, 0); err == nil {
		t.Fatal("no files: no error")
	}
}

func TestRunning(t *testing.T) {
	list := "PID\tStatus\tLabel\n-\t0\tcom.apple.foo\n4242\t0\tUIKitApplication:dev.simcam.spike.m10plain[2b6c][rb-legacy]\n" +
		"77\t0\tUIKitApplication:dev.simcam.spike.m10plainx[aaaa]\n-\t-9\tUIKitApplication:dev.x[1]\n"
	if !Running(list, "dev.simcam.spike.m10plain", 4242) {
		t.Error("running app not found")
	}
	if Running(list, "dev.simcam.spike.m10plain", 77) || Running(list, "dev.simcam.spike.m10plain", 1) {
		t.Error("another app's pid (a longer bundle id) or a stale pid counted")
	}
	if Running(list, "dev.x", 0) {
		t.Error("an exited app (pid -) counted")
	}
}
