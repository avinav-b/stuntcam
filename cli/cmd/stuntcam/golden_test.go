// SPDX-License-Identifier: Apache-2.0
package main

// Characterisation tests: the exact stdout, stderr and exit code of `stuntcam` for a set of scenarios, the simctl calls
// each one makes (and the host launchctl calls, where there are any) and the files it leaves in the stuntcam home.
// Every scenario runs main() in a child process (this test binary, re-executed with goldenChildEnv set) against fake
// xcrun, xcodebuild, xcode-select, codesign, ps, launchctl…
// scripts and a fake build, in a throwaway world: HOME, SIMCAM_HOME and the working directory are temp dirs.
// testdata/golden.txt holds the expected output; `go test -run TestGolden -update` rewrites it.

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/xcode"
)

var updateGolden = flag.Bool("update", false, "rewrite testdata/golden.txt from the current output")

const (
	goldenChildEnv = "STUNTCAM_GOLDEN_CHILD"
	goldenFile     = "testdata/golden.txt"
)

// closedStdoutEnv set to 1 makes the child close its stdout before main(): every write to it fails.
const closedStdoutEnv = "STUNTCAM_GOLDEN_CLOSED_STDOUT"

func TestMain(m *testing.M) {
	if os.Getenv(goldenChildEnv) == "1" {
		goldenChild()
		return
	}
	os.Exit(m.Run())
}

// goldenChild runs main() with this process's own arguments (so a stuntcam that launches itself, as verify's shutdown
// handoff does, runs what it asked for). A simulator's boot environment (launchd_sim's, which the real code reads with
// ps and sysctl) comes from $FAKE/bootenv/<udid>, one KEY=VALUE per line. The Xcodes installed on the machine running
// the test are not listed: doctor's lines would differ on a Mac that has some.
func goldenChild() {
	fake := os.Getenv("FAKE")
	installedXcodes = func() []xcode.Info { return nil }
	bootEnv = func(udid string) (map[string]string, error) {
		b, err := os.ReadFile(filepath.Join(fake, "bootenv", udid))
		if err != nil {
			return nil, errors.New("no launchd_sim for " + udid)
		}
		env := map[string]string{}
		for _, l := range strings.Split(string(b), "\n") {
			if k, v, ok := strings.Cut(l, "="); ok {
				env[k] = v
			}
		}
		return env, nil
	}
	os.Args = append([]string{"stuntcam"}, os.Args[1:]...)
	if os.Getenv(closedStdoutEnv) == "1" {
		os.Stdout.Close()
	}
	main()
	os.Exit(0)
}

// fake tools on the child's PATH. xcrun simctl answers from files under $FAKE and logs every call to $FAKE/xcrun.log;
// launchctl setenv/unsetenv/getenv inside a simulator keep their values in $FAKE/env/<udid>/<name>.
var fakeTools = map[string]string{
	"xcrun": `#!/bin/sh
[ "$1" = simctl ] && shift
pre=""
if [ "$1" = --set ]; then pre="private-"; shift 2; fi
printf '%s%s\n' "$pre" "$*" >> "$FAKE/xcrun.log"
case "$1" in
list)
	if [ -f "$FAKE/$pre$2.json" ]; then cat "$FAKE/$pre$2.json"; else echo '{}'; fi ;;
spawn)
	u=$2; shift 2
	case "$1 $2" in
	"launchctl getenv") [ -f "$FAKE/env/$u/$3" ] && cat "$FAKE/env/$u/$3" ;;
	"launchctl setenv") mkdir -p "$FAKE/env/$u"; printf '%s\n' "$4" > "$FAKE/env/$u/$3" ;;
	"launchctl unsetenv") rm -f "$FAKE/env/$u/$3" ;;
	"launchctl list") [ -f "$FAKE/launchctl-list" ] && cat "$FAKE/launchctl-list" ;;
	esac ;;
get_app_container)
	if [ -f "$FAKE/container/$2/$3" ]; then cat "$FAKE/container/$2/$3"; else
		printf 'An error was encountered processing the command (domain=NSPOSIXErrorDomain, code=2):\nNo such file or directory\n' >&2
		exit 2
	fi ;;
esac
exit 0
`,
	// -version for xcode.Active; as the command `exec` runs: its arguments, the TEST_RUNNER_/SIMCTL_CHILD_ variables it
	// got, then $FAKE/xcodebuild-run.sh (if any) and exit $FAKE_XB_EXIT
	"xcodebuild": `#!/bin/sh
if [ "$1" = -version ]; then printf 'Xcode 27.0\nBuild version 27A266a\n'; exit 0; fi
echo "xcodebuild $*"
env | grep -E '^(TEST_RUNNER_|SIMCTL_CHILD_)' | sort
[ -f "$FAKE/xcodebuild-run.sh" ] && . "$FAKE/xcodebuild-run.sh"
exit ${FAKE_XB_EXIT:-0}
`,
	"xcode-select": "#!/bin/sh\necho /Applications/Xcode.app/Contents/Developer\n",
	"mdfind":       "#!/bin/sh\nexit 0\n",
	"ps":           "#!/bin/sh\nexit 0\n",
	"osascript":    "#!/bin/sh\nexit 0\n",
	"codesign": "#!/bin/sh\necho \"Executable=$2\" >&2\n" +
		"echo 'CodeDirectory v=20400 size=1 flags=0x20002(adhoc,linker-signed) hashes=1+0' >&2\n",
	"launchctl": "#!/bin/sh\necho \"$*\" >> \"$FAKE/launchctl.log\"\nexit 1\n",
}

// world is one scenario's throwaway machine.
type world struct {
	t                                  *testing.T
	root, fake, bin, home, simHome, wd string
	build                              string // the fake build's loader + engine dir (STUNTCAM_BUILD)
	env                                []string
	now                                time.Time
	devs                               []fakeDev
	privateDevs                        []fakeDev
}

type fakeDev struct {
	udid, name, state, runtime string
	lastBooted                 time.Time
}

const (
	iosRT  = "com.apple.CoreSimulator.SimRuntime.iOS-27-0"
	ios26  = "com.apple.CoreSimulator.SimRuntime.iOS-26-3"
	tvosRT = "com.apple.CoreSimulator.SimRuntime.tvOS-26-0"
	goneRT = "com.apple.CoreSimulator.SimRuntime.iOS-17-5"
)

func newWorld(t *testing.T) *world {
	root := t.TempDir()
	if r, err := filepath.EvalSymlinks(root); err == nil {
		root = r
	}
	w := &world{t: t, root: root, fake: filepath.Join(root, "fake"), bin: filepath.Join(root, "bin"),
		home: filepath.Join(root, "home"), simHome: filepath.Join(root, "simcam"), wd: filepath.Join(root, "wd"),
		now: time.Now()}
	for _, d := range []string{w.fake, w.bin, w.home, w.simHome, w.wd} {
		os.MkdirAll(d, 0o755)
	}
	for n, s := range fakeTools {
		w.write(filepath.Join(w.bin, n), s)
		os.Chmod(filepath.Join(w.bin, n), 0o755)
	}
	// a build: loader + engine pair, stub and runner hook in their own content-hash dirs, a profile, an image and rules
	b := filepath.Join(root, "build")
	w.build = filepath.Join(b, "aaaaaaaaaaaa")
	w.write(filepath.Join(w.build, "libsimcamloader.dylib"), "loader")
	w.write(filepath.Join(w.build, "libsimcam.dylib"), "engine")
	w.write(filepath.Join(b, "bbbbbbbbbbbb", "libsimcamstub.dylib"), "stub")
	w.write(filepath.Join(b, "cccccccccccc", "libsimcamrunnerhook.dylib"), "hook")
	w.write(filepath.Join(b, "profile.plist"), "profile")
	w.write(filepath.Join(b, "qr.png"), "png")
	w.write(filepath.Join(b, "rules", "27A266a.json"),
		`{"rules":[{"name":"test-runner","libs":[{"path":"/usr/lib/libMainThreadChecker.dylib"}]}]}`)
	w.env = []string{
		"PATH=" + w.bin + string(os.PathListSeparator) + "/usr/bin" + string(os.PathListSeparator) + "/bin",
		"HOME=" + w.home, "SIMCAM_HOME=" + w.simHome, "FAKE=" + w.fake, "TMPDIR=" + os.TempDir(),
		"DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer",
		"STUNTCAM_BUILD=" + w.build, "STUNTCAM_PROFILE=" + filepath.Join(b, "profile.plist"),
		"STUNTCAM_IMAGE=" + filepath.Join(b, "qr.png"), "STUNTCAM_RULES=" + filepath.Join(b, "rules"),
	}
	return w
}

// write creates p (and its directory) with content s.
func (w *world) write(p, s string) {
	w.t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		w.t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		w.t.Fatal(err)
	}
}

// sim is a path under the stuntcam home's sims/<udid>/.
func (w *world) sim(udid string, rel ...string) string {
	return filepath.Join(append([]string{w.simHome, "sims", udid}, rel...)...)
}

func (w *world) setenv(kv ...string) { w.env = append(w.env, kv...) }

func (w *world) unsetenv(k string) {
	var out []string
	for _, kv := range w.env {
		if !strings.HasPrefix(kv, k+"=") {
			out = append(out, kv)
		}
	}
	w.env = out
}

// dev adds a simulator to `simctl list devices`; booted ones booted an hour ago.
func (w *world) dev(udid, name, state, runtime string) {
	d := fakeDev{udid, name, state, runtime, w.now.Add(-time.Hour)}
	w.devs = append(w.devs, d)
}

func devicesJSON(devs []fakeDev) string {
	byRT := map[string][]map[string]any{}
	for _, d := range devs {
		byRT[d.runtime] = append(byRT[d.runtime], map[string]any{"udid": d.udid, "name": d.name, "state": d.state,
			"isAvailable": true, "lastBootedAt": d.lastBooted.UTC().Format(time.RFC3339)})
	}
	b, _ := json.Marshal(map[string]any{"devices": byRT})
	return string(b)
}

// runtimes writes `simctl list runtimes -j`: the iOS 27.0 and 26.3 runtimes and tvOS 26.0, available.
func (w *world) runtimes() {
	var rs []map[string]any
	for _, r := range []struct{ id, v, b, p string }{{iosRT, "27.0", "24A1", "iOS"}, {ios26, "26.3", "23D1", "iOS"},
		{tvosRT, "26.0", "23J1", "tvOS"}} {
		rs = append(rs, map[string]any{"identifier": r.id, "version": r.v, "buildversion": r.b, "platform": r.p,
			"isAvailable": true})
	}
	b, _ := json.Marshal(map[string]any{"runtimes": rs})
	w.write(filepath.Join(w.fake, "runtimes.json"), string(b))
}

// bootArmed gives a booted sim a boot environment with the stub, naming simDir ("" = this home's sims/<udid>).
func (w *world) bootArmed(udid, simDir string) {
	if simDir == "" {
		simDir = w.sim(udid)
	}
	w.write(filepath.Join(w.fake, "bootenv", udid),
		"DYLD_INSERT_LIBRARIES=/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib\nSIMCAM_SIM_DIR="+simDir+"\n")
}

func (w *world) bootUnarmed(udid string) {
	w.write(filepath.Join(w.fake, "bootenv", udid), "DYLD_ROOT_PATH=/rt\n")
}

// current points a sim's `current` at a build id.
func (w *world) current(udid, build string) {
	w.write(w.sim(udid, "current"), fmt.Sprintf("1 %s/builds/%s/libsimcamloader.dylib\n", w.simHome, build))
}

// record writes sims/<udid>/arm.json; at is relative to now.
func (w *world) record(udid, mode string, at time.Duration, vars ...string) {
	b, _ := json.Marshal(map[string]any{"proto": 1, "mode": mode, "build": "aaaaaaaaaaaa", "at": w.now.Add(at).Unix(),
		"vars": vars})
	w.write(w.sim(udid, "arm.json"), string(b))
}

func (w *world) config(udid, build string) {
	w.write(w.sim(udid, "config.json"), `{"proto":1,"xcode_version":"27.0","xcode_build":"`+build+
		`","developer_dir":"/Applications/Xcode.app/Contents/Developer","rules_dir":"","source":"up","at":1}`)
}

// latch writes a valid latched-<build> marker (its mtime is when it expires) for bundle under dir.
func (w *world) latch(dir, bundle, features string) {
	p := filepath.Join(dir, bundle, "latched-aaaaaaaaaaaa")
	w.write(p, features+"\n")
	until := w.now.Add(20 * time.Hour)
	os.Chtimes(p, until, until)
}

// container gives bundle an app data container on udid (simctl get_app_container answers with it) and returns its path.
func (w *world) container(udid, bundle string) string {
	c := filepath.Join(w.root, "devices", udid, "data", "Containers", "Data", "Application", bundle)
	w.write(filepath.Join(w.fake, "container", udid, bundle), c+"\n")
	return c
}

// booting writes an arm record of a boot in progress by a live stuntcam (this test's process).
func (w *world) booting(udid string) {
	w.write(w.sim(udid, "arm.json"), fmt.Sprintf(`{"proto":1,"mode":"boot","build":"x","at":%d,"booting":true,"pid":%d}`,
		w.now.Unix(), os.Getpid()))
}

// launchdEnv sets a variable in a booted sim's (fake) launchd environment.
func (w *world) launchdEnv(udid, k, v string) {
	w.write(filepath.Join(w.fake, "env", udid, k), v+"\n")
}

// springboard makes `launchctl list` in every sim show a running SpringBoard.
func (w *world) springboard() {
	w.write(filepath.Join(w.fake, "launchctl-list"), "PID\tStatus\tLabel\n101\t0\tcom.apple.SpringBoard\n")
}

type cliResult struct {
	stdout, stderr, xcrun, launchctl, files string
	code                                    int
}

// shownFiles are the files under the stuntcam home whose content the golden output shows (others: their path only).
var shownFiles = map[string]bool{"current": true, "control": true, "chain.txt": true, "arm.json": true,
	"config.json": true}

// homeFiles lists every file under the stuntcam home after a run, with the content of shownFiles and hook .env files.
func (w *world) homeFiles() string {
	var b strings.Builder
	filepath.WalkDir(w.simHome, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(w.simHome, p)
		fmt.Fprintln(&b, rel)
		if shownFiles[d.Name()] || strings.HasSuffix(p, ".env") {
			c, _ := os.ReadFile(p)
			for _, l := range strings.Split(strings.TrimRight(string(c), "\n"), "\n") {
				fmt.Fprintln(&b, "  | "+l)
			}
		}
		return nil
	})
	return b.String()
}

func (w *world) run(args ...string) cliResult {
	w.t.Helper()
	w.write(filepath.Join(w.fake, "devices.json"), devicesJSON(w.devs))
	if w.privateDevs != nil {
		w.write(filepath.Join(w.fake, "private-devices.json"), devicesJSON(w.privateDevs))
	}
	os.Remove(filepath.Join(w.fake, "xcrun.log"))
	os.Remove(filepath.Join(w.fake, "launchctl.log"))
	c := exec.Command(os.Args[0], args...)
	c.Dir = w.wd
	c.Env = append([]string{goldenChildEnv + "=1"}, w.env...)
	// no controlling terminal: exec's Ctrl-C handling is the same everywhere
	c.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	var out, errb strings.Builder
	c.Stdout, c.Stderr = &out, &errb
	err := c.Run()
	code := 0
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		code = ee.ExitCode()
	} else if err != nil {
		w.t.Fatal(err)
	}
	log, _ := os.ReadFile(filepath.Join(w.fake, "xcrun.log"))
	hostLaunchctl, _ := os.ReadFile(filepath.Join(w.fake, "launchctl.log"))
	return cliResult{out.String(), errb.String(), sortCallRuns(string(log)), string(hostLaunchctl), w.homeFiles(), code}
}

// sortCallRuns sorts each run of consecutive launchctl setenv/getenv calls into one sim: arm sets and checks a sim's
// launchd variables concurrently, so their order within a run varies.
func sortCallRuns(log string) string {
	lines := strings.SplitAfter(log, "\n")
	kind := func(l string) string {
		f := strings.Fields(l)
		if len(f) > 3 && f[0] == "spawn" && f[2] == "launchctl" && (f[3] == "setenv" || f[3] == "getenv") {
			return f[1]
		}
		return "\x00" + l // a run of its own
	}
	for i := 0; i < len(lines); {
		j := i + 1
		for j < len(lines) && kind(lines[j]) == kind(lines[i]) {
			j++
		}
		sort.Strings(lines[i:j])
		i = j
	}
	return strings.Join(lines, "")
}

var goldenScrub = []struct {
	re   *regexp.Regexp
	with string
}{
	{regexp.MustCompile(`exec-[0-9]+-[0-9]+`), "exec-<run>"},
	{regexp.MustCompile(`[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}(:[0-9]{2})?`), "<time>"},
	{regexp.MustCompile(`armed [0-9hmsd]+ ago`), "armed <ago> ago"},
	{regexp.MustCompile(`"ArmedAt": [0-9]+`), `"ArmedAt": <at>`},
	{regexp.MustCompile(`pid [0-9]+`), "pid <pid>"},
	{regexp.MustCompile(`"host": "[a-z0-9]+/[a-z0-9]+"`), `"host": "<os>/<arch>"`},
	{regexp.MustCompile(`"(at|pid)":( ?)[0-9]+`), `"$1":$2<n>`},
	{regexp.MustCompile(`gui/[0-9]+/`), "gui/<uid>/"}, // the host launchctl's domain: the user running the test
}

// scrub makes output comparable across runs and machines: the world's root becomes $T and this test binary's path
// <test binary>; run ids, times and pids become placeholders.
func (w *world) scrub(s string) string {
	s = strings.ReplaceAll(s, w.root, "$T")
	if exe, err := os.Executable(); err == nil {
		s = strings.ReplaceAll(s, exe, "<test binary>")
	}
	for _, r := range goldenScrub {
		s = r.re.ReplaceAllString(s, r.with)
	}
	return s
}

type goldenCase struct {
	name  string
	setup func(w *world) // may be nil
	args  []string
}

func TestGolden(t *testing.T) {
	var cases []goldenCase
	cases = append(cases, usageCases()...)
	cases = append(cases, statusCases()...)
	cases = append(cases, doctorCases()...)
	cases = append(cases, gcCases()...)
	cases = append(cases, execCases()...)
	cases = append(cases, armCases()...)
	cases = append(cases, otherCases()...)
	got := map[string]string{}
	var order []string
	for _, c := range cases {
		if _, dup := got[c.name]; dup {
			t.Fatalf("two cases named %q", c.name)
		}
		w := newWorld(t)
		if c.setup != nil {
			c.setup(w)
		}
		r := w.run(c.args...)
		var b strings.Builder
		fmt.Fprintf(&b, "$ stuntcam %s\n", strings.Join(c.args, " "))
		fmt.Fprintf(&b, "--- stdout\n%s--- stderr\n%s--- simctl\n%s", r.stdout, r.stderr, r.xcrun)
		if r.launchctl != "" { // the host's launchctl (the login service), only where a scenario calls it
			fmt.Fprintf(&b, "--- launchctl\n%s", r.launchctl)
		}
		fmt.Fprintf(&b, "--- files\n%s--- exit %d\n", r.files, r.code)
		got[c.name] = w.scrub(b.String())
		order = append(order, c.name)
	}
	if *updateGolden {
		var b strings.Builder
		for _, n := range order {
			fmt.Fprintf(&b, "=== %s\n%s\n", n, got[n])
		}
		if err := os.WriteFile(goldenFile, []byte(b.String()), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	data, err := os.ReadFile(goldenFile)
	if err != nil {
		t.Fatalf("%v (go test -run TestGolden -update writes it)", err)
	}
	want := map[string]string{}
	for _, sec := range strings.Split("\n"+string(data), "\n=== ")[1:] {
		n, body, _ := strings.Cut(sec, "\n")
		want[n] = strings.TrimRight(body, "\n")
	}
	for _, n := range order {
		if w, ok := want[n]; !ok {
			t.Errorf("%s: not in %s", n, goldenFile)
		} else if w != strings.TrimRight(got[n], "\n") {
			t.Errorf("%s: output changed\n--- want\n%s\n--- got\n%s", n, w, got[n])
		}
	}
	if len(want) != len(order) {
		t.Errorf("%s has %d cases, the test %d", goldenFile, len(want), len(order))
	}
}

func usageCases() []goldenCase {
	cs := []goldenCase{
		{name: "usage: no arguments"},
		{name: "usage: help", args: []string{"help"}},
		{name: "usage: -h", args: []string{"-h"}},
		{name: "usage: --help", args: []string{"--help"}},
		{name: "usage: unknown command", args: []string{"frob"}},
		{name: "usage: private shutdown without arguments", args: []string{"_private-shutdown"}},
	}
	for _, c := range []string{"up", "verify", "exec", "gc", "down", "status", "swap", "trace", "env", "source", "orient",
		"doctor", "version"} {
		cs = append(cs, goldenCase{name: c + " -h", args: []string{c, "-h"}})
	}
	for _, a := range [][]string{
		{"chain", "-h"}, {"chain", "ls", "-h"}, {"chain"}, {"chain", "frob"}, {"chain", "ls", "x"},
		{"sentinel", "-h"}, {"sentinel", "ls", "-h"}, {"sentinel"}, {"sentinel", "ls", "x"},
		{"service"}, {"service", "-h"}, {"service", "status", "-h"}, {"service", "install"}, {"service", "run"},
		{"service", "run", "--only", "A", "--all"}, {"service", "frob", "--all"},
		{"up", "x"}, {"up", "--bogus"}, {"status", "x"}, {"swap", "a", "b"}, {"swap", "L", "--udid", "X"},
		{"trace"}, {"trace", "../x"}, {"trace", "a", "b"}, {"trace", "--pid", "x", "b"},
		{"source"}, {"source", "bogus"}, {"source", "--front", "X", "scene"}, {"source", "scene", "--udid", "X"},
		{"orient"}, {"orient", "sideways"}, {"exec"}, {"exec", "--"}, {"exec", "--bogus"},
		{"gc", "x"}, {"gc", "--keep-builds", "x"}, {"verify", "x"}, {"version", "x"}, {"doctor", "x"}, {"env", "x"},
		{"down", "x"},
	} {
		cs = append(cs, goldenCase{name: "usage error: " + strings.Join(a, " "), args: a})
	}
	return append(cs,
		goldenCase{name: "version", args: []string{"version"}},
		goldenCase{name: "version --check (not installed)", args: []string{"version", "--check"}},
		goldenCase{name: "no simulators: down", args: []string{"down"}},
		goldenCase{name: "no simulators: status", args: []string{"status"}},
		goldenCase{name: "no simulators: status --json", args: []string{"status", "--json"}},
		goldenCase{name: "unknown --udid", args: []string{"down", "--udid", "NOPE"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
		goldenCase{name: "two booted simulators", args: []string{"swap"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.dev("U2", "iPhone B", "Booted", iosRT)
		}},
		goldenCase{name: "no build", args: []string{"env"}, setup: func(w *world) {
			w.unsetenv("STUNTCAM_BUILD")
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
	)
}

// statusWorld has one simulator per case of statusOf's mode switch.
func statusWorld(w *world) {
	// shut down, armed in boot mode before
	w.dev("S1", "Off boot", "Shutdown", iosRT)
	w.record("S1", "boot", -2*time.Hour)
	w.current("S1", "aaaaaaaaaaaa")
	// shut down, launchd record
	w.dev("S2", "Off launchd", "Shutdown", iosRT)
	w.record("S2", "launchd", -2*time.Hour)
	// boot-armed by `up` this boot, with a config, a chain and a latched app
	w.dev("B1", "Boot up", "Booted", iosRT)
	w.bootArmed("B1", "")
	w.current("B1", "aaaaaaaaaaaa")
	w.record("B1", "boot", -30*time.Minute)
	w.config("B1", "27A266a")
	w.write(w.sim("B1", "chain.txt"), "# a comment\n/x/libother.dylib\n\n")
	w.latch(w.sim("B1", "state"), "dev.x", "session")
	// boot-armed by the scripts (no record)
	w.dev("B2", "Boot scripts", "Booted", iosRT)
	w.bootArmed("B2", "")
	w.current("B2", "aaaaaaaaaaaa")
	// boot-armed with an exec run's directory
	w.dev("B3", "Boot exec", "Booted", iosRT)
	w.bootArmed("B3", w.sim("exec-1-2"))
	w.current("exec-1-2", "dddddddddddd")
	w.config("exec-1-2", "27B5")
	// boot-armed under another home
	w.dev("B4", "Boot other home", "Booted", iosRT)
	w.bootArmed("B4", "/other/home/sims/B4")
	// `down` since this boot: boot env has the stub, no current
	w.dev("D1", "Down", "Booted", iosRT)
	w.bootArmed("D1", "")
	w.record("D1", "boot", -30*time.Minute)
	// rebooted without stuntcam: record says boot, boot env has no stub
	w.dev("R1", "Rebooted", "Booted", iosRT)
	w.bootUnarmed("R1")
	w.current("R1", "aaaaaaaaaaaa")
	w.record("R1", "boot", -30*time.Minute)
	// armed by the scripts in launchd mode: current, no record, boot env readable without the stub
	w.dev("A1", "Scripts launchd", "Booted", iosRT)
	w.bootUnarmed("A1")
	w.current("A1", "aaaaaaaaaaaa")
	// env-armed
	w.dev("E1", "Env", "Booted", iosRT)
	w.current("E1", "aaaaaaaaaaaa")
	w.record("E1", "env", -10*time.Minute)
	// booted, nothing at all
	w.dev("N1", "Nothing", "Booted", iosRT)
	// launchd record, no current
	w.dev("N2", "Launchd no current", "Booted", iosRT)
	w.record("N2", "launchd", -10*time.Minute)
	// boot record of this boot, no current, boot env unreadable
	w.dev("N3", "Down unreadable", "Booted", iosRT)
	w.record("N3", "boot", -30*time.Minute)
	// boot record of an earlier boot, current, boot env unreadable
	w.dev("R2", "Rebooted unreadable", "Booted", iosRT)
	w.current("R2", "aaaaaaaaaaaa")
	w.record("R2", "boot", -2*time.Hour)
	// boot record of this boot, current, boot env unreadable: the record decides
	w.dev("B5", "Boot unreadable", "Booted", iosRT)
	w.current("B5", "aaaaaaaaaaaa")
	w.record("B5", "boot", -30*time.Minute)
	// launchd-armed, the stub in launchd's environment
	w.dev("L1", "Launchd", "Booted", iosRT)
	w.current("L1", "aaaaaaaaaaaa")
	w.record("L1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES", "SIMCAM_CONTROL")
	w.launchdEnv("L1", "DYLD_INSERT_LIBRARIES", "/x/libother.dylib:/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib")
	// launchd record, the stub gone from launchd's environment
	w.dev("L2", "Launchd lost", "Booted", iosRT)
	w.current("L2", "aaaaaaaaaaaa")
	w.record("L2", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES")
	// being booted armed by another stuntcam (this test's pid: alive)
	w.dev("P1", "Booting", "Booted", iosRT)
	w.booting("P1")
	// shut down, never armed: not listed
	w.dev("X1", "Never", "Shutdown", iosRT)
}

func statusCases() []goldenCase {
	return []goldenCase{
		{name: "status: every mode", args: []string{"status"}, setup: statusWorld},
		{name: "status --json: every mode", args: []string{"status", "--json"}, setup: statusWorld},
		{name: "status: latched apps in order", args: []string{"status"}, setup: func(w *world) {
			w.dev("U", "iPhone", "Booted", iosRT)
			w.current("U", "aaaaaaaaaaaa")
			for _, b := range []string{"dev.d", "dev.b", "dev.a", "dev.c"} {
				w.latch(w.sim("U", "state"), b, "session")
			}
		}},
		{name: "status: fallback", args: []string{"status"}, setup: func(w *world) {
			w.dev("U", "iPhone", "Booted", iosRT)
			w.current("U", "aaaaaaaaaaaa")
			w.latch(filepath.Join(w.container("U", "dev.x"), "tmp", "simcam-state"), "dev.x", "activate")
			w.write(filepath.Join(w.container("U", "dev.y"), "tmp", "simcam-state", "dev.y", "events.log"),
				"1 1 activate - aaaaaaaaaaaa traced=0\n")
			w.container("U", "dev.p")
			p := filepath.Join(w.container("U", "dev.p"), "tmp", "simcam-state", "preflight-fail-aaaaaaaaaaaa")
			w.write(p, "preflight failed\n")
			until := w.now.Add(20 * time.Hour)
			os.Chtimes(p, until, until)
			for _, b := range []string{"dev.x", "dev.y", "dev.p", "dev.gone"} {
				w.write(w.sim("U", "state", b, "events.log"), "1 1 activate - aaaaaaaaaaaa traced=0\n")
			}
			w.write(w.sim("U", "state", "preflight-fail-aaaaaaaaaaaa"), "preflight failed\n")
			os.Chtimes(w.sim("U", "state", "preflight-fail-aaaaaaaaaaaa"), until, until)
		}},
	}
}

func doctorWorld(w *world) {
	w.runtimes()
	w.dev("B1", "Boot up", "Booted", iosRT)
	w.bootArmed("B1", "")
	w.current("B1", "aaaaaaaaaaaa")
	w.record("B1", "boot", -30*time.Minute)
	w.config("B1", "27B5")
	w.latch(w.sim("B1", "state"), "dev.x", "session")
	w.dev("N1", "Nothing", "Booted", iosRT)
	w.dev("L1", "Launchd", "Booted", iosRT)
	w.current("L1", "aaaaaaaaaaaa")
	w.record("L1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES")
	w.launchdEnv("L1", "DYLD_INSERT_LIBRARIES", "/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib")
	w.dev("S1", "Off", "Shutdown", iosRT)
}

func doctorCases() []goldenCase {
	return []goldenCase{
		{name: "doctor", args: []string{"doctor"}, setup: doctorWorld},
		{name: "doctor --json", args: []string{"doctor", "--json"}, setup: doctorWorld},
		{name: "doctor: no build, no simctl", args: []string{"doctor"}, setup: func(w *world) {
			w.unsetenv("STUNTCAM_BUILD")
			w.write(filepath.Join(w.bin, "xcrun"),
				"#!/bin/sh\necho 'xcrun: error: unable to find utility \"simctl\"' >&2\nexit 72\n")
		}},
	}
}

func gcWorld(w *world) {
	w.runtimes()
	w.dev("U1", "iPhone", "Booted", iosRT)
	w.current("U1", "111111111111")
	w.write(w.sim("U1", "arm.json"), `{"proto":1,"mode":"launchd","build":"222222222222","at":1}`)
	w.write(w.sim("GONE", "current"), "1 x\n")
	old := w.now.Add(-8 * 24 * time.Hour)
	w.write(w.sim("exec-1-1", "current"), "1 "+w.simHome+"/builds/333333333333/libsimcamloader.dylib\n")
	os.Chtimes(w.sim("exec-1-1"), old, old)
	w.current("exec-2-2", "444444444444")
	w.privateDevs = []fakeDev{{"P1", "canary old", "Shutdown", goneRT, w.now}, {"P2", "canary", "Shutdown", iosRT, w.now}}
	os.MkdirAll(filepath.Join(w.simHome, "devices"), 0o755)
	// builds, newest first: five unreferenced, two referenced, one stub dir
	for i, b := range []string{"555555555555", "666666666666", "777777777777", "888888888888", "999999999999",
		"111111111111", "222222222222", "eeeeeeeeeeee"} {
		p := filepath.Join(w.simHome, "builds", b)
		w.write(filepath.Join(p, "libsimcamloader.dylib"), "x")
		if b == "eeeeeeeeeeee" {
			w.write(filepath.Join(p, "libsimcamstub.dylib"), "x")
		}
		mt := w.now.Add(-time.Duration(i+1) * time.Hour)
		os.Chtimes(p, mt, mt)
	}
	// canaries: four, two untouched for days
	for i, c := range []string{"c1c1c1c1c1c1", "c2c2c2c2c2c2", "c3c3c3c3c3c3", "c4c4c4c4c4c4"} {
		p := filepath.Join(w.simHome, "canary", c)
		w.write(filepath.Join(p, "m10canary.app", "x"), "x")
		mt := w.now.Add(-time.Duration(i*30) * time.Hour)
		os.Chtimes(p, mt, mt)
	}
	stale := w.now.Add(-2 * time.Hour)
	for _, p := range []string{filepath.Join(w.simHome, "canary", ".stage-1"),
		filepath.Join(w.simHome, "restore-rules", ".old-1")} {
		os.MkdirAll(p, 0o755)
		os.Chtimes(p, stale, stale)
	}
}

func gcCases() []goldenCase {
	return []goldenCase{
		{name: "gc --dry-run", args: []string{"gc", "--dry-run"}, setup: gcWorld},
		{name: "gc", args: []string{"gc"}, setup: gcWorld},
		{name: "gc --keep-builds 1 --dry-run", args: []string{"gc", "--keep-builds", "1", "--dry-run"}, setup: gcWorld},
		{name: "gc: no runtimes listed", args: []string{"gc", "--dry-run"}, setup: func(w *world) {
			gcWorld(w)
			os.Remove(filepath.Join(w.fake, "runtimes.json"))
		}},
		{name: "gc: simctl fails", args: []string{"gc", "--dry-run"}, setup: func(w *world) {
			gcWorld(w)
			w.write(filepath.Join(w.bin, "xcrun"), "#!/bin/sh\necho boom >&2\nexit 1\n")
		}},
	}
}

// hookRun makes the fake xcodebuild write runner-hook log lines for udid and an activation in sim dir dir.
func hookRun(w *world, udid, dir string) {
	w.write(filepath.Join(w.fake, "xcodebuild-run.sh"), `
mkdir -p "$TEST_RUNNER_SIMCAM_HOOK_DIR"
printf '[1] armed dev.x\n[2] armed dev.y\n[3] failed dev.z: no stub\nnoise\n' > "$TEST_RUNNER_SIMCAM_HOOK_DIR/`+udid+`.log"
mkdir -p "`+dir+`/state/dev.x"
echo "1 1 activate - aaaaaaaaaaaa traced=0" >> "`+dir+`/state/dev.x/events.log"
`)
}

func execCases() []goldenCase {
	xbTest := []string{"exec", "--", "xcodebuild", "test", "-destination", "id=U1"}
	return []goldenCase{
		{name: "exec: exit code passed through", args: []string{"exec", "--", "sh", "-c", "echo out; echo err >&2; exit 3"}},
		{name: "exec: no --", args: []string{"exec", "sh", "-c", "exit 0"}},
		{name: "exec: command not found", args: []string{"exec", "--", "no-such-command-anywhere"}},
		{name: "exec: not executable", args: []string{"exec", "--", "./noexec"}, setup: func(w *world) {
			w.write(filepath.Join(w.wd, "noexec"), "#!/bin/sh\n")
		}},
		{name: "exec: killed by a signal", args: []string{"exec", "--", "sh", "-c", "kill -TERM $$"}},
		{name: "exec: the environment", args: []string{"exec", "--", "xcodebuild", "build"}, setup: func(w *world) {
			w.setenv("SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=/x/libother.dylib")
		}},
		{name: "exec: activations counted",
			args: []string{"exec", "--", "sh", "-c", `d=$(dirname "$SIMCTL_CHILD_SIMCAM_SIM_DIR"); mkdir -p "$d/U1/state/dev.a" "$SIMCTL_CHILD_SIMCAM_SIM_DIR/state/dev.b"; ` +
				`printf '1 1 activate - b traced=0\n2 1 activate - b traced=0\n3 1 activate - b traced=0\n' > "$d/U1/state/dev.a/events.log"; printf '1 1 activate - b traced=0\n' > "$SIMCTL_CHILD_SIMCAM_SIM_DIR/state/dev.b/events.log"`,
				"-destination", "id=u1"}, setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.write(w.sim("U1", "state", "dev.a", "events.log"), "1 1 activate - b traced=0\n")
			}},
		{name: "exec: xcodebuild test, booted unarmed destination", args: xbTest, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			hookRun(w, "U1", w.sim("U1"))
			w.setenv("SIMCAM_LOG=1", "TEST_RUNNER_DYLD_INSERT_LIBRARIES=/t/libmine.dylib")
		}},
		{name: "exec: xcodebuild test, launchd-armed destination", args: xbTest, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.current("U1", "aaaaaaaaaaaa")
			w.record("U1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES")
			w.launchdEnv("U1", "DYLD_INSERT_LIBRARIES", "/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib")
			hookRun(w, "U1", w.sim("U1"))
		}},
		{name: "exec: xcodebuild test, boot-armed destination", args: xbTest, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.bootArmed("U1", "")
			w.current("U1", "aaaaaaaaaaaa")
			w.record("U1", "boot", -30*time.Minute)
		}},
		{name: "exec: xcodebuild test, env-armed destination after down", args: xbTest, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.record("U1", "boot", -30*time.Minute)
		}},
		{name: "exec: xcodebuild test, no runner hook in the build", args: xbTest, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			os.RemoveAll(filepath.Join(w.root, "build", "cccccccccccc"))
		}},
		{name: "exec: xcodebuild test, also a tvOS destination",
			args: append(xbTest, "-destination", "id=TV1"), setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.dev("TV1", "Apple TV", "Booted", tvosRT)
			}},
		{name: "exec: xcodebuild test, named destination only", args: []string{"exec", "--", "xcodebuild", "test",
			"-destination", "platform=iOS Simulator,name=iPhone 17"}},
		{name: "exec: xcodebuild test, cleared environment",
			args: []string{"exec", "--", "env", "-i", "./xcodebuild", "test", "-destination", "id=U1"},
			setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				// in the working directory: a Mac's /usr/bin/xcodebuild must not run
				w.write(filepath.Join(w.wd, "xcodebuild"),
					"#!/bin/sh\necho \"xcodebuild $*\"; env | grep -c TEST_RUNNER_; exit 0\n")
				os.Chmod(filepath.Join(w.wd, "xcodebuild"), 0o755)
			}},
		{name: "exec: other command, unarmed and launchd destinations",
			args: []string{"exec", "--", "sh", "-c", "true", "-destination", "id=U1", "-destination", "id=L1"},
			setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.dev("L1", "iPhone L", "Booted", iosRT)
				w.current("L1", "aaaaaaaaaaaa")
				w.record("L1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES")
				w.launchdEnv("L1", "DYLD_INSERT_LIBRARIES", "/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib")
			}},
		{name: "exec: xcodebuild build, unarmed and launchd destinations",
			args: []string{"exec", "--", "xcodebuild", "build", "-destination", "id=U1", "-destination", "id=L1"},
			setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.dev("L1", "iPhone L", "Booted", iosRT)
				w.current("L1", "aaaaaaaaaaaa")
				w.record("L1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES")
				w.launchdEnv("L1", "DYLD_INSERT_LIBRARIES", "/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib")
			}},
		{name: "exec: shut-down destinations booted armed",
			args: []string{"exec", "--", "sh", "-c", "true", "-destination", "id=S1", "-destination", "id=TV2"},
			setup: func(w *world) {
				w.dev("S1", "iPhone S", "Shutdown", iosRT)
				w.dev("TV2", "Apple TV", "Shutdown", tvosRT)
				w.springboard()
			}},
		{name: "exec: destination being booted", args: []string{"exec", "--", "sh", "-c", "true", "-destination", "id=S1"},
			setup: func(w *world) {
				w.dev("S1", "iPhone S", "Shutdown", iosRT)
				w.booting("S1")
			}},
	}
}

func armCases() []goldenCase {
	return []goldenCase{
		{name: "up: booted sim, launchd mode",
			args: []string{"up", "--skip-verify", "--grant", "dev.x"}, setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.launchdEnv("U1", "DYLD_INSERT_LIBRARIES", "/x/libother.dylib")
			}},
		{name: "up: shut-down sim, boot mode",
			args: []string{"up", "--skip-verify", "--log", "--source", "scene"}, setup: func(w *world) {
				w.dev("U1", "iPhone A", "Shutdown", iosRT)
				w.springboard()
			}},
		{name: "up: already armed this boot",
			args: []string{"up", "--skip-verify", "--grant", "dev.x"}, setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.bootArmed("U1", "")
				w.current("U1", "aaaaaaaaaaaa")
				w.record("U1", "boot", -30*time.Minute)
				w.write(w.sim("U1", "down"), "1\n")
			}},
		// armed this boot by stuntcam 1.1.0 (the record and the boot environment have the old names): `up` points current
		// at this build and keeps the record's vars, without a reboot (the engine also reads the old names)
		{name: "up: armed this boot by stuntcam 1.1.0",
			args: []string{"up", "--skip-verify"}, setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.bootArmed("U1", "")
				w.current("U1", "aaaaaaaaaaaa")
				w.record("U1", "boot", -30*time.Minute, "DYLD_INSERT_LIBRARIES", "SIMCAM_SIM_DIR", "SIMCAM_STATE_DIR",
					"SIMCAM_CHAIN", "SIMCAM_CONTROL", "M2_FAKE", "M2_SESSION_HOOK", "M2_NOLOGDICT", "M2_PLIST", "M5_IMAGE")
			}},
		{name: "up: already armed, --source",
			args: []string{"up", "--skip-verify", "--source", "scene"}, setup: func(w *world) {
				w.dev("U1", "iPhone A", "Booted", iosRT)
				w.record("U1", "boot", -30*time.Minute)
			}},
		{name: "up: boot-armed, --mode launchd", args: []string{"up", "--mode", "launchd"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.record("U1", "boot", -30*time.Minute)
		}},
		{name: "up: armed under another home", args: []string{"up", "--skip-verify"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.bootArmed("U1", "/other/home/sims/U1")
		}},
		{name: "up: bad --mode", args: []string{"up", "--skip-verify", "--mode", "bogus"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
		{name: "up: bad --source", args: []string{"up", "--source", "picture:x"}},
		{name: "up: being booted", args: []string{"up", "--skip-verify"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Shutdown", iosRT)
			w.booting("U1")
		}},
		{name: "up: canary can't start", args: []string{"up"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
		{name: "verify: can't start", args: []string{"verify"}},
		{name: "down: launchd-armed", args: []string{"down"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.current("U1", "aaaaaaaaaaaa")
			w.record("U1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES", "SIMCAM_SIM_DIR")
			w.launchdEnv("U1", "DYLD_INSERT_LIBRARIES", "/x/libother.dylib:/h/builds/bbbbbbbbbbbb/libsimcamstub.dylib")
			w.launchdEnv("U1", "SIMCAM_SIM_DIR", w.sim("U1"))
		}},
		{name: "env", args: []string{"env", "--udid", "U1"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
		{name: "env: after down", args: []string{"env"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.record("U1", "boot", -30*time.Minute)
		}},
		{name: "swap", args: []string{"swap"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
			w.current("U1", "dddddddddddd")
			w.record("U1", "launchd", -10*time.Minute)
		}},
		{name: "swap: not armed", args: []string{"swap"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
	}
}

func controlWorld(w *world) {
	w.dev("U1", "iPhone A", "Booted", iosRT)
	w.current("U1", "aaaaaaaaaaaa")
	w.record("U1", "launchd", -10*time.Minute, "DYLD_INSERT_LIBRARIES", "SIMCAM_CONTROL")
	w.launchdEnv("U1", "SIMCAM_CONTROL", w.sim("U1", "control"))
}

func otherCases() []goldenCase {
	chainWorld := func(w *world) {
		w.dev("U1", "iPhone A", "Booted", iosRT)
		w.write(filepath.Join(w.wd, "libextra.dylib"), "x")
		w.write(w.sim("U1", "chain.txt"), "/x/liba.dylib\n")
	}
	sentinelWorld := func(w *world) {
		w.dev("U1", "iPhone A", "Booted", iosRT)
		w.current("U1", "aaaaaaaaaaaa")
		w.write(w.sim("U1", "state", "dev.x", "events.log"), "1700000000 1 activate - aaaaaaaaaaaa traced=0\n")
		w.latch(w.sim("U1", "state"), "dev.x", "session")
		w.write(w.sim("U1", "state", "dev.y", "events.log"), "1700000000 1 activate - aaaaaaaaaaaa traced=0\n")
		w.latch(filepath.Join(w.container("U1", "dev.x"), "tmp", "simcam-state"), "dev.x", "activate")
	}
	return []goldenCase{
		{name: "chain add", args: []string{"chain", "add", "libextra.dylib"}, setup: chainWorld},
		{name: "chain add: missing", args: []string{"chain", "add", "libnone.dylib"}, setup: chainWorld},
		{name: "chain add: ours", args: []string{"chain", "add", "libsimcamx.dylib"}, setup: chainWorld},
		{name: "chain rm", args: []string{"chain", "rm", "/x/liba.dylib"}, setup: chainWorld},
		{name: "chain ls", args: []string{"chain", "ls"}, setup: chainWorld},
		{name: "chain add: no LIB", args: []string{"chain", "add"}, setup: chainWorld},
		{name: "sentinel ls", args: []string{"sentinel", "ls"}, setup: sentinelWorld},
		{name: "sentinel clear", args: []string{"sentinel", "clear"}, setup: sentinelWorld},
		{name: "sentinel clear: shut down", args: []string{"sentinel", "clear", "--bundle", "dev.x"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Shutdown", iosRT)
			w.current("U1", "aaaaaaaaaaaa")
			w.write(w.sim("U1", "state", "dev.x"), "blocks\n")
		}},
		{name: "trace: none", args: []string{"trace", "dev.x"}, setup: sentinelWorld},
		{name: "source scene", args: []string{"source", "scene"}, setup: controlWorld},
		{name: "source image", args: []string{"source", "image:" + "pic.png"}, setup: func(w *world) {
			controlWorld(w)
			w.write(filepath.Join(w.wd, "pic.png"), "png")
		}},
		{name: "source: missing image", args: []string{"source", "image:none.png"}, setup: controlWorld},
		{name: "source: webcam without a helper", args: []string{"source", "webcam"}, setup: controlWorld},
		{name: "source: not armed", args: []string{"source", "scene"}, setup: func(w *world) {
			w.dev("U1", "iPhone A", "Booted", iosRT)
		}},
		{name: "orient", args: []string{"orient", "landscapeleft"}, setup: controlWorld},
		{name: "orient: shut down", args: []string{"orient", "auto"}, setup: func(w *world) {
			controlWorld(w)
			w.devs[0].state = "Shutdown"
		}},
		{name: "service status", args: []string{"service", "status"}},
		{name: "service uninstall", args: []string{"service", "uninstall"}},
	}
}

// `stuntcam status` puts an app's latches in one pair of brackets: a marker's "(marker: …)" already has its own, so a
// latch held only by a marker isn't bracketed again; counted features with a marker keep the outer pair.
func TestStatusLatchBrackets(t *testing.T) {
	w := newWorld(t)
	w.dev("U", "iPhone", "Booted", iosRT)
	w.current("U", "aaaaaaaaaaaa")
	w.latch(w.sim("U", "state"), "dev.x", "session")
	w.latch(w.sim("U", "state"), "dev.y", "session")
	at := w.now.Unix()
	w.write(w.sim("U", "state", "dev.y", "events.log"), fmt.Sprintf("%d 1 activate - aaaaaaaaaaaa traced=0\n"+
		"%d 1 crash session aaaaaaaaaaaa\n%d 2 activate - aaaaaaaaaaaa traced=0\n%d 2 crash session aaaaaaaaaaaa\n",
		at, at, at, at))
	r := w.run("status")
	got := w.scrub(r.stdout)
	for _, want := range []string{
		"  latched: dev.x (marker: session until <time>): no camera and stock CoreMotion until " +
			"`stuntcam sentinel clear --bundle dev.x`\n",
		"  latched: dev.y (session (marker: session until <time>)): no camera and stock CoreMotion until " +
			"`stuntcam sentinel clear --bundle dev.y`\n",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("status: no line\n%s--- got\n%s", want, got)
		}
	}
}

// a --json command whose JSON can't be written (stdout closed) says so on stderr and exits 1, whatever its checks found
func TestJSONWriteFailure(t *testing.T) {
	for _, args := range [][]string{{"status", "--json"}, {"doctor", "--json"}} {
		w := newWorld(t)
		doctorWorld(w)
		w.setenv(closedStdoutEnv + "=1")
		r := w.run(args...)
		if want := "stuntcam: write /dev/stdout: file already closed\n"; r.code != 1 || r.stderr != want {
			t.Errorf("%s: exit %d, stderr %q; want exit 1, stderr %q", strings.Join(args, " "), r.code, r.stderr, want)
		}
	}
	// with failed checks too, the write error is still the one reported (not "N check(s) failed")
	w := newWorld(t)
	w.unsetenv("STUNTCAM_BUILD")
	w.write(filepath.Join(w.bin, "xcrun"), "#!/bin/sh\necho 'xcrun: error: unable to find utility \"simctl\"' >&2\nexit 72\n")
	w.setenv(closedStdoutEnv + "=1")
	r := w.run("doctor", "--json")
	if want := "stuntcam: write /dev/stdout: file already closed\n"; r.code != 1 || r.stderr != want {
		t.Errorf("doctor --json with failed checks: exit %d, stderr %q; want exit 1, stderr %q", r.code, r.stderr, want)
	}
}
