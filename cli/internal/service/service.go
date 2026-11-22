// SPDX-License-Identifier: Apache-2.0

// Package service is the notify-only background helper: every 2 s it looks for iOS simulators that booted without
// stuntcam (Xcode, Simulator, a restart), arms each in launchd mode at once (so launches from the command line, Expo,
// Flutter, Maestro get the camera), and posts one notification saying Xcode Run needs `stuntcam up`. It never reboots.
package service

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

const Label = "dev.stuntcam.service"

// Armed tells whether a booted sim needs the service:
//   - Unarmed: arm it.
//   - Armed: leave this boot alone.
//   - Pending: `up` or `exec` is arming it right now. Look again at the next poll, and don't mark the boot handled.
type Armed func(d simctl.Device) State

type State int

const (
	Unarmed State = iota
	IsArmed
	Pending
)

// ToArm returns the booted iOS sims that need arming and have not been handled for this boot yet (handled maps UDID to
// the boot time already handled, so each boot is armed and announced once).
func ToArm(devices []simctl.Device, armed Armed, handled map[string]time.Time, only ...string) []simctl.Device {
	var out []simctl.Device
	for _, device := range devices {
		if len(only) > 0 && !slices.Contains(only, device.UDID) {
			continue
		}

		if device.State != simctl.Booted || !strings.Contains(device.RuntimeID, "iOS") {
			continue
		}

		handledBoot, wasHandled := handled[device.UDID]
		if wasHandled && (device.LastBooted.IsZero() || !device.LastBooted.After(handledBoot)) {
			continue
		}

		switch armed(device) {
		case IsArmed:
			handled[device.UDID] = device.LastBooted
			continue
		case Pending:
			continue
		}

		out = append(out, device)
	}

	return out
}

// Plist is the LaunchAgent that runs `<exe> service run` at login and restarts it if it fails. only = the simulators it
// may arm; empty means every iOS simulator (`--all`, which the user has to ask for: on a shared Mac other people's sims
// are booted too).
func Plist(exe string, env map[string]string, preboot string, only []string) string {
	args := []string{exe, "service", "run"}
	for _, udid := range only {
		args = append(args, "--only", udid)
	}

	if len(only) == 0 {
		args = append(args, "--all")
	}

	if preboot != "" {
		args = append(args, "--preboot", preboot)
	}

	var plist strings.Builder
	plist.WriteString(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>` + Label + `</string>
<key>ProgramArguments</key><array>`)
	for _, a := range args {
		plist.WriteString("<string>" + xmlEscape(a) + "</string>")
	}

	// restarted only after a failure, at most every 10 s
	plist.WriteString("</array>\n<key>RunAtLoad</key><true/>\n<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n<key>ThrottleInterval</key><integer>10</integer>\n")
	plist.WriteString("<key>StandardOutPath</key><string>" + xmlEscape(filepath.Join(home.Logs(), "service.log")) + "</string>\n")
	plist.WriteString("<key>StandardErrorPath</key><string>" + xmlEscape(filepath.Join(home.Logs(), "service.log")) + "</string>\n")
	if len(env) > 0 {
		plist.WriteString("<key>EnvironmentVariables</key><dict>")
		keys := make([]string, 0, len(env))
		for k := range env {
			keys = append(keys, k)
		}

		sort.Strings(keys)
		for _, k := range keys {
			plist.WriteString("<key>" + xmlEscape(k) + "</key><string>" + xmlEscape(env[k]) + "</string>")
		}

		plist.WriteString("</dict>\n")
	}

	plist.WriteString("</dict></plist>\n")
	return plist.String()
}

func PlistPath() string {
	d, _ := os.UserHomeDir() // fails only without $HOME
	return filepath.Join(d, "Library", "LaunchAgents", Label+".plist")
}

var envRe = regexp.MustCompile(`<key>([A-Z_]+)</key><string>([^<]*)</string>`)

// InstalledEnv reads the environment of the installed LaunchAgent (nil if none is installed): `gc` keeps the build it
// names.
func InstalledEnv() map[string]string {
	data, err := os.ReadFile(PlistPath())
	if err != nil {
		return nil
	}

	out := map[string]string{}
	unescape := strings.NewReplacer("&lt;", "<", "&gt;", ">", "&quot;", `"`, "&amp;", "&")
	for _, match := range envRe.FindAllStringSubmatch(string(data), -1) {
		out[match[1]] = unescape.Replace(match[2])
	}

	return out
}

func xmlEscape(s string) string {
	r := strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;", `"`, "&quot;")
	return r.Replace(s)
}

// Notice is the one notification per unarmed boot.
func Notice(d simctl.Device) (title, body string) {
	return "stuntcam: " + d.Name + " has a camera for command-line launches",
		fmt.Sprintf("It booted without stuntcam. Xcode Run won't have a camera until `stuntcam up --udid %s --reboot`.",
			d.UDID)
}
