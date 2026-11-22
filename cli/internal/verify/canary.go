// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// resultRe finds the canary's RESULT line in its console.
var resultRe = regexp.MustCompile(`RESULT[^\r\n]*`)

// runCanary's tries and pauses.
const (
	launchTries     = 3                       // a launch refused as Busy is tried again, twice
	refusedPause    = 2 * time.Second         // between refused tries
	consolePoll     = 100 * time.Millisecond  // how often the console is read
	backgroundPause = 1200 * time.Millisecond // in Settings before the canary is brought back (BG-REQUEST)
	consoleFlush    = 300 * time.Millisecond  // after the RESULT line or the exit, for the console to flush
)

// runCanary launches the canary with its console attached and waits for its RESULT line (canaryResultWait). On
// BG-REQUEST it sends the app to the background (Settings) and back, as scripts/run.sh does. A launch refused as "Busy"
// is tried launchTries times, refusedPause apart. The console goes to logFile.
func runCanary(run *running, udid, logFile string) canaryRun {
	ctl := run.ctl
	var consoleBuf *bytes.Buffer
	defer func() {
		if consoleBuf != nil {
			_ = os.MkdirAll(filepath.Dir(logFile), 0o755) // best effort: the console log is evidence, not the verdict
			_ = os.WriteFile(logFile, []byte(simctl.Snapshot(consoleBuf)), 0o644)
		}
	}()

	for try := 1; ; try++ {
		mark("launch try=%d log=%s", try, filepath.Base(logFile))
		cmd, output, err := ctl.LaunchConsole(udid, CanaryBundle, nil)
		if err != nil {
			return canaryRun{outcome: launchFailed, detail: err.Error()}
		}

		consoleBuf = output
		run.mu.Lock()
		run.cmd = cmd
		run.mu.Unlock()
		done := make(chan struct{})
		go func() {
			cmd.Wait() // its exit only ends the wait: the verdict comes from the console
			close(done)
		}()

		exited := waitForResult(ctl, udid, consoleBuf, done)
		time.Sleep(consoleFlush)
		stopCanary(run, udid, cmd, done)
		console := simctl.Snapshot(consoleBuf)
		if m := resultRe.FindString(console); m != "" {
			return canaryRun{outcome: gotResult, line: strings.TrimRight(m, "\r"), ran: true}
		}

		if launchWasRefused(console) {
			if try == launchTries {
				return canaryRun{outcome: launchRefused}
			}

			_ = os.WriteFile(logFile+".refused-"+strconv.Itoa(try), []byte(console), 0o644) // evidence only
			time.Sleep(refusedPause)
			continue
		}

		return noVerdict(console, exited)
	}
}

// waitForResult polls the console until it shows a RESULT line, the canary exits (done closes: exited) or
// canaryResultWait has passed. It answers the canary's BG-REQUEST once.
func waitForResult(ctl simctl.Ctl, udid string, consoleBuf *bytes.Buffer, done <-chan struct{}) (exited bool) {
	backgrounded, seen := false, false
	deadline := time.Now().Add(canaryResultWait)
	for time.Now().Before(deadline) {
		select {
		case <-done:
			return true
		case <-time.After(consolePoll):
		}

		output := simctl.Snapshot(consoleBuf)
		if !seen && strings.Contains(output, "CANARY") {
			seen = true
			mark("first-canary-output")
		}

		if resultRe.MatchString(output) {
			mark("result-seen")
			return false
		}

		if !backgrounded && strings.Contains(output, "BG-REQUEST") {
			mark("bg-request")
			backgrounded = true
			_ = ctl.Launch(udid, "com.apple.Preferences") // a trip that fails shows in the canary's result
			time.Sleep(backgroundPause)
			_ = ctl.Launch(udid, CanaryBundle)
		}
	}

	return false
}

// stopCanary ends a launch: its console command, the canary and Settings.
func stopCanary(run *running, udid string, cmd *exec.Cmd, done <-chan struct{}) {
	if cmd.Process != nil {
		_ = cmd.Process.Signal(syscall.SIGTERM) // it may have exited already
	}

	run.mu.Lock()
	run.cmd = nil
	run.mu.Unlock()
	go func() {
		<-done
		simctl.Forget(cmd)
	}()

	_ = run.ctl.Terminate(udid, CanaryBundle) // fails if it is not running, which is fine
	_ = run.ctl.Terminate(udid, "com.apple.Preferences")
	mark("terminated")
}

// launchWasRefused reports whether simctl refused the launch (Busy, failed preflight) before the canary printed
// anything.
func launchWasRefused(console string) bool {
	refused := strings.Contains(console, "Busy") || strings.Contains(console, "failed preflight")
	return refused && !strings.Contains(console, "CANARY")
}

// noVerdict is a launch whose console has no RESULT line: a crash if the canary ran and then exited, else no verdict in
// time. Either quotes the console's last CANARY, reason: or failed-to-launch line.
func noVerdict(console string, exited bool) canaryRun {
	last := ""
	for _, line := range strings.Split(console, "\n") {
		if strings.Contains(line, "CANARY") || strings.Contains(line, "reason:") ||
			strings.Contains(line, "failed to launch") {
			last = strings.TrimSpace(line)
		}
	}

	ran := strings.Contains(console, "CANARY")
	if exited && ran {
		return canaryRun{outcome: exitedNoVerdict, detail: trim(last, lastLineLen), ran: true}
	}

	return canaryRun{outcome: noVerdictInTime, detail: trim(last, lastLineLen), ran: ran}
}
