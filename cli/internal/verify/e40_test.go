// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"strings"
	"testing"
	"time"
)

// Tests for w4 e40: a content fail is confirmed by one more launch before it is cached.

const (
	e40Pass  = "RESULT core=PASS ext=PASS t=13.4s"
	e40Fail  = "RESULT core=FAIL ext=PASS t=13.2s core:[photo-then-stop: didFinishCaptureFor before didStopRunning (stop notifications at completion: 1)]"
	e40Fail2 = "RESULT core=FAIL ext=PASS t=13.3s core:[photo-jpeg: fileDataRepresentation decodes]"
	e40Inc   = "RESULT core=INCONCLUSIVE ext=PASS t=18.0s timing:[frames: A frames=4 (>= 12 in window) dropped=0]"
	e40Crash = "RESULT core=FAIL ext=FAIL core:[canary exited without a verdict (crash); last: CANARY| 6.1 ok x]"
)

func TestContentFail(t *testing.T) {
	for res, want := range map[string]bool{e40Fail: true, e40Fail2: true, e40Pass: false, e40Inc: false, e40Crash: false} {
		if got := ContentFail(res); got != want {
			t.Errorf("ContentFail(%.40q) = %v, want %v", res, got, want)
		}
	}
}

func TestConfirmFail(t *testing.T) {
	for _, c := range []struct {
		name, first, second string
		launches            int
		wantResult          string
		wantFailedFirst     bool
		wantNoStore         bool
		wantNote            string
	}{
		{"a pass needs no confirmation", e40Pass, "", 0, e40Pass, false, false, ""},
		{"an inconclusive is not confirmed here", e40Inc, "", 0, e40Inc, false, false, ""},
		{"a crash fail already took two crashes", e40Crash, "", 0, e40Crash, false, false, ""},
		{"a second content fail is the verdict", e40Fail, e40Fail, 1, e40Fail, false, false, "confirmed by a second launch"},
		{"another content fail confirms too", e40Fail, e40Fail2, 1, e40Fail2, false, false, "confirmed by a second launch: fail(photo-then-stop"},
		{"a pass after a fail", e40Fail, e40Pass, 1, e40Pass, true, false, "the launch run to confirm it passed"},
		{"an inconclusive after a fail", e40Fail, e40Inc, 1, e40Fail, false, true, "not cached"},
		{"a crash after a fail", e40Fail, e40Crash, 1, e40Fail, false, true, "not cached"},
	} {
		t.Run(c.name, func(t *testing.T) {
			n := 0
			got := confirmFail(c.first, func() string { n++; return c.second })
			if n != c.launches {
				t.Fatalf("%d confirming launches, want %d", n, c.launches)
			}
			if got.Result != c.wantResult || (got.FailedFirst != "") != c.wantFailedFirst || got.NoStore != c.wantNoStore || (c.wantNote == "" && got.Note != "") || !strings.Contains(got.Note, c.wantNote) {
				t.Fatalf("got %+v", got)
			}
			if c.wantFailedFirst && got.FailedFirst != c.first {
				t.Fatalf("FailedFirst %q, want the first RESULT", got.FailedFirst)
			}
		})
	}
}

// a pass after a confirming launch is cached like a relaunched pass (RetriedPassTTL); a confirmed fail is cached for good
func TestFailedFirstCache(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	now := time.Now()
	r := Result{Key: "ff", Verdict: "pass", State: Pass, FailedFirst: e40Fail, CanaryResult: e40Pass, At: now.Add(-10 * time.Minute).Unix()}
	if err := store(r); err != nil {
		t.Fatal(err)
	}
	if got, ok, _, _ := cached("ff", now); !ok || !got.RetriedPass() || got.FailedFirst != e40Fail {
		t.Fatalf("fresh pass after a fail: ok=%v %+v", ok, got)
	}
	r.Key, r.At = "ff-old", now.Add(-2*time.Hour).Unix()
	store(r)
	if _, ok, why, _ := cached("ff-old", now); ok || !strings.Contains(why, "canary retry") {
		t.Fatalf("old pass after a fail used: ok=%v why=%q", ok, why)
	}
	st, v := Classify(e40Fail)
	f := Result{Key: "fail-old", Verdict: v, State: st, CanaryResult: e40Fail, At: now.Add(-48 * time.Hour).Unix()}
	store(f)
	if got, ok, _, _ := cached("fail-old", now); !ok || got.State != Fail || got.RetriedPass() {
		t.Fatalf("confirmed fail: ok=%v %+v", ok, got)
	}
}

// w4 e44 (review L9): decide, Run's launch-to-cache tail, with a fake canary: which launches run, what is cached,
// and FailedFirst's wiring into the Result and its cache file.
func TestDecideConfirmFail(t *testing.T) {
	for _, c := range []struct {
		name        string
		results     []string // RESULT per launch, in order
		wantLogs    string   // the launches' log suffixes
		wantVerdict State
		wantFF      bool
		wantFile    bool
	}{
		{"fail then inconclusive: reported, not cached", []string{e40Fail, e40Inc}, "1 c", Fail, false, false},
		{"fail then crash: reported, not cached", []string{e40Fail, e40Crash}, "1 c", Fail, false, false},
		{"fail then pass: short-lived pass", []string{e40Fail, e40Pass}, "1 c", Pass, true, true},
		{"fail then fail: cached fail", []string{e40Fail, e40Fail}, "1 c", Fail, false, true},
		{"inconclusive, fail, fail: three launches", []string{e40Inc, e40Fail, e40Fail2}, "1 2 c", Fail, false, true},
		{"pass: one launch", []string{e40Pass}, "1", Pass, false, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Setenv("SIMCAM_HOME", t.TempDir())
			var logs []string
			l := launches{
				run: func(n string) (string, bool, bool) {
					logs = append(logs, n)
					if len(logs) > len(c.results) {
						t.Fatalf("launch %d (%s): only %d expected", len(logs), n, len(c.results))
					}
					return c.results[len(logs)-1], false, true
				},
				reboot: func() error { t.Fatal("reboot"); return nil },
				redo:   func(bool) (bool, error) { return false, nil },
			}
			r := Result{Key: "k", At: time.Now().Unix()}
			decide(&r, l, time.Now())
			if got := strings.Join(logs, " "); got != c.wantLogs {
				t.Fatalf("launches %q, want %q", got, c.wantLogs)
			}
			if r.State != c.wantVerdict || (r.FailedFirst != "") != c.wantFF {
				t.Fatalf("got %+v", r)
			}
			if c.wantFF && (r.FailedFirst != e40Fail || !r.RetriedPass()) {
				t.Fatalf("FailedFirst %q, RetriedPass %v", r.FailedFirst, r.RetriedPass())
			}
			got, ok, _, _ := cached("k", time.Now())
			if ok != c.wantFile {
				t.Fatalf("cached: %v, want %v (%+v)", ok, c.wantFile, got)
			}
			if ok && (got.FailedFirst != r.FailedFirst || got.State != r.State) {
				t.Fatalf("cache file %+v, Result %+v", got, r)
			}
		})
	}
	t.Run("cache bypassed: nothing written", func(t *testing.T) {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		n := 0
		l := launches{run: func(string) (string, bool, bool) { n++; return []string{e40Fail, e40Pass}[n-1], false, true },
			reboot: func() error { return nil }, redo: func(bool) (bool, error) { return false, nil }}
		r := Result{Key: "k", NoCache: "SIMCTL_CHILD_X"}
		decide(&r, l, time.Now())
		if _, ok, _, _ := cached("k", time.Now()); ok || r.FailedFirst == "" {
			t.Fatalf("cached %v, %+v", ok, r)
		}
	})
}
