// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func writeVerdict(t *testing.T, dir string, r Result) {
	t.Helper()
	os.MkdirAll(dir, 0o755)
	b, _ := json.Marshal(r)
	if err := os.WriteFile(filepath.Join(dir, r.Key+".json"), b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// w4 e51: the package ships the release gate's canary passes (design §2 C-MF1); only a plain pass counts.
func TestShippedVerdicts(t *testing.T) {
	d := t.TempDir()
	now := time.Now().Unix()
	writeVerdict(t, d, Result{Key: "pass", Verdict: "pass", CanaryResult: "RESULT core=PASS", At: now})
	writeVerdict(t, d, Result{Key: "fail", Verdict: "fail(photo)", At: now})
	writeVerdict(t, d, Result{Key: "retried", Verdict: "pass", Retried: "retried:[photo]", At: now})
	writeVerdict(t, d, Result{Key: "retried-line", Verdict: "pass", CanaryResult: "RESULT core=PASS retried:[photo.0]", At: now})
	writeVerdict(t, d, Result{Key: "relaunched", Verdict: "pass", Relaunched: "RESULT inconclusive", At: now})
	writeVerdict(t, d, Result{Key: "failedfirst", Verdict: "pass", FailedFirst: "RESULT core=FAIL", At: now})
	writeVerdict(t, d, Result{Key: "inconclusive", Verdict: "inconclusive(boot)", At: now})
	writeVerdict(t, d, Result{Key: "other-key-inside", Verdict: "pass", At: now})
	os.Rename(filepath.Join(d, "other-key-inside.json"), filepath.Join(d, "renamed.json"))
	os.WriteFile(filepath.Join(d, "junk.json"), []byte("{"), 0o644)

	if r, ok := Shipped(d, "pass"); !ok || !r.Shipped || !r.Cached || r.State != Pass {
		t.Fatalf("plain pass: %+v %v", r, ok)
	}
	for _, k := range []string{"fail", "retried", "retried-line", "relaunched", "failedfirst", "inconclusive", "renamed", "junk", "missing", "../pass", ""} {
		if _, ok := Shipped(d, k); ok {
			t.Errorf("%q must be a miss", k)
		}
	}
	if _, ok := Shipped("", "pass"); ok {
		t.Error("no shipped dir")
	}
}

// This Mac's cache comes first: a fail it cached wins over a shipped pass for the same key; with nothing cached, the
// shipped pass is used. Any local entry for the key (an expired retried, relaunched or failed-first pass, an unreadable
// file) means this Mac measured the build itself: the shipped verdict is skipped and the canary runs (review), and
// the message says so truthfully.
func TestLocalCacheBeatsShipped(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	now := time.Now()
	d := t.TempDir()
	for _, k := range []string{"k", "only-shipped", "old-retried", "old-relaunched", "old-failedfirst", "junk", "fresh-retried"} {
		writeVerdict(t, d, Result{Key: k, Verdict: "pass", At: now.Unix(), PackagerXcode: "27A266a"})
	}
	old := now.Add(-2 * RetriedPassTTL).Unix()
	for _, r := range []Result{
		{Key: "k", Verdict: "fail(photo)", State: Fail, At: now.Unix()},
		{Key: "old-retried", Verdict: "pass", State: Pass, Retried: "retried:[photo]", At: old},
		{Key: "old-relaunched", Verdict: "pass", State: Pass, Relaunched: "RESULT inconclusive", At: old},
		{Key: "old-failedfirst", Verdict: "pass", State: Pass, FailedFirst: "RESULT core=FAIL", At: old},
		{Key: "fresh-retried", Verdict: "pass", State: Pass, Retried: "retried:[photo]", At: now.Unix()},
	} {
		if err := store(r); err != nil {
			t.Fatal(err)
		}
	}
	os.WriteFile(cachePath("junk"), []byte("{"), 0o644)
	if r, ok, _ := lookup("k", d, now); !ok || r.State != Fail || r.Shipped {
		t.Fatalf("local fail: %+v %v", r, ok)
	}
	if r, ok, why := lookup("only-shipped", d, now); !ok || r.State != Pass || !r.Shipped || why != "" || r.PackagerXcode != "27A266a" {
		t.Fatalf("shipped: %+v %v %q", r, ok, why)
	}
	if r, ok, _ := lookup("fresh-retried", d, now); !ok || r.Shipped || !r.RetriedPass() {
		t.Fatalf("a fresh local retried pass is used as is: %+v %v", r, ok)
	}
	for k, want := range map[string]string{"old-retried": "needed a canary retry and", "old-relaunched": "(a relaunch)",
		"old-failedfirst": "(its first launch failed)", "junk": "unreadable"} {
		r, ok, why := lookup(k, d, now)
		if ok || r.Shipped || !strings.Contains(why, want) || !strings.Contains(why, "running the canary again") ||
			!strings.Contains(why, "takes precedence over the one shipped") {
			t.Errorf("%s: an expired or unreadable local entry must skip the shipped verdict: %+v %v %q", k, r, ok, why)
		}
	}
	// without a shipped verdict for it, the line doesn't mention one
	if _, ok, why := lookup("old-retried", t.TempDir(), now); ok || strings.Contains(why, "shipped") {
		t.Fatalf("%v %q", ok, why)
	}
	if _, ok, _ := lookup("none", d, now); ok {
		t.Fatal("miss")
	}
}

func TestShippedSource(t *testing.T) {
	at := time.Date(2026, 11, 14, 12, 0, 0, 0, time.Local).Unix()
	if s := (Result{At: at, PackagerXcode: "27A266a"}).ShippedSource(); s != "measured 2026-11-14, packaged with Xcode 27A266a" {
		t.Fatal(s)
	}
	if s := (Result{At: at}).ShippedSource(); !strings.Contains(s, "Xcode unknown") {
		t.Fatal(s)
	}
}
