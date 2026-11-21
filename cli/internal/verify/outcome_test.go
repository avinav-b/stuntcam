// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// Characterisation tests: what verify prints, returns and caches for each kind of canary launch, compared byte for byte
// with testdata/*.golden. `go test ./internal/verify -run Golden -update` rewrites the golden files.

var update = flag.Bool("update", false, "rewrite testdata/*.golden")

func checkGolden(t *testing.T, name, got string) {
	t.Helper()
	path := filepath.Join("testdata", name)
	if *update {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got != string(want) {
		gl, wl := strings.Split(got, "\n"), strings.Split(string(want), "\n")
		for i := 0; i < len(gl) || i < len(wl); i++ {
			g, w := "", ""
			if i < len(gl) {
				g = gl[i]
			}
			if i < len(wl) {
				w = wl[i]
			}
			if g != w {
				t.Fatalf("%s differs at line %d:\n got  %q\n want %q", path, i+1, g, w)
			}
		}
	}
}

// launch outcomes as runCanary returns them
var (
	goldPass    = printed("RESULT core=PASS ext=PASS t=13.4s")
	goldRetried = printed("RESULT core=PASS ext=PASS t=16.7s retried:[photo-4x3: longest frame gap 215 ms -> retry ok]")
	goldFail    = printed("RESULT core=FAIL ext=PASS t=13.2s core:[photo-then-stop: didFinishCaptureFor before didStopRunning]")
	goldFail2   = printed("RESULT core=FAIL ext=PASS t=13.3s core:[photo-jpeg: fileDataRepresentation decodes] ext:[x]")
	goldInc     = printed("RESULT core=INCONCLUSIVE ext=PASS t=18.0s timing:[frames: A frames=4 (>= 12 in window)]")
	goldWatch   = printed("RESULT core=INCONCLUSIVE ext=INCONCLUSIVE watchdog stage=movie after 45s")
	goldLong    = printed("RESULT core=FAIL ext=PASS t=13.2s core:[" + strings.Repeat("photo-jpeg: a long check text; ", 14) + "]")
	goldCrash   = crashLaunch("CANARY| 6.1 ok x")
	goldCrashL  = crashLaunch("CANARY| " + strings.Repeat("y", 112))
	goldTimeout = timeoutLaunch("CANARY| photo-4x3 start", true)
	goldRefused = canaryRun{outcome: launchRefused}
	goldNoStart = canaryRun{outcome: launchFailed, detail: `exec: "xcrun": executable file not found in $PATH`}
)

func crashLaunch(last string) canaryRun {
	return canaryRun{outcome: exitedNoVerdict, detail: trim(last, lastLineLen), ran: true}
}

func timeoutLaunch(last string, ran bool) canaryRun {
	return canaryRun{outcome: noVerdictInTime, detail: trim(last, lastLineLen), ran: ran}
}

func TestDecideGolden(t *testing.T) {
	var out strings.Builder
	for _, c := range []struct {
		name      string
		launches  []canaryRun
		rebootErr error
		redo      bool
		redoErr   error
		noCache   string
	}{
		{name: "pass", launches: []canaryRun{goldPass}},
		{name: "retried pass", launches: []canaryRun{goldRetried}},
		{name: "fail confirmed", launches: []canaryRun{goldFail, goldFail}},
		{name: "fail confirmed by another fail", launches: []canaryRun{goldFail, goldFail2}},
		{name: "fail then pass", launches: []canaryRun{goldFail, goldPass}},
		{name: "fail then inconclusive", launches: []canaryRun{goldFail, goldInc}},
		{name: "fail then crash", launches: []canaryRun{goldFail, goldCrash}},
		{name: "long fail then pass", launches: []canaryRun{goldLong, goldPass}},
		{name: "long fail confirmed", launches: []canaryRun{goldLong, goldLong}},
		{name: "inconclusive then pass", launches: []canaryRun{goldInc, goldPass}},
		{name: "inconclusive then retried pass", launches: []canaryRun{goldInc, goldRetried}},
		{name: "inconclusive then crash", launches: []canaryRun{goldInc, goldCrash}},
		{name: "inconclusive twice", launches: []canaryRun{goldInc, goldWatch}},
		{name: "inconclusive, fail, fail", launches: []canaryRun{goldInc, goldFail, goldFail2}},
		{name: "inconclusive, fail, pass", launches: []canaryRun{goldInc, goldFail, goldPass}},
		{name: "crash then crash", launches: []canaryRun{goldCrash, goldCrashL}},
		{name: "crash then pass", launches: []canaryRun{goldCrash, goldPass}},
		{name: "crash then inconclusive", launches: []canaryRun{goldCrashL, goldInc}},
		{name: "timeout twice", launches: []canaryRun{goldTimeout, timeoutLaunch("", false)}},
		{name: "timeout then fail then fail", launches: []canaryRun{goldTimeout, goldFail, goldFail}},
		{name: "refused, reboot, pass", launches: []canaryRun{goldRefused, goldPass}},
		{name: "refused, reboot, refused, pass", launches: []canaryRun{goldRefused, goldRefused, goldPass}},
		{name: "refused, reboot fails", launches: []canaryRun{goldRefused}, rebootErr: fmt.Errorf("simctl boot U: exit status 149")},
		{name: "not started, redo, pass", launches: []canaryRun{goldNoStart, goldPass}, redo: true},
		{name: "not started, redo fails", launches: []canaryRun{goldNoStart}, redoErr: fmt.Errorf("install failed: x")},
		{name: "cache bypassed", launches: []canaryRun{goldFail, goldPass}, noCache: "SIMCTL_CHILD_X"},
	} {
		t.Setenv("SIMCAM_HOME", t.TempDir())
		var logs []string
		var ran []bool
		n := 0
		l := launches{
			run: func(suffix string) canaryRun {
				logs = append(logs, suffix)
				if n++; n > len(c.launches) {
					t.Fatalf("%s: launch %d (%s): only %d expected", c.name, n, suffix, len(c.launches))
				}
				return c.launches[n-1]
			},
			reboot: func() error { logs = append(logs, "reboot"); return c.rebootErr },
			redo: func(r bool) (bool, error) {
				ran = append(ran, r)
				return c.redo, c.redoErr
			},
		}
		r := Result{Key: "k", At: 1, NoCache: c.noCache, Notes: []string{"canary /x"}}
		decide(&r, l, time.Now())
		b, _ := json.MarshalIndent(r, "", " ")
		fmt.Fprintf(&out, "== %s\nlaunches: %s\nredo ran: %v\nstate=%s cached=%v\n%s\n", c.name, strings.Join(logs, " "), ran, r.State, r.Cached, b)
		if f, err := os.ReadFile(cachePath("k")); err == nil {
			fmt.Fprintf(&out, "cache file:\n%s\n", f)
		} else {
			fmt.Fprintf(&out, "cache file: none\n")
		}
	}
	checkGolden(t, "decide.golden", out.String())
}

// fakeCanaryXcrun puts an xcrun on PATH whose `launch --console-pty` runs <dir>/console (a scenario script) and whose other
// calls are logged to <dir>/calls; `launch U <canary>` (the return from the background) creates <dir>/fg.
func fakeCanaryXcrun(t *testing.T) string {
	dir := t.TempDir()
	script := `#!/bin/sh
d="$FAKE_CANARY_DIR"
echo "$*" | sed "s|$d|SET|" >> "$d/calls"
case "$*" in
*" launch --console-pty "*) exec sh "$d/console";;
*" launch U ` + CanaryBundle + `") touch "$d/fg"; exit 0;;
*" launch U "*|*" terminate U "*) exit 0;;
esac
exit 3
`
	if err := os.WriteFile(filepath.Join(dir, "xcrun"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("FAKE_CANARY_DIR", dir)
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	return dir
}

func TestRunCanaryGolden(t *testing.T) {
	old := canaryResultWait
	canaryResultWait = 1500 * time.Millisecond
	defer func() { canaryResultWait = old }()
	var out strings.Builder
	for _, c := range []struct{ name, console string }{
		{"result", "echo 'CANARY| start'; echo 'RESULT core=PASS ext=PASS t=1.0s'; exec sleep 5"},
		{"result with CR", `printf 'CANARY| a\r\nRESULT core=FAIL ext=PASS t=2s core:[qr: x]\r\n'; exec sleep 5`},
		{"crash", "echo 'CANARY| 6.1 ok x'; echo 'some other line'; exit 1"},
		{"crash with a reason", "echo 'CANARY| 6.1 ok x'; echo '  reason: SIGSEGV  '; exit 1"},
		{"crash, long last line", "echo 'CANARY| " + strings.Repeat("z", 150) + "'; exit 1"},
		{"exits without starting", "echo 'failed to launch: x'; exit 1"},
		{"no verdict in time", "echo 'CANARY| photo-4x3 start'; exec sleep 10"},
		{"no output in time", "echo 'hello'; exec sleep 10"},
		{"refused every time", "echo 'An error was encountered processing the command (code=1): Busy'; exit 1"},
		{"failed preflight then result", `n=$(cat "$FAKE_CANARY_DIR/n" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$FAKE_CANARY_DIR/n"
if [ $n -lt 2 ]; then echo 'failed preflight'; exit 1; fi
echo 'CANARY| go'; echo 'RESULT core=PASS ext=PASS t=3s'; exec sleep 5`},
		{"background request", `echo 'CANARY| bg'; echo 'BG-REQUEST'
while [ ! -f "$FAKE_CANARY_DIR/fg" ]; do sleep 0.05; done
echo 'RESULT core=PASS ext=PASS t=4s'; exec sleep 5`},
	} {
		dir := fakeCanaryXcrun(t)
		if err := os.WriteFile(filepath.Join(dir, "console"), []byte(c.console+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		logFile := filepath.Join(dir, "logs", "run.log")
		run := &running{ctl: simctl.Ctl{Set: dir}}
		res := runCanary(run, "U", logFile)
		fmt.Fprintf(&out, "== %s\nresult: %q\nrefused=%v ran=%v\n", c.name, res.resultLine(), res.refused(), res.ran)
		writeRunFiles(&out, dir)
	}
	// simctl can't be started at all
	dir := t.TempDir()
	t.Setenv("PATH", dir)
	res := runCanary(&running{ctl: simctl.Ctl{Set: dir}}, "U", filepath.Join(dir, "logs", "run.log"))
	fmt.Fprintf(&out, "== launch fails\nresult: %q\nrefused=%v ran=%v\n", res.resultLine(), res.refused(), res.ran)
	writeRunFiles(&out, dir)
	checkGolden(t, "runcanary.golden", out.String())
}

func writeRunFiles(out *strings.Builder, dir string) {
	calls, _ := os.ReadFile(filepath.Join(dir, "calls"))
	fmt.Fprintf(out, "calls:\n%s", calls)
	logs, _ := filepath.Glob(filepath.Join(dir, "logs", "*"))
	sort.Strings(logs)
	for _, l := range logs {
		b, _ := os.ReadFile(l)
		fmt.Fprintf(out, "file %s:\n%s\n", filepath.Base(l), b)
	}
}

// the lock's refusal texts end up in the inconclusive verdict
func TestLockTextsGolden(t *testing.T) {
	var out strings.Builder
	say := func(s string) { fmt.Fprintf(&out, "say: %s\n", s) }
	tmp := t.TempDir()
	t.Setenv("SIMCAM_HOME", tmp)
	os.MkdirAll(home.Root(), 0o755)
	d := lockDir("24A434")
	h := holder(t)
	rel := func(s string) string { return strings.ReplaceAll(s, tmp, "HOME") }

	holdLock(t, d, h.Process.Pid, false)
	why := acquireWait(d, time.Second, say).Error()
	fmt.Fprintf(&out, "live owner: false %s\n", rel(strings.ReplaceAll(why, fmt.Sprint(h.Process.Pid), "PID")))
	os.RemoveAll(d)

	os.Mkdir(d, 0o755) // no pid yet, a fresh dir
	why = acquireWait(d, time.Second, say).Error()
	fmt.Fprintf(&out, "no pid: false %s\n", rel(why))
	os.RemoveAll(d)

	holdLock(t, d, h.Process.Pid, true)
	why = acquireWait(d, 500*time.Millisecond, say).Error()
	fmt.Fprintf(&out, "shutting down: false %s\n", rel(why))
	os.RemoveAll(d)

	missing := filepath.Join(tmp, "missing", "verify-X.lock")
	why = acquireWait(missing, time.Second, say).Error()
	fmt.Fprintf(&out, "no home: false %s\n", rel(why))
	checkGolden(t, "lock.golden", out.String())
}

// why a cached entry is not used
func TestCachedTextsGolden(t *testing.T) {
	var out strings.Builder
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Verdicts(), 0o755)
	oldLocal := time.Local
	time.Local = time.UTC // the texts show a local time of day
	defer func() { time.Local = oldLocal }()
	now := time.Unix(1_800_000_000, 0)
	at := now.Add(-2 * time.Hour).Unix()
	put := func(key, body string) { os.WriteFile(filepath.Join(home.Verdicts(), key+".json"), []byte(body), 0o644) }
	put("unreadable", `{"verdict":`)
	put("other", `{"verdict":"maybe"}`)
	put("inc", `{"verdict":"inconclusive(x)"}`)
	put("retried", fmt.Sprintf(`{"verdict":"pass","retried":"r","at":%d}`, at))
	put("relaunched", fmt.Sprintf(`{"verdict":"pass","relaunched":"RESULT x","at":%d}`, at))
	put("failedfirst", fmt.Sprintf(`{"verdict":"pass","failed_first":"RESULT y","at":%d}`, at))
	put("shell", fmt.Sprintf(`{"verdict":"pass","result":"RESULT core=PASS retried:[a -> retry ok]","at":%d}`, at))
	put("fail", fmt.Sprintf(`{"verdict":"fail(x)","retried":"r","at":%d}`, at))
	shipped := t.TempDir()
	os.WriteFile(filepath.Join(shipped, "retried.json"), []byte(`{"key":"retried","verdict":"pass"}`), 0o644)
	os.WriteFile(filepath.Join(shipped, "plain.json"), []byte(`{"key":"plain","verdict":"pass","at":5}`), 0o644)
	for _, k := range []string{"none", "unreadable", "other", "inc", "retried", "relaunched", "failedfirst", "shell", "fail"} {
		c := cached(k, now)
		fmt.Fprintf(&out, "cached %s: ok=%v present=%v state=%s retried=%q why=%q\n",
			k, c.usable, c.present, c.result.State, c.result.Retried, c.why)
	}
	for _, k := range []string{"retried", "plain", "none"} {
		r, ok, why := lookup(k, shipped, now)
		fmt.Fprintf(&out, "lookup %s: ok=%v shipped=%v cached=%v why=%q\n", k, ok, r.Shipped, r.Cached, why)
	}
	checkGolden(t, "cached.golden", out.String())
}
