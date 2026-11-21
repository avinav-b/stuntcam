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

// canaryResultWait is how long runCanary waits for the RESULT line: the canary's own budget is 45 s. A var for tests.
// The no-verdict text is built from it ("no verdict in 60 s"); scripts/verify.sh spells the same text out, and the two
// share the verdict cache, so change both together.
var canaryResultWait = 60 * time.Second

// runCanary's tries and pauses.
const (
	launchTries     = 3                       // a launch refused as Busy is tried again, twice
	refusedPause    = 2 * time.Second         // between refused tries
	consolePoll     = 100 * time.Millisecond  // how often the console is read
	backgroundPause = 1200 * time.Millisecond // in Settings before the canary is brought back (BG-REQUEST)
	consoleFlush    = 300 * time.Millisecond  // after the RESULT line or the exit, for the console to flush
)

// runCanary launches the canary with its console attached and waits for its RESULT line (canaryResultWait). On
// BG-REQUEST it sends the app to the background (Settings) and back, as scripts/run.sh does. A launch refused as "Busy" is
// tried launchTries times, refusedPause apart. The console goes to logFile.
func runCanary(run *running, udid, logFile string) canaryRun {
	ctl := run.ctl
	var buf *bytes.Buffer
	defer func() {
		if buf != nil {
			_ = os.MkdirAll(filepath.Dir(logFile), 0o755)
			_ = os.WriteFile(logFile, []byte(simctl.Snapshot(buf)), 0o644)
		}
	}()
	for try := 1; ; try++ {
		mark("launch try=%d log=%s", try, filepath.Base(logFile))
		cmd, b, err := ctl.LaunchConsole(udid, CanaryBundle, nil)
		if err != nil {
			return canaryRun{outcome: launchFailed, detail: err.Error()}
		}
		buf = b
		run.mu.Lock()
		run.cmd = cmd
		run.mu.Unlock()
		done := make(chan struct{})
		go func() { cmd.Wait(); close(done) }()
		exited := waitForResult(ctl, udid, buf, done)
		time.Sleep(consoleFlush)
		stopCanary(run, udid, cmd, done)
		console := simctl.Snapshot(buf)
		if m := resultRe.FindString(console); m != "" {
			return canaryRun{outcome: gotResult, line: strings.TrimRight(m, "\r"), ran: true}
		}
		if launchWasRefused(console) {
			if try == launchTries {
				return canaryRun{outcome: launchRefused}
			}
			_ = os.WriteFile(logFile+".refused-"+strconv.Itoa(try), []byte(console), 0o644)
			time.Sleep(refusedPause)
			continue
		}
		return noVerdict(console, exited)
	}
}

// waitForResult polls the console until it shows a RESULT line, the canary exits (done closes: exited) or
// canaryResultWait has passed. It answers the canary's BG-REQUEST once.
func waitForResult(ctl simctl.Ctl, udid string, buf *bytes.Buffer, done <-chan struct{}) (exited bool) {
	bg, seen := false, false
	deadline := time.Now().Add(canaryResultWait)
	for time.Now().Before(deadline) {
		select {
		case <-done:
			return true
		case <-time.After(consolePoll):
		}
		s := simctl.Snapshot(buf)
		if !seen && strings.Contains(s, "CANARY") {
			seen = true
			mark("first-canary-output")
		}
		if resultRe.MatchString(s) {
			mark("result-seen")
			return false
		}
		if !bg && strings.Contains(s, "BG-REQUEST") {
			mark("bg-request")
			bg = true
			_ = ctl.Launch(udid, "com.apple.Preferences")
			time.Sleep(backgroundPause)
			_ = ctl.Launch(udid, CanaryBundle)
		}
	}
	return false
}

// stopCanary ends a launch: its console command, the canary and Settings.
func stopCanary(run *running, udid string, cmd *exec.Cmd, done <-chan struct{}) {
	if cmd.Process != nil {
		_ = cmd.Process.Signal(syscall.SIGTERM)
	}
	run.mu.Lock()
	run.cmd = nil
	run.mu.Unlock()
	go func() { <-done; simctl.Forget(cmd) }()
	_ = run.ctl.Terminate(udid, CanaryBundle)
	_ = run.ctl.Terminate(udid, "com.apple.Preferences")
	mark("terminated")
}

// launchWasRefused: simctl refused the launch (Busy, failed preflight) before the canary printed anything.
func launchWasRefused(console string) bool {
	refused := strings.Contains(console, "Busy") || strings.Contains(console, "failed preflight")
	return refused && !strings.Contains(console, "CANARY")
}

// noVerdict is a launch whose console has no RESULT line: a crash if the canary ran and then exited, else no verdict in
// time. Either quotes the console's last CANARY, reason: or failed-to-launch line.
func noVerdict(console string, exited bool) canaryRun {
	last := ""
	for _, l := range strings.Split(console, "\n") {
		if strings.Contains(l, "CANARY") || strings.Contains(l, "reason:") || strings.Contains(l, "failed to launch") {
			last = strings.TrimSpace(l)
		}
	}
	ran := strings.Contains(console, "CANARY")
	if exited && ran {
		return canaryRun{outcome: exitedNoVerdict, detail: trim(last, lastLineLen), ran: true}
	}
	return canaryRun{outcome: noVerdictInTime, detail: trim(last, lastLineLen), ran: ran}
}
