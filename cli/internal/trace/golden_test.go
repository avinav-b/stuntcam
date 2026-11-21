// SPDX-License-Identifier: Apache-2.0
package trace

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Characterisation tests: Parse's errors, and Build's report (JSON and table) for the real trace and for odd photos,
// compared with testdata/*.golden. `go test ./internal/trace -run Golden -update` rewrites them.

var update = flag.Bool("update", false, "rewrite testdata/*.golden")

func checkGolden(t *testing.T, name, got string) {
	t.Helper()
	path := filepath.Join("testdata", name)
	if *update {
		if err := os.WriteFile(path, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got != string(want) {
		t.Fatalf("%s differs:\n--- got\n%s\n--- want\n%s", path, got, want)
	}
}

func TestParseGolden(t *testing.T) {
	head := "stuntcam-trace 1\npid 7\nbundle b\nbuild x\ntimebase 125 3\nwritten 5 6\nrecorded 2\n"
	var out strings.Builder
	for _, in := range []string{
		"",
		"hello\n",
		"stuntcam-trace 2\nend\n",
		"stuntcam-trace 1\n",
		head + "op 0 1 2 shutter 3 0\n",
		head + "op 0 1 2 shutter 3 0\nend\n",
		head + "op 0 1 2 shutter 3\nend\n",
		head + "op x 1 2 shutter 3 0\nend\n",
		head + "op 0 1 2 shutter y z\nend\n",
		"stuntcam-trace 1\npid seven\nend\n",
		"stuntcam-trace 1\ntimebase 1 x\nend\n",
		"stuntcam-trace 1\nwritten x 1\nend\n",
		"stuntcam-trace 1\nrecorded -1\nend\n",
		"stuntcam-trace 1\npid 1 2\nbundle\nunknown line\n\nend\n",
		"stuntcam-trace 1\ntimebase 0 0\nend\n",
	} {
		f, err := Parse([]byte(in))
		fmt.Fprintf(&out, "== %q\nerr: %v\n%+v\n", in, err, f)
	}
	checkGolden(t, "parse.golden", out.String())
}

func TestBuildGolden(t *testing.T) {
	real := load(t)
	real.Path, real.Location = "/s/trace-61256.txt", "state-dir"
	older := File{Pid: 9, Path: "/t/trace-9.txt", Location: "container-tmp", WrittenNs: 1794657000000000000}
	odd := File{Numer: 125, Denom: 3, Pid: 2, Bundle: "b", Build: "x", WrittenNs: 1794657193946587000, Recorded: 30}
	add := func(name string, sid, arg int64, mach uint64) {
		i := uint64(len(odd.Ops))
		odd.Ops = append(odd.Ops, Op{Index: i, WallNs: 1794657180000000000 + int64(i)*1000, Mach: mach, Name: name,
			SettingsID: sid, Arg: arg})
	}
	add("activate", -1, 0, 100)
	add("slot12", 1, 0, 1000) // photo 1: no shutter, origin slot12
	add("timestamp", 1, 4870, 1100)
	add("still-complete", 1, 0, 1200)
	add("shutter", 2, 0, 2000) // photo 2: time goes back, a stage twice
	add("slot12", 2, 0, 1900)
	add("slot12", 2, 0, 2100)
	add("shutter", 3, 0, 3000) // photo 3: failed
	add("slot12", 3, 0, 3100)
	add("timestamp", 3, 9, 3200)
	add("still-complete", 3, -16800, 3300)
	add("encoded", 4, 512, 3400)  // photo 4: a stage alone
	add("movie-stop", 7, 3, 3500) // not a photo stage, with a settings id
	add("crash", -1, 11, 3600)
	add("session-start", -1, 2, 3700)
	running, stopped := true, false
	var out strings.Builder
	for _, c := range []struct {
		name    string
		f       File
		others  []File
		running *bool
	}{
		{"real", real, []File{older}, &running},
		{"odd", odd, nil, &stopped},
		{"empty", File{Numer: 1, Denom: 1}, nil, nil},
	} {
		r := Build(c.f, c.others, "UDID", "iPhone", c.running)
		b, err := json.MarshalIndent(r, "", " ")
		if err != nil {
			t.Fatal(err)
		}
		var h bytes.Buffer
		Human(&h, r, time.Unix(0, c.f.WrittenNs).Add(90*time.Second), time.UTC)
		fmt.Fprintf(&out, "== %s\n%s\n%s", c.name, b, h.String())
	}
	checkGolden(t, "build.golden", out.String())
}
