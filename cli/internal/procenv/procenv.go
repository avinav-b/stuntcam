// SPDX-License-Identifier: Apache-2.0

// Package procenv reads a booted simulator's boot environment from the host: the environment of its launchd_sim process
// (KERN_PROCARGS2). Boot-mode arming lives only there: `launchctl getenv` inside the sim reads every arming variable as
// empty, and a reboot without stuntcam clears it. Reading it adds no process to the simulator.
package procenv

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"errors"
	"os/exec"
	"strconv"
	"strings"
)

// LaunchdSimPid finds the launchd_sim of a device in `ps -axww -o pid=,args=` output: its only argument is the device's
// …/Devices/<udid>/data/var/run/launchd_bootstrap.plist (any device set). The line is matched as a whole after the pid,
// since a device set under ~/Library/Application Support (and an Xcode at /Applications/Xcode 27.app) has spaces in its
// path.
func LaunchdSimPid(psOut, udid string) (int, bool) {
	plist := "/" + udid + "/data/var/run/launchd_bootstrap.plist"
	scanner := bufio.NewScanner(strings.NewReader(psOut))
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	for scanner.Scan() {
		pidStr, rest, ok := strings.Cut(strings.TrimSpace(scanner.Text()), " ")
		rest = strings.TrimSpace(rest)
		if !ok || !strings.HasSuffix(rest, plist) {
			continue
		}

		exe := strings.TrimSuffix(rest, plist)
		if i := strings.Index(exe, "launchd_sim "); i < 0 || i > 0 && exe[i-1] != '/' {
			continue // e.g. a grep/pgrep for the path: launchd_sim must be the executable, its file name exactly
		}

		if pid, err := strconv.Atoi(pidStr); err == nil {
			return pid, true
		}
	}

	return 0, false
}

// ParseProcargs2 splits a KERN_PROCARGS2 buffer: int32 argc, the exec path, NUL padding, argc arguments, then the
// environment (NUL-terminated strings up to an empty one).
func ParseProcargs2(raw []byte) (args, env []string, err error) {
	if len(raw) < 4 {
		return nil, nil, errors.New("procargs2: short buffer")
	}

	argc := int(binary.LittleEndian.Uint32(raw[:4]))
	rest := raw[4:]
	i := bytes.IndexByte(rest, 0) // exec path
	if i < 0 {
		return nil, nil, errors.New("procargs2: no exec path")
	}

	rest = rest[i:]
	for len(rest) > 0 && rest[0] == 0 {
		rest = rest[1:]
	}

	var strs []string
	for len(rest) > 0 {
		i := bytes.IndexByte(rest, 0)
		if i < 0 {
			i = len(rest)
		}

		if i == 0 {
			if len(strs) >= argc {
				break // the empty string after the environment
			}
		}

		strs = append(strs, string(rest[:i]))
		if i == len(rest) {
			break
		}

		rest = rest[i+1:]
	}

	if len(strs) < argc {
		return strs, nil, errors.New("procargs2: fewer arguments than argc")
	}

	return strs[:argc], strs[argc:], nil
}

// Map turns KEY=value strings into a map.
func Map(env []string) map[string]string {
	vars := map[string]string{}
	for _, kv := range env {
		if k, v, ok := strings.Cut(kv, "="); ok && k != "" {
			vars[k] = v
		}
	}
	return vars
}

// BootEnv is the boot environment of a booted device (its launchd_sim's environment).
func BootEnv(udid string) (map[string]string, error) {
	out, err := exec.Command("ps", "-axww", "-o", "pid=,args=").Output()
	if err != nil {
		return nil, err
	}

	pid, ok := LaunchdSimPid(string(out), udid)
	if !ok {
		return nil, errors.New("no launchd_sim for " + udid)
	}

	b, err := procargs2(pid)
	if err != nil {
		return nil, err
	}

	_, env, err := ParseProcargs2(b)
	return Map(env), err
}

// Armed says what a boot environment arms: the stub in DYLD_INSERT_LIBRARIES and the sim dir it names (SIMCAM_SIM_DIR).
func Armed(env map[string]string, stubName string) (armed bool, simDir string) {
	for _, p := range strings.Split(env["DYLD_INSERT_LIBRARIES"], ":") {
		if strings.HasSuffix(p, "/"+stubName) {
			return true, env["SIMCAM_SIM_DIR"]
		}
	}
	return false, ""
}

// ExecPath is pid's executable path (the first string of KERN_PROCARGS2), or "" if it can't be read.
func ExecPath(pid int) string {
	b, err := procargs2(pid)
	if err != nil || len(b) < 4 {
		return ""
	}
	rest := b[4:]
	if i := bytes.IndexByte(rest, 0); i > 0 {
		return string(rest[:i])
	}
	return ""
}
