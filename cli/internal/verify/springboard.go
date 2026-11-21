// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"os/exec"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// The SpringBoard guard's poll. A simulator's processes are host processes: the device's launchd_sim
// runs as `launchd_sim <set>/<udid>/data/var/run/launchd_bootstrap.plist`, and its SpringBoard is a direct child whose
// executable is …/CoreServices/SpringBoard.app/SpringBoard, with the same pid `launchctl list` shows inside the device
// (checked on iOS 26.3, 27.0 and 17.5). One `ps` takes ~40 ms where `simctl spawn … launchctl list` took 0.3–1.0 s under
// load, so the guard can poll every 150 ms and costs about its 2 s window.

// hostProcs lists host processes in `ps -o pid=,ppid=,stat=,command=` form, cheaply, for the guard's poll of device udid
// (whose directory is deviceDir): every process by its accounting name (`ps … ucomm=`, from the kernel's process table, no
// argv), plus one synthesized line for the device's launchd_sim, whose pid launchd itself names (`launchctl list`: the job
// com.apple.CoreSimulator.SimDevice.<udid>). Only if launchd doesn't know that job does it read the launchd_sim processes'
// argv (ps -p): a full `ps -axww … command=` reads every process's argv, and right after a boot under an
// xcodebuild loop that blocked in the kernel for up to 10.6 s (a kill can't end it sooner), the launchd_sim argv read too.
// A var for tests.
var hostProcs = func(udid, deviceDir string) (string, error) {
	names, err := runBounded(exec.Command("/bin/ps", "-ax", "-o", "pid=,ppid=,stat=,ucomm="), 2*time.Second)
	if err != nil {
		return "", err
	}
	var ld []string
	var out strings.Builder
	for _, l := range strings.Split(names, "\n") {
		if f := strings.Fields(l); len(f) >= 4 && f[3] == "launchd_sim" {
			ld = append(ld, f[0])
			continue
		}
		out.WriteString(l + "\n")
	}
	if pid, known := launchdSimPID(udid); known {
		if pid != "" {
			fmt.Fprintf(&out, "%s 1 S launchd_sim %s/data/var/run/launchd_bootstrap.plist\n", pid, strings.TrimRight(deviceDir, "/"))
		}
		return out.String(), nil
	}
	if len(ld) > 0 {
		ps := exec.Command("/bin/ps", "-ww", "-o", "pid=,ppid=,stat=,command=", "-p", strings.Join(ld, ","))
		args, err := runBounded(ps, 2*time.Second)
		if err != nil {
			return "", err
		}
		out.WriteString(args)
	}
	return out.String(), nil
}

// launchdSimPID asks launchd for the device's launchd_sim job: its pid ("" if the job exists but isn't running); known false
// if launchd has no such job (or launchctl failed). A var for tests.
var launchdSimPID = func(udid string) (pid string, known bool) {
	out, err := runBounded(exec.Command("/bin/launchctl", "list"), 2*time.Second)
	if err != nil {
		return "", false
	}
	return parseLaunchctlList(out, udid)
}

// parseLaunchctlList finds the device's launchd_sim job in `launchctl list` output (PID, status, label per line).
func parseLaunchctlList(out, udid string) (pid string, known bool) {
	label := "com.apple.CoreSimulator.SimDevice." + udid
	for _, l := range strings.Split(out, "\n") {
		if f := strings.Fields(l); len(f) == 3 && f[2] == label {
			if f[0] == "-" {
				return "", true
			}
			return f[0], true
		}
	}
	return "", false
}

// springboardFromPS finds the device's SpringBoard pid in hostProcs output: the live (not zombie) child of the launchd_sim
// whose bootstrap plist is under deviceDir, named SpringBoard (ucomm) or …/SpringBoard.app/SpringBoard (command). "" if there is no such launchd_sim, or it has no SpringBoard child.
func springboardFromPS(ps, deviceDir string) (pid string, launchd bool) {
	type proc struct{ pid, ppid, stat, cmd string }
	var procs []proc
	for _, l := range strings.Split(ps, "\n") {
		f := strings.Fields(l)
		if len(f) < 4 {
			continue
		}
		i := strings.Index(l, f[2]) + len(f[2]) // the command keeps its own spaces (runtime paths have them)
		procs = append(procs, proc{f[0], f[1], f[2], strings.TrimSpace(l[i:])})
	}
	boot := strings.TrimRight(deviceDir, "/") + "/data/var/run/launchd_bootstrap.plist"
	var ld string
	for _, p := range procs {
		plist, isLaunchdSim := strings.CutPrefix(p.cmd, "launchd_sim ")
		if isLaunchdSim && strings.TrimSpace(plist) == boot && !strings.HasPrefix(p.stat, "Z") {
			ld = p.pid
		}
	}
	if ld == "" {
		return "", false
	}
	for _, p := range procs {
		isSpringBoard := p.cmd == "SpringBoard" || strings.HasSuffix(p.cmd, "/SpringBoard.app/SpringBoard")
		if p.ppid == ld && !strings.HasPrefix(p.stat, "Z") && isSpringBoard {
			return p.pid, true
		}
	}
	return "", true
}

// springboardPID is the guard's poll: host side first. Only if the host can't see the device's launchd_sim at all (ps
// failed, or a runtime laid out differently) does it ask inside the device. A launchd_sim without a SpringBoard child is
// an answer ("not running yet"), not a reason to spawn: right after bootstatus a spawn poll found nothing the next host
// poll didn't (29 of 31), and under an xcodebuild loop one `simctl spawn` took 9.7-11.3 s on 27.0. how says which
// answered. A host poll that fails (ps timed out) is no answer ("host-error"); only the third in a row falls back to spawn.
func springboardPID(ctl simctl.Ctl, deviceDir, udid string, hostErrs *int) (pid, how string) {
	ps, err := hostProcs(udid, deviceDir)
	if err == nil {
		*hostErrs = 0
		if p, launchd := springboardFromPS(ps, deviceDir); p != "" || launchd {
			return p, "host"
		}
	} else if *hostErrs++; *hostErrs < 3 {
		return "", "host-error"
	}
	return springboardSpawn(ctl, udid), "spawn"
}

// springboardSpawn asks inside the device (the slow fallback poll); a var so tests can stand in for it.
var springboardSpawn = func(ctl simctl.Ctl, udid string) string {
	out, err := ctl.Spawn(udid, "launchctl", "list")
	if err != nil {
		return ""
	}
	for _, l := range strings.Split(out, "\n") {
		f := strings.Fields(l)
		if len(f) == 3 && f[2] == "com.apple.SpringBoard" && f[0] != "-" {
			return f[0]
		}
	}
	return ""
}

// guardSpringBoard waits until SpringBoard has kept one pid for 2 s of wall time, at most 30 s: on a loaded
// Mac backboardd can restart SpringBoard a few seconds after bootstatus returns, and an app installed or launched in that
// gap is refused for the rest of the boot. poll is the interval between host-side polls; a spawn poll is slow by itself.
func guardSpringBoard(ctl simctl.Ctl, deviceDir, udid string, poll time.Duration) {
	var last string
	since := time.Now()
	deadline := time.Now().Add(30 * time.Second)
	mark("guard-start")
	hostErrs := 0
	spawnOnly := false // the host sees the device's launchd_sim but never a SpringBoard child, while the device lists one
	for time.Now().Before(deadline) {
		p0 := time.Now()
		var pid, how string
		if spawnOnly {
			pid, how = springboardSpawn(ctl, udid), "spawn"
		} else {
			pid, how = springboardPID(ctl, deviceDir, udid, &hostErrs)
			// a runtime that renames SpringBoard or moves it under another parent: after 6 s without one on the host, ask
			// inside once; if the device has one, poll that way from now on
			if how == "host" && pid == "" && time.Since(since) >= 6*time.Second && last == "" {
				if sp := springboardSpawn(ctl, udid); sp != "" {
					pid, how, spawnOnly = sp, "spawn", true
				}
			}
		}
		mark("guard-poll pid=%q took=%.3f how=%s", pid, time.Since(p0).Seconds(), how)
		if how == "host-error" { // no answer: neither a pid nor its absence
			time.Sleep(poll)
			continue
		}
		if pid != "" && pid == last {
			if time.Since(since) >= 2*time.Second {
				break
			}
		} else {
			since, last = time.Now(), pid
		}
		if how != "spawn" {
			time.Sleep(poll)
		} else {
			time.Sleep(300 * time.Millisecond)
		}
	}
	mark("guard-done")
}
