// SPDX-License-Identifier: Apache-2.0
// The canary's output: one "CANARY| <seconds> <text>" log line per check and event, and one RESULT line at the end.
//
// The core verdict is one of three:
// - FAIL when any content check failed.
// - INCONCLUSIVE when only timing checks missed (frame windows, fps, first-frame latency, the photo and movie
//   completion deadlines, the frame gap during a photo), or when the budget ran out with no FAIL recorded. The
//   watchdog's exit 3, with nothing failed, is INCONCLUSIVE too.
// - PASS otherwise.
// Late is a timing miss, but never is a FAIL: no frame at all, a photo or movie that never completes, or a photo
// completion without a photo.
//
// scripts/run.sh retries an INCONCLUSIVE once and counts a second one as FAIL, so a real hang still fails a test run
// but a starved machine does not. scripts/verify.sh reports a second one as inconclusive(reason) and never caches it.
// The extended checks (ext) are reported separately and never change the core verdict.
import Foundation

// The verdict so far: the stage the check lines name, and the checks that missed, for the RESULT line.
//
// Threading: logLock guards the lists. It is the lock log() takes, and every method below holds it for one whole
// read-and-update.
// `stage` is not locked. Only the canary's own thread writes it, at the start of each stage. check() reads it, and
// check() may run on other threads.
// The watchdog also reads `stage` from another thread, just before the process exits, so that read can race with a
// stage change. The race is known and accepted. At worst the watchdog's RESULT line names the wrong stage.
final class Verdict {
  var stage = "init"
  // failed checks, "<stage>: <message>", in the order they failed
  private var coreFailures: [String] = []
  private var coreTimingMisses: [String] = []
  private var extFailures: [String] = []
  // Photo stages whose only miss was the frame-gap check are run once more at the end (Retry.swift). Each entry is
  // "<first attempt's misses> -> retry ok|SLOW|FAIL".
  private var retried: [String] = []

  // the lists as they are now
  struct Snapshot {
    let core: [String]
    let timing: [String]
    let ext: [String]
    let retried: [String]
  }

  func snapshot() -> Snapshot {
    logLock.withLock {
      Snapshot(core: coreFailures, timing: coreTimingMisses, ext: extFailures, retried: retried)
    }
  }

  func record(_ entry: String, timing: Bool, ext: Bool) {
    logLock.withLock {
      if ext {
        extFailures.append(entry)
      } else if timing {
        coreTimingMisses.append(entry)
      } else {
        coreFailures.append(entry)
      }
    }
  }

  // the number of core failures and timing misses so far: a stage compares them before and after it runs
  func coreCounts() -> (failures: Int, timing: Int) {
    logLock.withLock { (coreFailures.count, coreTimingMisses.count) }
  }

  // Whether, since `before`, no core check failed and every timing miss satisfies `allowed` (true when there were no
  // misses at all).
  func hasOnlyMisses(since before: (failures: Int, timing: Int), matching allowed: (String) -> Bool) -> Bool {
    logLock.withLock {
      coreFailures.count == before.failures && coreTimingMisses[before.timing...].allSatisfy(allowed)
    }
  }

  // The timing misses since `before`. If no core check failed and every one satisfies `retryable`, they are taken back
  // out of the verdict (withdrawn is then true), for a retry to decide.
  func withdrawRetryableMisses(since before: (failures: Int, timing: Int),
                               where retryable: (String) -> Bool) -> (misses: [String], withdrawn: Bool) {
    logLock.withLock {
      let failed = coreFailures.count > before.failures
      let misses = Array(coreTimingMisses[before.timing...])
      let withdrawn = !failed && !misses.isEmpty && misses.allSatisfy(retryable)
      if withdrawn {
        coreTimingMisses.removeSubrange(before.timing...)
      }

      return (misses, withdrawn)
    }
  }

  // Records how a retry went (FAIL, SLOW or ok, judged by what it added since `before`) next to the first attempt's
  // misses, and returns the outcome.
  func recordRetry(firstMisses: [String], since before: (failures: Int, timing: Int)) -> String {
    logLock.withLock {
      let outcome: String
      if coreFailures.count > before.failures {
        outcome = "FAIL"
      } else if coreTimingMisses.count > before.timing {
        outcome = "SLOW"
      } else {
        outcome = "ok"
      }

      retried.append("\(firstMisses.joined(separator: "; ")) -> retry \(outcome)")
      return outcome
    }
  }
}

let verdict = Verdict()
let logLock = NSLock()

func log(_ line: String) {
  logLock.withLock {
    print(String(format: "CANARY| %6.3f ", Date().timeIntervalSince(launchTime)) + line)
    fflush(stdout)
  }
}

// One check line: "ok   ", "SLOW " (a timing miss) or "FAIL ", then "[ext] " for an extended check, then
// "<stage>: <message>". A miss is also recorded for the RESULT line.
func check(_ passed: Bool, timing: Bool = false, _ what: @autoclosure () -> String, ext: Bool = false) {
  let message = what()
  if !passed {
    verdict.record("\(verdict.stage): \(message)", timing: timing, ext: ext)
  }
  log(checkPrefix(passed: passed, timing: timing) + (ext ? "[ext] " : "") + "\(verdict.stage): \(message)")
}

func checkPrefix(passed: Bool, timing: Bool) -> String {
  if passed {
    return "ok   "
  }
  return timing ? "SLOW " : "FAIL "
}

// Logs the RESULT line of a finished run and exits: 0 for PASS, 1 for FAIL, 4 for INCONCLUSIVE.
func finish() -> Never {
  let lists = verdict.snapshot()
  let core = lists.core
  let timing = lists.timing
  let ext = lists.ext
  let coreResult = !core.isEmpty ? "FAIL" : timing.isEmpty ? "PASS" : "INCONCLUSIVE"
  let extResult = ext.isEmpty ? "PASS" : "FAIL"
  log("RESULT core=\(coreResult) ext=\(extResult) t=\(String(format: "%.1f", Date().timeIntervalSince(launchTime)))s"
      + retriedField(lists.retried)
      + listField("core", core)
      + listField("timing", timing)
      + listField("ext", ext))
  exit(!core.isEmpty ? 1 : timing.isEmpty ? 0 : 4)
}

// The RESULT line when the budget runs out first, then exit 3, or 1 if a check failed: a content FAIL recorded before
// the budget ran out stays a FAIL, and only a run with none is INCONCLUSIVE.
func watchdogExit() -> Never {
  let lists = verdict.snapshot()
  let core = lists.core
  let ext = lists.ext
  let coreResult = core.isEmpty ? "INCONCLUSIVE" : "FAIL"
  let extResult = ext.isEmpty ? "INCONCLUSIVE" : "FAIL"
  log("RESULT core=\(coreResult) ext=\(extResult) watchdog stage=\(verdict.stage) after \(config.budget)s"
      + retriedField(lists.retried)
      + listField("core", core)
      + listField("ext", ext))
  exit(core.isEmpty ? 3 : 1)
}

// " <name>:[<entry>; <entry>]", or nothing for an empty list
func listField(_ name: String, _ entries: [String]) -> String {
  entries.isEmpty ? "" : " \(name):[\(entries.joined(separator: "; "))]"
}

// It comes before core:[ so that the verdict parsers' core:[…] cut (scripts/verify.sh, the CLI's Classify) is
// unchanged.
func retriedField(_ retried: [String]) -> String {
  listField("retried", retried)
}
