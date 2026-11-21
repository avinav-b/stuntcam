// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"strconv"
	"time"
)

// canaryRun is what one launch of the canary came to (runCanary). Only a RESULT line the canary printed is parsed
// (Classify); every other outcome carries its own state.
type canaryRun struct {
	outcome canaryOutcome
	line    string // gotResult: the canary's RESULT line
	detail  string // launchFailed: the error; exitedNoVerdict and noVerdictInTime: the console's last line (lastLineLen)
	ran     bool   // the canary printed a CANARY line: it started
}

type canaryOutcome int

const (
	gotResult       canaryOutcome = iota // the canary printed its RESULT line
	launchFailed                         // simctl couldn't launch it
	launchRefused                        // every try was refused (Busy, failed preflight)
	exitedNoVerdict                      // it ran, then exited without a RESULT line: a crash
	noVerdictInTime                      // no RESULT line within canaryResultWait, or it exited without starting
	crashedOnce                          // finalRun: one crash after an inconclusive first launch
)

// crashText names a crash in its verdict: fail(canary exited without a verdict (crash); last: …).
const crashText = "canary exited without a verdict (crash)"

// Byte limits of what a verdict and its notes quote.
const (
	failChecksLen   = 294 // the core:[…] list in fail(…) (scripts/verify.sh: cut -c7-300 of "core:[…")
	inconclusiveLen = 193 // the RESULT line after "RESULT " in inconclusive(…)
	noteLen         = 160 // a RESULT line or a verdict quoted in Result.Notes
	recordLen       = 300 // an earlier launch's RESULT line in Relaunched or FailedFirst; the retried list in the notes
	lastLineLen     = 120 // the console's last line quoted for a launch without a verdict
)

func (c canaryRun) refused() bool { return c.outcome == launchRefused }

func (c canaryRun) crashed() bool { return c.outcome == exitedNoVerdict }

// contentFail says whether the run is a content fail: core=FAIL from checks that ran, not a canary that exited without a
// verdict (a crash, which already needs two in a row).
func (c canaryRun) contentFail() bool {
	st, _ := c.verdict()
	return st == Fail && !c.crashed()
}

// verdict is the run's state and verdict text, with scripts/verify.sh's text rules.
func (c canaryRun) verdict() (State, string) {
	switch c.outcome {
	case gotResult:
		return Classify(c.line)
	case exitedNoVerdict:
		return Fail, failVerdict(c.crashChecks())
	default:
		return Inconclusive, inconclusiveVerdict(inconclusiveCore(c.inconclusiveWhy()))
	}
}

// resultLine is the RESULT line recorded for the run (Result.CanaryResult, Relaunched, FailedFirst, the notes): the
// canary's own, else a stand-in in the same format for a launch that printed none.
func (c canaryRun) resultLine() string {
	switch c.outcome {
	case gotResult:
		return c.line
	case exitedNoVerdict:
		return "RESULT core=FAIL ext=FAIL core:[" + c.crashChecks() + "]"
	default:
		return "RESULT " + inconclusiveCore(c.inconclusiveWhy())
	}
}

// retried is the canary's retried:[…] list, "" for a launch without a RESULT line.
func (c canaryRun) retried() string {
	if c.outcome != gotResult {
		return ""
	}
	return Retried(c.line)
}

// crashChecks stands in for the canary's core:[…] list when it crashed.
func (c canaryRun) crashChecks() string { return crashText + "; last: " + c.detail }

// inconclusiveWhy says why a launch without a RESULT line, other than a crash, is inconclusive.
func (c canaryRun) inconclusiveWhy() string {
	switch c.outcome {
	case launchFailed:
		return "launch failed: " + c.detail
	case launchRefused:
		return "every launch refused: Busy"
	case noVerdictInTime:
		return "no verdict in " + strconv.Itoa(int(canaryResultWait/time.Second)) + " s; last: " + c.detail
	case crashedOnce:
		return "the canary crashed once, after an inconclusive first run; not cached"
	}
	return ""
}

func inconclusiveCore(why string) string { return "core=INCONCLUSIVE (" + why + ")" }

func failVerdict(checks string) string { return "fail(" + trim(checks, failChecksLen) + ")" }

func inconclusiveVerdict(rest string) string {
	return "inconclusive(" + trim(rest, inconclusiveLen) + ")"
}

// finalRun is the outcome after a second launch: the second one, except that fail(crash) needs two crashes in a row (a
// single crash after an inconclusive first launch is still inconclusive, never cached), as in scripts/verify.sh.
func finalRun(first, second canaryRun) canaryRun {
	if second.crashed() && !first.crashed() {
		return canaryRun{outcome: crashedOnce}
	}
	return second
}
