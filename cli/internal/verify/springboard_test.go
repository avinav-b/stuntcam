// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

const set = "/Users/x/Library/Application Support/stuntcam-e37/devices"

// ps output as `ps -axww -o pid=,ppid=,stat=,command=` prints it: the user's own booted sim (default set) and ours
func psFixture(sb string) string {
	s := `    1     0 Ss   /sbin/launchd
26714     1 S    launchd_sim /Users/x/Library/Developer/CoreSimulator/Devices/5EED0002-0000-4000-8000-00000000B002/data/var/run/launchd_bootstrap.plist
26720 26714 Ss   /Library/Developer/CoreSimulator/Volumes/iOS_23D8133/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 26.3.simruntime/Contents/Resources/RuntimeRoot/System/Library/CoreServices/SpringBoard.app/SpringBoard
91194     1 S    launchd_sim ` + set + `/5EED0001-0000-4000-8000-00000000B001/data/var/run/launchd_bootstrap.plist
91218 91194 Ss   /private/var/run/com.apple.security.cryptexd/mnt/x/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 27.0.simruntime/Contents/Resources/RuntimeRoot/usr/libexec/backboardd
91300 91194 Ss   /bin/zsh -c echo /SpringBoard.app/SpringBoard is not it
`
	return s + sb
}

func TestSpringboardFromPS(t *testing.T) {
	dev := set + "/5EED0001-0000-4000-8000-00000000B001"
	live := "91197 91194 Ss   /private/var/run/com.apple.security.cryptexd/mnt/x/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS 27.0.simruntime/Contents/Resources/RuntimeRoot/System/Library/CoreServices/SpringBoard.app/SpringBoard\n"
	if p, ld := springboardFromPS(psFixture(live), dev); p != "91197" || !ld {
		t.Fatalf("got %q %v", p, ld)
	}
	if p, ld := springboardFromPS(psFixture(live), dev+"/"); p != "91197" || !ld {
		t.Fatalf("trailing slash: %q %v", p, ld)
	}
	// SpringBoard gone (restarting) or a zombie: none, but the device's launchd_sim is there
	if p, ld := springboardFromPS(psFixture(""), dev); p != "" || !ld {
		t.Fatalf("absent: %q %v", p, ld)
	}
	if p, _ := springboardFromPS(psFixture("91197 91194 Z    /x/SpringBoard.app/SpringBoard\n"), dev); p != "" {
		t.Fatalf("zombie: %q", p)
	}
	// another device's SpringBoard is never ours; a device that isn't booted has no launchd_sim
	if p, ld := springboardFromPS(psFixture(live), set+"/0000AAAA-0000-4000-8000-00000000B001"); p != "" || ld {
		t.Fatalf("other device: %q %v", p, ld)
	}
	if p, _ := springboardFromPS(psFixture(live), "/Users/x/Library/Developer/CoreSimulator/Devices/5EED0002-0000-4000-8000-00000000B002"); p != "26720" {
		t.Fatalf("default-set device: %q", p)
	}
}

func TestGuardSpringBoard(t *testing.T) {
	dev := set + "/5EED0001-0000-4000-8000-00000000B001"
	sb := func(pid string) string {
		return pid + " 91194 Ss   /r/iOS 27.0.simruntime/Contents/Resources/RuntimeRoot/System/Library/CoreServices/SpringBoard.app/SpringBoard\n"
	}
	var mu sync.Mutex
	t0 := time.Now()
	spawns := 0
	oldPS, oldSpawn := hostProcs, springboardSpawn
	defer func() { hostProcs, springboardSpawn = oldPS, oldSpawn }()
	// absent for 0.5 s, pid 100 until 1.0 s (backboardd's restart), then pid 200 for good
	hostProcs = func(string, string) (string, error) {
		e := time.Since(t0)
		switch {
		case e < 500*time.Millisecond:
			return psFixture(""), nil
		case e < time.Second:
			return psFixture(sb("100")), nil
		}
		return psFixture(sb("200")), nil
	}
	springboardSpawn = func(simctl.Ctl, string) string { mu.Lock(); spawns++; mu.Unlock(); return "" }
	guardSpringBoard(simctl.Ctl{}, dev, "U", 50*time.Millisecond)
	e := time.Since(t0)
	// lower bound exact (the rule); upper bound loose: -race on a loaded Mac (e.g. during the e36-e38 xcodebuild loops) can
	// delay a 50 ms poll by a second or more (w4 e38, review L4)
	if e < 3*time.Second || e > 6*time.Second {
		t.Fatalf("guard returned after %s, want ~3 s (2 s after the last pid change at 1 s)", e)
	}
	if spawns != 0 {
		t.Fatalf("%d spawn polls while the host saw the device's launchd_sim (w4 e38: one took ~10 s under load)", spawns)
	}
	// ps fails (times out): no answer, and no spawn until the third failure in a row (w4 e38)
	spawns = 0
	fails := 0
	hostProcs = func(string, string) (string, error) {
		mu.Lock()
		defer mu.Unlock()
		if fails < 2 {
			fails++
			return "", errors.New("timed out")
		}
		return psFixture(sb("400")), nil
	}
	springboardSpawn = func(simctl.Ctl, string) string { mu.Lock(); spawns++; mu.Unlock(); return "" }
	t0 = time.Now()
	guardSpringBoard(simctl.Ctl{}, dev, "U", 50*time.Millisecond)
	if spawns != 0 || time.Since(t0) > 5*time.Second {
		t.Fatalf("ps errors: %d spawns, %s", spawns, time.Since(t0))
	}
	n := 0
	errs := 0
	hostProcs = func(string, string) (string, error) { return "", errors.New("timed out") }
	springboardSpawn = func(simctl.Ctl, string) string { n++; return "500" }
	for i := 0; i < 3; i++ {
		if p, how := springboardPID(simctl.Ctl{}, dev, "U", &errs); (i < 2) != (how == "host-error") || (i == 2 && p != "500") {
			t.Fatalf("poll %d: %q %s", i, p, how)
		}
	}
	// the ucomm form (hostProcs since w4 e38): a SpringBoard child by name; a zsh child is not it
	ucomm := "91194     1 S    launchd_sim " + set + "/5EED0001-0000-4000-8000-00000000B001/data/var/run/launchd_bootstrap.plist\n91300 91194 Ss   zsh\n91197 91194 Ss   SpringBoard\n"
	if p, ld := springboardFromPS(ucomm, dev); p != "91197" || !ld {
		t.Fatalf("ucomm form: %q %v", p, ld)
	}
	// the host method finds nothing at all (e.g. a runtime laid out differently): the spawn answer is used
	t0 = time.Now()
	hostProcs = func(string, string) (string, error) { return "", nil }
	springboardSpawn = func(simctl.Ctl, string) string { return "300" }
	guardSpringBoard(simctl.Ctl{}, dev, "U", 50*time.Millisecond)
	if e := time.Since(t0); e < 2*time.Second || e > 5*time.Second {
		t.Fatalf("spawn-only guard returned after %s", e)
	}
}

func TestParseLaunchctlList(t *testing.T) {
	out := "PID\tStatus\tLabel\n-\t0\tcom.apple.CoreSimulator.SimLaunchHost-x86\n26714\t0\tcom.apple.CoreSimulator.SimDevice.5EED0002-0000-4000-8000-00000000B002\n-\t0\tcom.apple.CoreSimulator.SimDevice.5EED0001-0000-4000-8000-00000000B001\n"
	if p, k := parseLaunchctlList(out, "5EED0002-0000-4000-8000-00000000B002"); p != "26714" || !k {
		t.Fatalf("running: %q %v", p, k)
	}
	if p, k := parseLaunchctlList(out, "5EED0001-0000-4000-8000-00000000B001"); p != "" || !k {
		t.Fatalf("not running: %q %v", p, k)
	}
	if _, k := parseLaunchctlList(out, "0000"); k {
		t.Fatal("unknown device reported known")
	}
}
