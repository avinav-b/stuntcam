// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"strings"
	"testing"
	"time"
)

// Tests for confirming a fail: a content fail is confirmed by one more launch before it is cached.

const (
	resultPass         = "RESULT core=PASS ext=PASS t=13.4s"
	resultContentFail  = "RESULT core=FAIL ext=PASS t=13.2s core:[photo-then-stop: didFinishCaptureFor before didStopRunning (stop notifications at completion: 1)]"
	resultContentFail2 = "RESULT core=FAIL ext=PASS t=13.3s core:[photo-jpeg: fileDataRepresentation decodes]"
	resultInconclusive = "RESULT core=INCONCLUSIVE ext=PASS t=18.0s timing:[frames: A frames=4 (>= 12 in window) dropped=0]"
)

// a canary that ran and exited without a verdict
var resultCrashFail = canaryRun{outcome: exitedNoVerdict, detail: "CANARY| 6.1 ok x", ran: true}

// printed is a launch whose canary printed the RESULT line
func printed(line string) canaryRun { return canaryRun{outcome: gotResult, line: line, ran: true} }

func TestContentFail(t *testing.T) {
	for _, c := range []struct {
		run  canaryRun
		want bool
	}{{printed(resultContentFail), true}, {printed(resultContentFail2), true}, {printed(resultPass), false}, {printed(resultInconclusive), false}, {resultCrashFail, false}} {
		if got := c.run.contentFail(); got != c.want {
			t.Errorf("contentFail(%.40q) = %v, want %v", c.run.resultLine(), got, c.want)
		}
	}
}

func TestConfirmFail(t *testing.T) {
	for _, c := range []struct {
		name            string
		first, second   canaryRun
		launches        int
		wantResult      canaryRun
		wantFailedFirst bool
		wantNoStore     bool
		wantNote        string
	}{
		{"a pass needs no confirmation", printed(resultPass), canaryRun{}, 0, printed(resultPass), false, false, ""},
		{"an inconclusive is not confirmed here", printed(resultInconclusive), canaryRun{}, 0, printed(resultInconclusive), false, false, ""},
		{"a crash fail already took two crashes", resultCrashFail, canaryRun{}, 0, resultCrashFail, false, false, ""},
		{"a second content fail is the verdict", printed(resultContentFail), printed(resultContentFail), 1, printed(resultContentFail), false, false,
			"confirmed by a second launch"},
		{"another content fail confirms too", printed(resultContentFail), printed(resultContentFail2), 1, printed(resultContentFail2), false, false,
			"confirmed by a second launch: fail(photo-then-stop"},
		{"a pass after a fail", printed(resultContentFail), printed(resultPass), 1, printed(resultPass), true, false,
			"the launch run to confirm it passed"},
		{"an inconclusive after a fail", printed(resultContentFail), printed(resultInconclusive), 1, printed(resultContentFail), false, true, "not cached"},
		{"a crash after a fail", printed(resultContentFail), resultCrashFail, 1, printed(resultContentFail), false, true, "not cached"},
	} {
		t.Run(c.name, func(t *testing.T) {
			n := 0
			got := confirmFail(c.first, func() canaryRun { n++; return c.second })
			if n != c.launches {
				t.Fatalf("%d confirming launches, want %d", n, c.launches)
			}
			if got.Decisive != c.wantResult || (got.FailedFirst != "") != c.wantFailedFirst || got.NoStore != c.wantNoStore || (c.wantNote == "" && got.Note != "") || !strings.Contains(got.Note, c.wantNote) {
				t.Fatalf("got %+v", got)
			}
			if c.wantFailedFirst && got.FailedFirst != c.first.resultLine() {
				t.Fatalf("FailedFirst %q, want the first RESULT", got.FailedFirst)
			}
		})
	}
}

// a pass after a confirming launch is cached like a relaunched pass (RetriedPassTTL); a confirmed fail is cached for good
func TestFailedFirstCache(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	now := time.Now()
	r := Result{Key: "ff", Verdict: "pass", State: Pass, FailedFirst: resultContentFail, CanaryResult: resultPass, At: now.Add(-10 * time.Minute).Unix()}
	if err := store(r); err != nil {
		t.Fatal(err)
	}
	if c := cached("ff", now); !c.usable || !c.result.RetriedPass() || c.result.FailedFirst != resultContentFail {
		t.Fatalf("fresh pass after a fail: usable=%v %+v", c.usable, c.result)
	}
	r.Key, r.At = "ff-old", now.Add(-2*time.Hour).Unix()
	store(r)
	if c := cached("ff-old", now); c.usable || !strings.Contains(c.why, "canary retry") {
		t.Fatalf("old pass after a fail used: usable=%v why=%q", c.usable, c.why)
	}
	st, v := Classify(resultContentFail)
	f := Result{Key: "fail-old", Verdict: v, State: st, CanaryResult: resultContentFail, At: now.Add(-48 * time.Hour).Unix()}
	store(f)
	if c := cached("fail-old", now); !c.usable || c.result.State != Fail || c.result.RetriedPass() {
		t.Fatalf("confirmed fail: usable=%v %+v", c.usable, c.result)
	}
}

// decide, Run's launch-to-cache tail, with a fake canary: which launches run, what is cached,
// and FailedFirst's wiring into the Result and its cache file.
func TestDecideConfirmFail(t *testing.T) {
	for _, c := range []struct {
		name        string
		results     []canaryRun // per launch, in order
		wantLogs    string      // the launches' log suffixes
		wantVerdict State
		wantFF      bool
		wantFile    bool
	}{
		{"fail then inconclusive: reported, not cached", []canaryRun{printed(resultContentFail), printed(resultInconclusive)}, "1 c", Fail, false, false},
		{"fail then crash: reported, not cached", []canaryRun{printed(resultContentFail), resultCrashFail}, "1 c", Fail, false, false},
		{"fail then pass: short-lived pass", []canaryRun{printed(resultContentFail), printed(resultPass)}, "1 c", Pass, true, true},
		{"fail then fail: cached fail", []canaryRun{printed(resultContentFail), printed(resultContentFail)}, "1 c", Fail, false, true},
		{"inconclusive, fail, fail: three launches",
			[]canaryRun{printed(resultInconclusive), printed(resultContentFail), printed(resultContentFail2)}, "1 2 c", Fail, false, true},
		{"pass: one launch", []canaryRun{printed(resultPass)}, "1", Pass, false, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Setenv("SIMCAM_HOME", t.TempDir())
			var logs []string
			l := launches{
				run: func(n string) canaryRun {
					logs = append(logs, n)
					if len(logs) > len(c.results) {
						t.Fatalf("launch %d (%s): only %d expected", len(logs), n, len(c.results))
					}
					return c.results[len(logs)-1]
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
			if c.wantFF && (r.FailedFirst != resultContentFail || !r.RetriedPass()) {
				t.Fatalf("FailedFirst %q, RetriedPass %v", r.FailedFirst, r.RetriedPass())
			}
			entry := cached("k", time.Now())
			got := entry.result
			if entry.usable != c.wantFile {
				t.Fatalf("cached: %v, want %v (%+v)", entry.usable, c.wantFile, got)
			}
			if entry.usable && (got.FailedFirst != r.FailedFirst || got.State != r.State) {
				t.Fatalf("cache file %+v, Result %+v", got, r)
			}
		})
	}
	t.Run("cache bypassed: nothing written", func(t *testing.T) {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		n := 0
		l := launches{run: func(string) canaryRun { n++; return []canaryRun{printed(resultContentFail), printed(resultPass)}[n-1] },
			reboot: func() error { return nil }, redo: func(bool) (bool, error) { return false, nil }}
		r := Result{Key: "k", NoCache: "SIMCTL_CHILD_X"}
		decide(&r, l, time.Now())
		if c := cached("k", time.Now()); c.usable || r.FailedFirst == "" {
			t.Fatalf("cached %v, %+v", c.usable, r)
		}
	})
}
