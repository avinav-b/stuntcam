// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"os/exec"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// The SpringBoard guard's poll. A simulator's processes are host processes: the device's launchd_sim runs as
// `launchd_sim <set>/<udid>/data/var/run/launchd_bootstrap.plist`, and its SpringBoard is a direct child whose
// executable is …/CoreServices/SpringBoard.app/SpringBoard, with the same pid `launchctl list` shows inside the device
// (checked on iOS 26.3, 27.0 and 17.5). One `ps` takes ~40 ms where `simctl spawn … launchctl list` took 0.3–1.0 s
// under load, so the guard can poll every 150 ms and costs about its 2 s window.

// The guard's bounds.
const (
	// psTimeout bounds each ps or launchctl call of a host-side poll; one that takes longer is a failed poll.
	psTimeout = 2 * time.Second
	// hostErrorsBeforeSpawn is the number of failed host polls in a row at which the guard asks inside the device.
	hostErrorsBeforeSpawn = 3
	// springboardSteady is how long SpringBoard must keep one pid before the guard returns.
	springboardSteady = 2 * time.Second
	// guardLimit bounds the whole guard.
	guardLimit = 30 * time.Second
	// hostBlindLimit is how long the host may see no SpringBoard before the guard checks inside the device once.
	hostBlindLimit = 6 * time.Second
	// springboardPoll is the interval between host-side polls.
	springboardPoll = 150 * time.Millisecond
	// spawnPollPause is the pause after a poll inside the device (such a poll is slow by itself).
	spawnPollPause = 300 * time.Millisecond
)

// realHostProcs cheaply lists host processes in `ps -o pid=,ppid=,stat=,command=` form, for the guard's poll of
// device udid, whose directory is deviceDir. It is called through the hostProcs seam. The list has:
//   - every process by its accounting name (`ps … ucomm=`). That comes from the kernel's process table, without argv;
//   - one made-up line for the device's launchd_sim. launchd itself names its pid: `launchctl list` has the job
//     com.apple.CoreSimulator.SimDevice.<udid>.
//
// Only if launchd doesn't know that job does it read the launchd_sim processes' argv (ps -p). It avoids argv because
// a full `ps -axww … command=` reads every process's argv. Right after a boot under an xcodebuild loop, that blocked
// in the kernel for up to 10.6 s, and a kill couldn't end it sooner. Reading the launchd_sim argv blocked too.
func realHostProcs(udid, deviceDir string) (string, error) {
	names, err := runBounded(exec.Command("/bin/ps", "-ax", "-o", "pid=,ppid=,stat=,ucomm="), psTimeout)
	if err != nil {
		return "", err
	}

	var launchdSims []string
	var out strings.Builder
	for _, line := range strings.Split(names, "\n") {
		if fields := strings.Fields(line); len(fields) >= 4 && fields[3] == "launchd_sim" {
			launchdSims = append(launchdSims, fields[0])
			continue
		}

		out.WriteString(line + "\n")
	}

	if pid, known := launchdSimPID(udid); known {
		if pid != "" {
			fmt.Fprintf(&out, "%s 1 S launchd_sim %s/data/var/run/launchd_bootstrap.plist\n", pid,
				strings.TrimRight(deviceDir, "/"))
		}

		return out.String(), nil
	}

	if len(launchdSims) > 0 {
		ps := exec.Command("/bin/ps", "-ww", "-o", "pid=,ppid=,stat=,command=", "-p", strings.Join(launchdSims, ","))
		args, err := runBounded(ps, psTimeout)
		if err != nil {
			return "", err
		}

		out.WriteString(args)
	}

	return out.String(), nil
}

// launchdSimPID asks launchd for the pid of the device's launchd_sim job. The pid is "" if the job exists but isn't
// running. known is false if launchd has no such job, or if launchctl failed.
func launchdSimPID(udid string) (pid string, known bool) {
	out, err := runBounded(exec.Command("/bin/launchctl", "list"), psTimeout)
	if err != nil {
		return "", false
	}
	return parseLaunchctlList(out, udid)
}

// parseLaunchctlList finds the device's launchd_sim job in `launchctl list` output (PID, status, label per line).
func parseLaunchctlList(out, udid string) (pid string, known bool) {
	label := "com.apple.CoreSimulator.SimDevice." + udid
	for _, line := range strings.Split(out, "\n") {
		if fields := strings.Fields(line); len(fields) == 3 && fields[2] == label {
			if fields[0] == "-" {
				return "", true
			}

			return fields[0], true
		}
	}

	return "", false
}

// springboardFromPS finds the device's SpringBoard pid in hostProcs output.
// SpringBoard is the live (not zombie) child of the launchd_sim whose bootstrap plist is under deviceDir. Its name is
// SpringBoard as a ucomm, or …/SpringBoard.app/SpringBoard as a command.
// The pid is "" if there is no such launchd_sim, or if it has no SpringBoard child.
func springboardFromPS(psOutput, deviceDir string) (pid string, launchd bool) {
	type proc struct{ pid, ppid, stat, cmd string }
	var procs []proc
	for _, line := range strings.Split(psOutput, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 4 {
			continue
		}

		// the command keeps its own spaces (runtime paths have them)
		cmdStart := strings.Index(line, fields[2]) + len(fields[2])
		procs = append(procs, proc{fields[0], fields[1], fields[2], strings.TrimSpace(line[cmdStart:])})
	}

	boot := strings.TrimRight(deviceDir, "/") + "/data/var/run/launchd_bootstrap.plist"
	var launchdSim string
	for _, proc := range procs {
		plist, isLaunchdSim := strings.CutPrefix(proc.cmd, "launchd_sim ")
		if isLaunchdSim && strings.TrimSpace(plist) == boot && !strings.HasPrefix(proc.stat, "Z") {
			launchdSim = proc.pid
		}
	}

	if launchdSim == "" {
		return "", false
	}

	for _, proc := range procs {
		isSpringBoard := proc.cmd == "SpringBoard" || strings.HasSuffix(proc.cmd, "/SpringBoard.app/SpringBoard")
		if proc.ppid == launchdSim && !strings.HasPrefix(proc.stat, "Z") && isSpringBoard {
			return proc.pid, true
		}
	}

	return "", true
}

// springboardPID is the guard's poll. how says which side answered.
//   - It asks the host side first. Only if the host can't see the device's launchd_sim at all does it ask inside the
//     device. That happens when ps failed, or with a runtime laid out differently.
//   - A launchd_sim without a SpringBoard child is an answer ("not running yet"), not a reason to spawn. Right after
//     bootstatus, a spawn poll found nothing the next host poll didn't (29 of 31). And under an xcodebuild loop, one
//     `simctl spawn` took 9.7-11.3 s on 27.0.
//   - A host poll that fails, as when ps timed out, is no answer ("host-error"). Only after hostErrorsBeforeSpawn
//     failures in a row does it fall back to spawn. hostErrs counts those failures across calls.
func springboardPID(ctl simctl.Ctl, deviceDir, udid string, hostErrs *int) (pid, how string) {
	ps, err := hostProcs(udid, deviceDir)
	if err == nil {
		*hostErrs = 0
		if found, launchd := springboardFromPS(ps, deviceDir); found != "" || launchd {
			return found, "host"
		}
	} else {
		*hostErrs++
		if *hostErrs < hostErrorsBeforeSpawn {
			return "", "host-error"
		}
	}

	return springboardSpawn(ctl, udid), "spawn"
}

// realSpringboardSpawn asks inside the device (the slow fallback poll). Called through the springboardSpawn seam.
func realSpringboardSpawn(ctl simctl.Ctl, udid string) string {
	out, err := ctl.Spawn(udid, "launchctl", "list")
	if err != nil {
		return ""
	}

	for _, line := range strings.Split(out, "\n") {
		fields := strings.Fields(line)
		if len(fields) == 3 && fields[2] == "com.apple.SpringBoard" && fields[0] != "-" {
			return fields[0]
		}
	}

	return ""
}

// guardSpringBoard waits until SpringBoard has kept one pid for springboardSteady of wall time, at most guardLimit: on
// a loaded Mac backboardd can restart SpringBoard a few seconds after bootstatus returns, and an app installed or
// launched in that gap is refused for the rest of the boot. poll is the interval between host-side polls; a spawn poll
// is slow by itself.
func guardSpringBoard(ctl simctl.Ctl, deviceDir, udid string, poll time.Duration) {
	var last string
	since := time.Now()
	deadline := time.Now().Add(guardLimit)
	mark("guard-start")
	hostErrs := 0
	spawnOnly := false // the host sees the device's launchd_sim but never a SpringBoard child, while the device lists one

	for time.Now().Before(deadline) {
		pollStart := time.Now()
		var pid, how string
		if spawnOnly {
			pid, how = springboardSpawn(ctl, udid), "spawn"
		} else {
			pid, how = springboardPID(ctl, deviceDir, udid, &hostErrs)
			// a runtime that renames SpringBoard or moves it under another parent: after hostBlindLimit without one on
			// the host, ask inside once; if the device has one, poll that way from now on
			if how == "host" && pid == "" && time.Since(since) >= hostBlindLimit && last == "" {
				if spawned := springboardSpawn(ctl, udid); spawned != "" {
					pid, how, spawnOnly = spawned, "spawn", true
				}
			}
		}

		mark("guard-poll pid=%q took=%.3f how=%s", pid, time.Since(pollStart).Seconds(), how)
		if how == "host-error" { // no answer: neither a pid nor its absence
			time.Sleep(poll)
			continue
		}

		if pid != "" && pid == last {
			if time.Since(since) >= springboardSteady {
				break
			}
		} else {
			since, last = time.Now(), pid
		}

		if how != "spawn" {
			time.Sleep(poll)
		} else {
			time.Sleep(spawnPollPause)
		}
	}

	mark("guard-done")
}
