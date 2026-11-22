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

// resultPrefix starts the canary's RESULT line. inconclusive(…) quotes the line after it.
const resultPrefix = "RESULT "

// Classify turns the canary's RESULT line into a verdict, with scripts/verify.sh's text rules: fail(<the core:[…] list,
// up to the timing:[ or ext:[ list, failChecksLen bytes>), inconclusive(<the line after "RESULT ", inconclusiveLen
// bytes>).
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
					checks = checks[:j]
					cut = true
				}
			}

			if !cut { // the list ends the line: drop its own closing bracket only
				checks = strings.TrimSuffix(checks, "]")
			}
		}

		return Fail, failVerdict(checks)
	default:
		// Cut by length, without checking the text: resultRe found the line by its "RESULT" start.
		detail := ""
		if len(result) >= len(resultPrefix) {
			detail = result[len(resultPrefix):]
		}

		return Inconclusive, inconclusiveVerdict(detail)
	}
}

// Retried returns the canary's retried:[…] list (stages whose first attempt missed only timing checks and were run
// again at the end of the same launch), or "".
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

// RetriedPassTTL is how long a retried pass is cached. scripts/verify.sh applies the same rule.
//
// The canary's in-process retry of a photo's frame-gap check runs about 13 s later, in the same, already warm process.
// So it can't tell a post-boot stall, which is what it is for, from a regression that only hits the first photo of a
// process. Both show as "-> retry ok".
//
// A plain pass is cached until the key changes. A retried pass is run again by the first verify or `up` after an hour.
// That bounds how long such a regression can hide behind one cached verdict. It also spares every `up` of a burst,
// such as a CI matrix or a morning of rebuilds, a 20-25 s canary run when a healthy engine needed the retry once.
const RetriedPassTTL = time.Hour

// RetriedPassTTLText is RetriedPassTTL for messages ("1 h").
func RetriedPassTTLText() string {
	if RetriedPassTTL%time.Hour == 0 {
		return fmt.Sprintf("%d h", RetriedPassTTL/time.Hour)
	}
	return fmt.Sprintf("%d min", RetriedPassTTL/time.Minute)
}

// RetriedPass says whether a verdict is a pass that needed the canary's frame-gap retry, a whole relaunch, or a launch
// to confirm a content fail that then passed: such a pass is cached for RetriedPassTTL only.
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
			Note: "the first launch failed (" + trim(firstVerdict, noteLen) +
				"); the launch run to confirm it passed",
		}
	default: // inconclusive, or a crash: no second opinion
		return confirmation{
			Decisive: first,
			NoStore:  true,
			Note: "not cached: the launch run to confirm this fail gave no verdict (" +
				trim(secondVerdict, noteLen) + ")",
		}
	}
}

// launches is how decide reaches the device. Tests use fakes.
//   - run launches the canary once and returns what it came to. n is the log suffix: 1, 1r, 2 or c.
//   - reboot re-arms and reboots after refused launches.
//   - redo reinstalls and re-grants after a launch that didn't start, when the install was skipped.
type launches struct {
	run    func(n string) canaryRun
	reboot func() error
	redo   func(ran bool) (bool, error)
}

// decide runs the canary through runner until it has a verdict. That can take a relaunch after an INCONCLUSIVE or a
// crash, or a confirming launch after a content fail.
// It fills in result's verdict, times and notes, and caches a pass or fail. runStart is when Run started.
// Two crashes in a row are a fail. Refused launches, and a canary with no RESULT line in time, are inconclusive, which
// is never cached.
func decide(result *Result, runner launches, runStart time.Time) {
	canaryStart := time.Now()
	latest := runner.run("1")
	if latest.refused() {
		result.Notes = append(result.Notes, "every launch refused this boot; rebooted once")
		if err := runner.reboot(); err != nil {
			result.State = Inconclusive
			result.Verdict = "inconclusive(reboot after refused launches: " + err.Error() + ")"
			return
		}

		latest = runner.run("1r")
	}

	if state, _ := latest.verdict(); state == Inconclusive || latest.crashed() {
		result.Notes = append(result.Notes, "first run: "+trim(latest.resultLine(), noteLen))
		if redone, err := runner.redo(latest.ran); err != nil {
			result.State = Inconclusive
			result.Verdict = "inconclusive(" + err.Error() + ")"
			return
		} else if redone {
			result.Notes = append(result.Notes,
				"the canary did not start with the install or grant skipped: reinstalled and granted")
		}

		second := runner.run("2")
		result.Relaunched = trim(latest.resultLine(), recordLen)
		latest = finalRun(latest, second)
	}

	confirmed := confirmFail(latest, func() canaryRun {
		mark("confirm-fail")
		return runner.run("c")
	})

	if confirmed.Note != "" {
		result.Notes = append(result.Notes, confirmed.Note)
	}

	latest = confirmed.Decisive
	result.FailedFirst = confirmed.FailedFirst
	mark("canary-done")
	result.CanarySeconds = int(time.Since(canaryStart).Seconds())
	result.Times = fmt.Sprintf("create=%ds boot=%ds canary=%ds total=%ds",
		result.CreateSeconds, result.BootSeconds, result.CanarySeconds, int(time.Since(runStart).Seconds()))
	result.CanaryResult = latest.resultLine()
	result.State, result.Verdict = latest.verdict()
	if result.Retried = latest.retried(); result.Retried != "" {
		result.Notes = append(result.Notes, "canary stage retried: "+trim(result.Retried, recordLen))
	}

	if result.NoCache == "" && !confirmed.NoStore {
		if err := store(*result); err != nil {
			result.Notes = append(result.Notes, "could not cache the verdict: "+err.Error())
		}
	}
}

func logPath(key, runID, n string) string {
	return filepath.Join(home.Logs(), "verify-"+key+"-"+runID+"-"+n+".log")
}
