// SPDX-License-Identifier: Apache-2.0
package procenv

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
)

func TestLaunchdSimPid(t *testing.T) {
	ps := `    1 /sbin/launchd
20497 launchd_sim /Users/x/Library/Developer/CoreSimulator/Devices/5EED0004-0000-4000-8000-00000000B004/data/var/run/launchd_bootstrap.plist
 3301 /bin/zsh -c grep 5EED0003-0000-4000-8000-00000000B003/data/var/run/launchd_bootstrap.plist
 3302 pgrep -f launchd_sim .*/5EED0003-0000-4000-8000-00000000B003/data/var/run/launchd_bootstrap.plist
 3303 /usr/bin/not_launchd_sim /x/5EED0003-0000-4000-8000-00000000B003/data/var/run/launchd_bootstrap.plist
26714 /Library/Developer/PrivateFrameworks/CoreSimulator.framework/Resources/bin/launchd_sim /Users/x/Library/Application Support/stuntcam/devices/5EED0003-0000-4000-8000-00000000B003/data/var/run/launchd_bootstrap.plist
  812 /Applications/Xcode 27.app/Contents/Developer/launchd_sim /Users/x/Library/Application Support/stuntcam e30 & co/devices/5EED0005-0000-4000-8000-00000000B005/data/var/run/launchd_bootstrap.plist
`
	tests := []struct {
		name, udid string
		pid        int
		found      bool
	}{
		{"default device set", "5EED0004-0000-4000-8000-00000000B004", 20497, true},
		// the private device set under Application Support; the grep/pgrep lines naming the udid are not it
		{"path with a space", "5EED0003-0000-4000-8000-00000000B003", 26714, true},
		{"spaces in the launchd_sim path and the device set", "5EED0005-0000-4000-8000-00000000B005", 812, true},
		{"not booted", "5EED0007-0000-4000-8000-00000000B007", 0, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			pid, ok := LaunchdSimPid(ps, tt.udid)
			if !tt.found {
				if ok {
					t.Fatal("found a sim that isn't booted")
				}
				return
			}
			if !ok || pid != tt.pid {
				t.Fatalf("%d %v", pid, ok)
			}
		})
	}
}

func buf(argc int, exe string, strs ...string) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, uint32(argc))
	b = append(b, exe...)
	b = append(b, 0, 0, 0, 0)
	for _, s := range strs {
		b = append(b, s...)
		b = append(b, 0)
	}
	return append(b, 0, 0, 'j', 'u', 'n', 'k')
}

func TestParseProcargs2(t *testing.T) {
	a, e, err := ParseProcargs2(buf(2, "/x/launchd_sim", "launchd_sim", "/D/U/data/var/run/launchd_bootstrap.plist",
		"DYLD_INSERT_LIBRARIES=/h/builds/09908f5dbe40/libsimcamstub.dylib", "SIMCAM_SIM_DIR=/h/sims/U", "EMPTY="))
	if err != nil || len(a) != 2 || a[0] != "launchd_sim" {
		t.Fatalf("%q %v", a, err)
	}
	m := Map(e)
	if !reflect.DeepEqual(m, map[string]string{"DYLD_INSERT_LIBRARIES": "/h/builds/09908f5dbe40/libsimcamstub.dylib", "SIMCAM_SIM_DIR": "/h/sims/U", "EMPTY": ""}) {
		t.Fatalf("%v", m)
	}
	if ok, dir := Armed(m, "libsimcamstub.dylib"); !ok || dir != "/h/sims/U" {
		t.Fatalf("%v %q", ok, dir)
	}
	if ok, _ := Armed(map[string]string{"DYLD_INSERT_LIBRARIES": "/x/libother.dylib", "SIMCAM_SIM_DIR": "/h"}, "libsimcamstub.dylib"); ok {
		t.Fatal("armed without the stub")
	}
	if _, _, err := ParseProcargs2([]byte{1, 0}); err == nil {
		t.Fatal("short buffer accepted")
	}
	short := buf(3, "/x", "a")
	if _, _, err := ParseProcargs2(short[:len(short)-6]); err == nil {
		t.Fatal("argc beyond the buffer accepted")
	}
}

// the real sysctl on this process: our own environment comes back
func TestProcargs2Self(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("macOS only")
	}
	t.Setenv("PROCENV_TEST_MARK", "x1") // only in Go's env, not the process's original block
	b, err := procargs2(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	a, e, err := ParseProcargs2(b)
	if err != nil || len(a) == 0 || !strings.HasSuffix(a[0], ".test") && !strings.Contains(a[0], "procenv") {
		t.Fatalf("args %q %v", a, err)
	}
	if len(e) == 0 {
		t.Fatal("no environment")
	}
}

func TestExecPathSelf(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("KERN_PROCARGS2 is macOS only")
	}
	exe, _ := os.Executable()
	got := ExecPath(os.Getpid())
	if a, _ := filepath.EvalSymlinks(got); a != exe {
		if b, _ := filepath.EvalSymlinks(exe); a != b {
			t.Fatalf("%q, want %q", got, exe)
		}
	}
	if ExecPath(-1) != "" {
		t.Fatal()
	}
}
