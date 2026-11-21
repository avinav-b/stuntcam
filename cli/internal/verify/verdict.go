// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// StateOf maps a verdict string to its state ("" = not a verdict).
func StateOf(verdict string) State {
	switch {
	case verdict == "pass":
		return Pass
	case strings.HasPrefix(verdict, "fail("):
		return Fail
	case strings.HasPrefix(verdict, "inconclusive("):
		return Inconclusive
	}
	return ""
}

// Classify turns the canary's RESULT line into a verdict, with scripts/verify.sh's text rules: fail(<the core:[…] list,
// up to the timing:[ or ext:[ list, failChecksLen bytes>), inconclusive(<the line after "RESULT ", inconclusiveLen bytes>).
func Classify(result string) (State, string) {
	switch {
	case strings.Contains(result, "core=PASS"):
		return Pass, "pass"
	case strings.Contains(result, "core=FAIL"):
		checks := ""
		if i := strings.Index(result, "core:["); i >= 0 {
			checks = result[i+len("core:["):]
			cut := false
			for _, sep := range []string{"] timing:[", "] ext:["} {
				if j := strings.Index(checks, sep); j >= 0 {
					checks, cut = checks[:j], true
				}
			}
			if !cut { // the list ends the line: drop its own closing bracket only
				checks = strings.TrimSuffix(checks, "]")
			}
		}
		return Fail, failVerdict(checks)
	default:
		r := result
		if len(r) >= 7 {
			r = r[7:] // "RESULT "
		} else {
			r = ""
		}
		return Inconclusive, inconclusiveVerdict(r)
	}
}

// Retried returns the canary's retried:[…] list (stages whose first attempt missed only timing checks and were run again
// at the end of the same launch), or "".
func Retried(result string) string {
	i := strings.Index(result, " retried:[")
	if i < 0 {
		return ""
	}
	rest := result[i+len(" retried:["):]
	for _, sep := range []string{"] core:[", "] timing:[", "] ext:["} {
		if j := strings.Index(rest, sep); j >= 0 {
			rest = rest[:j]
		}
	}
	return strings.TrimSuffix(rest, "]")
}

func trim(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

// RetriedPassTTL is how long a retried pass is cached. The canary's in-process retry of a photo's frame-gap check
// runs ~13 s later in the same, already warm process, so it can't tell a post-boot stall (what it is for) from a
// regression that only hits the first photo of a process: both show as "-> retry ok". A plain pass is cached until the
// key changes; a retried pass is re-run by the first verify or `up` after an hour, which bounds how long such a regression
// can hide behind one cached verdict, without making every `up` of a burst (a CI matrix, a morning of rebuilds) pay a
// 20-25 s canary when a healthy engine needed the retry once. scripts/verify.sh applies the same rule.
const RetriedPassTTL = time.Hour

// RetriedPassTTLText is RetriedPassTTL for messages ("1 h").
func RetriedPassTTLText() string {
	if RetriedPassTTL%time.Hour == 0 {
		return fmt.Sprintf("%d h", RetriedPassTTL/time.Hour)
	}
	return fmt.Sprintf("%d min", RetriedPassTTL/time.Minute)
}

// RetriedPass says whether a verdict is a pass that needed the canary's frame-gap retry, a whole relaunch, or a launch to
// confirm a content fail that then passed: such a pass is cached for RetriedPassTTL only.
func (r Result) RetriedPass() bool {
	return r.State == Pass && (r.Retried != "" || r.Relaunched != "" || r.FailedFirst != "")
}

// confirmation is what confirmFail decided.
type confirmation struct {
	Decisive    canaryRun // the launch that decides the verdict
	FailedFirst string    // the confirmed fail's RESULT when the confirming launch passed
	NoStore     bool      // a content fail the confirming launch couldn't confirm or refute: report it, don't cache it
	Note        string    // for Result.Notes, "" if no confirming launch ran
}

// confirmFail runs one more launch (again) after a content fail, so a fail is cached only when two launches in a row
// failed on content: a cached fail makes `up` refuse the build until a --fresh verify, so a one-off (an engine race, a
// flaky check) must not be cached. Anything else is returned as it is, without a launch.
func confirmFail(first canaryRun, again func() canaryRun) confirmation {
	if !first.contentFail() {
		return confirmation{Decisive: first}
	}
	_, firstVerdict := first.verdict()
	second := again()
	secondState, secondVerdict := second.verdict()
	switch {
	case second.contentFail():
		return confirmation{Decisive: second, Note: "confirmed by a second launch: " + trim(firstVerdict, noteLen)}
	case secondState == Pass:
		return confirmation{
			Decisive:    second,
			FailedFirst: trim(first.resultLine(), recordLen),
			Note:        "the first launch failed (" + trim(firstVerdict, noteLen) + "); the launch run to confirm it passed",
		}
	default: // inconclusive, or a crash: no second opinion
		return confirmation{
			Decisive: first,
			NoStore:  true,
			Note:     "not cached: the launch run to confirm this fail gave no verdict (" + trim(secondVerdict, noteLen) + ")",
		}
	}
}

// launches is how decide reaches the device: run launches the canary once (n: the log suffix: 1, 1r, 2, c) and returns
// what it came to; reboot re-arms and reboots after refused launches; redo reinstalls and re-grants a skipped install after
// a launch that didn't start. Fakes in tests.
type launches struct {
	run    func(n string) canaryRun
	reboot func() error
	redo   func(ran bool) (bool, error)
}

// decide runs the canary through l until it has a verdict (a relaunch after an INCONCLUSIVE or a crash, a confirming
// launch after a content fail), fills r's verdict, times (t0: when Run started) and notes, and caches a pass or fail.
func decide(r *Result, l launches, t0 time.Time) {
	t2 := time.Now()
	res := l.run("1")
	if res.refused() {
		r.Notes = append(r.Notes, "every launch refused this boot; rebooted once")
		if err := l.reboot(); err != nil {
			r.State, r.Verdict = Inconclusive, "inconclusive(reboot after refused launches: "+err.Error()+")"
			return
		}
		res = l.run("1r")
	}
	if st, _ := res.verdict(); st == Inconclusive || res.crashed() {
		r.Notes = append(r.Notes, "first run: "+trim(res.resultLine(), noteLen))
		if redone, err := l.redo(res.ran); err != nil {
			r.State, r.Verdict = Inconclusive, "inconclusive("+err.Error()+")"
			return
		} else if redone {
			r.Notes = append(r.Notes, "the canary did not start with the install or grant skipped: reinstalled and granted")
		}
		second := l.run("2")
		r.Relaunched = trim(res.resultLine(), recordLen)
		res = finalRun(res, second)
	}
	conf := confirmFail(res, func() canaryRun {
		mark("confirm-fail")
		return l.run("c")
	})
	if conf.Note != "" {
		r.Notes = append(r.Notes, conf.Note)
	}
	res, r.FailedFirst = conf.Decisive, conf.FailedFirst
	mark("canary-done")
	r.CanarySeconds = int(time.Since(t2).Seconds())
	r.Times = fmt.Sprintf("create=%ds boot=%ds canary=%ds total=%ds",
		r.CreateSeconds, r.BootSeconds, r.CanarySeconds, int(time.Since(t0).Seconds()))
	r.CanaryResult = res.resultLine()
	r.State, r.Verdict = res.verdict()
	if r.Retried = res.retried(); r.Retried != "" {
		r.Notes = append(r.Notes, "canary stage retried: "+trim(r.Retried, recordLen))
	}
	if r.NoCache == "" && !conf.NoStore {
		if err := store(*r); err != nil {
			r.Notes = append(r.Notes, "could not cache the verdict: "+err.Error())
		}
	}
}

func logPath(key, runID, n string) string {
	return filepath.Join(home.Logs(), "verify-"+key+"-"+runID+"-"+n+".log")
}
