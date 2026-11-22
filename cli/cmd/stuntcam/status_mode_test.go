// SPDX-License-Identifier: Apache-2.0
package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// setModeBeforeRewrite is setMode as it was before it was split into steps: one switch whose cases are tried in
// order. TestSetModeMatchesSwitch checks the steps give the same simStatus for every input it can make.
func (s *simStatus) setModeBeforeRewrite(d simctl.Device, rec arm.Record, hasRecord, hasCurrent bool, boot bootArm) {
	switch {
	case d.State != simctl.Booted:
		if hasRecord && rec.Mode == arm.Boot {
			s.Note = "boot-mode arming ends at shutdown; `stuntcam up` boots it armed"
		}
	case hasRecord && arm.InProgress(rec):
		s.Mode, s.Note = modeBooting, "being booted armed by `stuntcam up` or `exec` right now"
	case boot.read && boot.stub && boot.dir != "" && filepath.Clean(boot.dir) != home.Sim(d.UDID):
		s.Mode = modeBoot
		s.readOtherSimDir(d, boot.dir)
	case boot.read && boot.stub && !hasCurrent:
		s.Mode, s.Note = modeBootedUnarmed, noteDownSinceBoot
	case boot.read && boot.stub:
		s.Mode = modeBoot
		if hasRecord && rec.Mode == arm.Boot {
			s.ArmedAt = rec.At
		} else {
			s.Note = "boot-armed outside the stuntcam command (scripts/boot.sh?)"
		}
	case boot.read && hasRecord && rec.Mode == arm.Boot: // the boot environment has no stub
		s.Mode, s.Note = modeBootedUnarmed, noteRebootedSinceUp
	// from here on the boot environment is unread, or has no stub and the record (if any) isn't boot mode's
	case !hasRecord && hasCurrent:
		s.Mode = modeScriptArmed
		s.Note = "armed outside the stuntcam command (scripts/boot.sh or launchd-arm.sh?): mode unknown"
	case hasCurrent && rec.Mode == arm.Env: // hasCurrent: hasRecord too
		s.Mode, s.Note = modeEnv, "per-launch arming only (`stuntcam env`): launches without its exports get no camera"
	case !hasRecord || !hasCurrent:
		s.Mode = modeBootedUnarmed
		if hasRecord && arm.SameBoot(rec, true, d.LastBooted) { // a boot record of this boot (its boot env unread)
			s.Note = noteDownSinceBoot
		}
	// from here on there are both a record and current
	case rec.Mode == arm.Boot && !arm.SameBoot(rec, true, d.LastBooted):
		s.Mode, s.Note = modeBootedUnarmed, noteRebootedSinceUp
	default:
		s.Mode, s.ArmedAt = statusMode(rec.Mode), rec.At
		if rec.Mode == arm.Launchd {
			if v, _ := ctl.Getenv(d.UDID, "DYLD_INSERT_LIBRARIES"); !strings.Contains(v, artifacts.StubName) {
				s.Mode, s.Note = modeBootedUnarmed, "the stub is no longer in the simulator's launchd environment"
			}
		}
	}
}

// TestSetModeMatchesSwitch runs setMode and setModeBeforeRewrite on every combination of sim state, boot time, arm
// record (none, or each mode with a boot-in-progress, this-boot and earlier-boot time), current, boot environment
// (unread, no stub, the stub with each kind of sim dir) and launchd environment (stub, none, unreadable), and
// compares the whole simStatus.
func TestSetModeMatchesSwitch(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	const udid = "U1"
	now := time.Now()

	// an exec run's sim dir with its own current and config.json, and another home's dir for the same udid
	runDir := t.TempDir()
	os.WriteFile(filepath.Join(runDir, "current"), []byte("1 /h/builds/aaaaaaaaaaaa/libsimcamloader.dylib\n"), 0o644)
	os.WriteFile(filepath.Join(runDir, "config.json"), []byte(`{"xcode_build":"27A266a"}`), 0o644)
	otherHomeDir := filepath.Join(t.TempDir(), "sims", udid)

	// a fake xcrun answers `launchctl getenv DYLD_INSERT_LIBRARIES` from $FAKE_INSERTS, or fails when it is "fail"
	bin := t.TempDir()
	script := "#!/bin/sh\ncase \"$*\" in *'launchctl getenv DYLD_INSERT_LIBRARIES') " +
		"[ \"$FAKE_INSERTS\" = fail ] && exit 3; printf '%s\\n' \"$FAKE_INSERTS\";; *) exit 3;; esac\n"
	os.WriteFile(filepath.Join(bin, "xcrun"), []byte(script), 0o755)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))

	states := []simctl.State{simctl.Booted, "Shutdown"}
	lastBoots := []time.Time{now.Add(-time.Minute), {}}
	modes := []arm.Mode{arm.Boot, arm.Launchd, arm.Env, "", "future"}
	recordTimes := []func(arm.Mode) arm.Record{
		func(m arm.Mode) arm.Record { return arm.Record{Mode: m, At: now.Unix()} },
		func(m arm.Mode) arm.Record { return arm.Record{Mode: m, At: now.Add(-time.Hour).Unix()} },
		func(m arm.Mode) arm.Record {
			return arm.Record{Mode: m, At: now.Unix(), Booting: true, Pid: os.Getpid()}
		},
		func(m arm.Mode) arm.Record {
			return arm.Record{Mode: m, At: now.Add(-time.Hour).Unix(), Booting: true, Pid: os.Getpid()}
		},
	}
	boots := []bootArm{
		{},
		{read: true},
		{read: true, stub: true},
		{read: true, stub: true, dir: home.Sim(udid)},
		{read: true, stub: true, dir: home.Sim(udid) + "/"},
		{read: true, stub: true, dir: runDir},
		{read: true, stub: true, dir: otherHomeDir},
	}
	launchdInserts := []string{"/h/builds/09908f5dbe40/" + artifacts.StubName, "", "fail"}

	type input struct {
		device     simctl.Device
		record     arm.Record
		hasRecord  bool
		hasCurrent bool
		boot       bootArm
	}
	var inputs []input
	for _, state := range states {
		for _, lastBooted := range lastBoots {
			device := simctl.Device{UDID: udid, Name: "iPhone", State: state, LastBooted: lastBooted}
			for _, hasCurrent := range []bool{false, true} {
				for _, boot := range boots {
					inputs = append(inputs, input{device, arm.Record{}, false, hasCurrent, boot})
					for _, mode := range modes {
						for _, makeRecord := range recordTimes {
							inputs = append(inputs, input{device, makeRecord(mode), true, hasCurrent, boot})
						}
					}
				}
			}
		}
	}

	seen := map[statusMode]int{}
	notes := map[string]int{}
	fromRunDir := 0
	for _, inserts := range launchdInserts {
		t.Setenv("FAKE_INSERTS", inserts)
		for _, in := range inputs {
			start := simStatus{UDID: udid, Mode: modeUnarmed} // as statusOf starts it
			before, after := start, start
			before.setModeBeforeRewrite(in.device, in.record, in.hasRecord, in.hasCurrent, in.boot)
			after.setMode(in.device, in.record, in.hasRecord, in.hasCurrent, in.boot)
			if !reflect.DeepEqual(before, after) {
				t.Fatalf("inserts %q, %+v:\nbefore %+v\nafter  %+v", inserts, in, before, after)
			}
			seen[after.Mode]++
			notes[after.Note]++
			if after.Build == "aaaaaaaaaaaa" && after.Xcode == "27A266a" {
				fromRunDir++
			}
		}
	}

	// every mode and note setMode can give was reached
	for _, mode := range []statusMode{modeUnarmed, modeBooting, modeBoot, modeLaunchd, modeEnv, modeScriptArmed,
		modeBootedUnarmed, "future", ""} {
		if seen[mode] == 0 {
			t.Errorf("no input gave mode %q", mode)
		}
	}
	if len(notes) != 11 { // no note, and ten texts (two of them name the other dir)
		t.Errorf("%d notes reached, want 11", len(notes))
	}
	if fromRunDir == 0 || notes["the stub is no longer in the simulator's launchd environment"] == 0 {
		t.Errorf("the run dir's build was read %d times; the launchd check failed %d times", fromRunDir,
			notes["the stub is no longer in the simulator's launchd environment"])
	}
	if t.Failed() || testing.Verbose() {
		t.Log(len(inputs)*len(launchdInserts), "combinations; modes:", seen)
		for note, n := range notes {
			t.Logf("%5d %s", n, note)
		}
	}
}
