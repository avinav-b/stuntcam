// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/avinav-b/stuntcam/cli/internal/home"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

func TestClassify(t *testing.T) {
	cases := map[string]State{
		"RESULT core=PASS ext=PASS t=12.1s":                                   Pass,
		"RESULT core=FAIL ext=PASS t=16.2s core:[frames: A frames=0 (>= 12)]": Fail,
		"RESULT core=INCONCLUSIVE ext=PASS t=14s timing:[frames: A fps 7.3]":  Inconclusive,
		"RESULT core=INCONCLUSIVE ext=INCONCLUSIVE watchdog stage=movie":      Inconclusive,
		"": Inconclusive,
	}
	for in, want := range cases {
		if got, v := Classify(in); got != want {
			t.Errorf("%q: %s (%s)", in, got, v)
		}
	}
	for in, want := range map[string]string{
		"RESULT core=FAIL ext=PASS core:[qr: QR 'x' via metadata]":                                                      "fail(qr: QR 'x' via metadata)",
		`RESULT core=FAIL ext=PASS t=9s core:[movie: movie track ["1920x1080"] matches; devices=[]] timing:[a] ext:[b]`: `fail(movie: movie track ["1920x1080"] matches; devices=[])`,
		"RESULT core=FAIL ext=FAIL (no verdict)":                                                                        "fail()",
		"RESULT core=INCONCLUSIVE (no verdict in 60 s; last: x)":                                                        "inconclusive(core=INCONCLUSIVE (no verdict in 60 s; last: x))",
	} {
		if _, v := Classify(in); v != want {
			t.Errorf("%q -> %q, want %q", in, v, want)
		}
	}
}

// the canary's retried:[…] field sits before core:[, so verdicts and fail texts are what they were
func TestClassifyRetried(t *testing.T) {
	r := "RESULT core=PASS ext=PASS t=16.7s retried:[photo-4x3: longest frame gap during the photo 215 ms (<= 100 ms, 4 frames) -> retry ok]"
	if st, v := Classify(r); st != Pass || v != "pass" {
		t.Fatal(st, v)
	}
	if got := Retried(r); got != "photo-4x3: longest frame gap during the photo 215 ms (<= 100 ms, 4 frames) -> retry ok" {
		t.Fatalf("%q", got)
	}
	twice := "RESULT core=INCONCLUSIVE ext=PASS t=17s retried:[photo-hvc1: longest frame gap during the photo 158 ms (<= 100 ms, 9 frames) -> retry SLOW] timing:[photo-hvc1: longest frame gap during the photo 140 ms (<= 100 ms, 8 frames)]"
	if st, _ := Classify(twice); st != Inconclusive {
		t.Fatal("missed twice:", st)
	}
	if got := Retried(twice); !strings.HasSuffix(got, "-> retry SLOW") {
		t.Fatalf("%q", got)
	}
	fail := "RESULT core=FAIL ext=PASS t=17s retried:[frames: A fps 7.3 (>= 10) -> retry FAIL] core:[frames: A pixels not all zero] ext:[x]"
	if st, v := Classify(fail); st != Fail || v != "fail(frames: A pixels not all zero)" {
		t.Fatal(st, v)
	}
	if Retried("RESULT core=PASS ext=PASS t=14.1s") != "" {
		t.Fatal("no retried field")
	}
}

func TestCache(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if err := store(Result{Key: "k1", State: Inconclusive, Verdict: "inconclusive(x)"}); err != nil {
		t.Fatal(err)
	}
	if _, ok := Cached("k1"); ok {
		t.Fatal("inconclusive cached")
	}
	if err := store(Result{Key: "k2", State: Pass, Verdict: "pass"}); err != nil {
		t.Fatal(err)
	}
	if r, ok := Cached("k2"); !ok || !r.Cached || r.State != Pass {
		t.Fatalf("%+v %v", r, ok)
	}
	// a file scripts/verify.sh wrote: no state field, the state comes from the verdict
	os.MkdirAll(home.Verdicts(), 0o755)
	os.WriteFile(filepath.Join(home.Verdicts(), "k3.json"), []byte(`{"proto":1,"key":"k3","verdict":"fail(qr: no QR)","result":"RESULT core=FAIL","runtime_build":"23D8133","engine_build":"e","stub_build":"s","canary_sha":"c","profile_sha":"p","device":"U","times":"total=40s","at":1}`), 0o644)
	if r, ok := Cached("k3"); !ok || r.State != Fail {
		t.Fatalf("shell file: %+v %v", r, ok)
	}
	for k, body := range map[string]string{"k4": `{"verdict":"maybe"}`, "k5": `{"verdict":`} {
		os.WriteFile(filepath.Join(home.Verdicts(), k+".json"), []byte(body), 0o644)
		if _, ok := Cached(k); ok {
			t.Errorf("%s: not a miss", body)
		}
	}
	if Key("23D8133", "899608875612", "09908f5dbe40", "1a2b3c4d", "31b77b151791") != "23D8133-899608875612-09908f5dbe40-c1a2b3c4d-31b77b151791" {
		t.Error("key format differs from scripts/verify.sh")
	}
}

func TestLock(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Root(), 0o755)
	d := lockDir("23D8133")
	if ok, why := acquire(d); !ok {
		t.Fatal(why)
	}
	// held by a live process (this one, as seen by a second caller)
	if ok, _ := acquire(d); ok {
		t.Fatal("acquired twice")
	}
	release(d)
	if _, err := os.Stat(d); err == nil {
		t.Fatal("not released")
	}
	// a dead owner's lock is taken over; someone else's live lock is not released by us
	os.Mkdir(d, 0o755)
	os.WriteFile(filepath.Join(d, "pid"), []byte("999999\n"), 0o644)
	if ok, why := acquire(d); !ok {
		t.Fatal("stale lock not taken over: " + why)
	}
	os.WriteFile(filepath.Join(d, "pid"), []byte("1\n"), 0o644)
	release(d)
	if _, err := os.Stat(d); err != nil {
		t.Fatal("released another owner's lock")
	}
}

func TestProPhonesNewestFirst(t *testing.T) {
	got := proPhonesNewestFirst([]simctl.DeviceType{{Name: "iPhone 15 Pro"}, {Name: "iPhone 17 Pro Max"}, {Name: "iPhone 18 Pro"}, {Name: "iPhone 17 Pro"}, {Name: "iPad Pro"}})
	if len(got) != 3 || got[0].Name != "iPhone 18 Pro" || got[2].Name != "iPhone 15 Pro" {
		t.Fatalf("%v", got)
	}
}

func TestFinalNeedsTwoCrashes(t *testing.T) {
	crash := "RESULT core=FAIL ext=FAIL core:[" + crashText + "; last: x]"
	inc := "RESULT core=INCONCLUSIVE (no verdict in 60 s; last: x)"
	if st, _ := Classify(Final(inc, crash)); st != Inconclusive {
		t.Error("one crash after an inconclusive run became", st)
	}
	if st, _ := Classify(Final(crash, crash)); st != Fail {
		t.Error("two crashes:", st)
	}
	if st, _ := Classify(Final(crash, "RESULT core=PASS ext=PASS")); st != Pass {
		t.Error("crash then pass:", st)
	}
}
