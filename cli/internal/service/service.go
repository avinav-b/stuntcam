// SPDX-License-Identifier: Apache-2.0

// Package service is the notify-only background helper (design §4.6): every 2 s it looks for iOS simulators that booted
// without stuntcam (Xcode, Simulator, a restart), arms each in launchd mode at once (so launches from the command line, Expo,
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

// Armed tells whether a booted sim needs the service: Unarmed (arm it), Armed (leave this boot alone), or Pending (being
// armed by `up`/`exec` right now: look again at the next poll, don't mark the boot handled).
type Armed func(d simctl.Device) State

type State int

const (
	Unarmed State = iota
	IsArmed
	Pending
)

// ToArm returns the booted iOS sims that need arming and have not been handled for this boot yet (handled maps UDID to the
// boot time already handled, so each boot is armed and announced once).
func ToArm(ds []simctl.Device, armed Armed, handled map[string]time.Time, only ...string) []simctl.Device {
	var out []simctl.Device
	for _, d := range ds {
		if len(only) > 0 && !slices.Contains(only, d.UDID) {
			continue
		}
		if d.State != "Booted" || !strings.Contains(d.RuntimeID, "iOS") {
			continue
		}
		if t, ok := handled[d.UDID]; ok && (d.LastBooted.IsZero() || !d.LastBooted.After(t)) {
			continue
		}
		switch armed(d) {
		case IsArmed:
			handled[d.UDID] = d.LastBooted
			continue
		case Pending:
			continue
		}
		out = append(out, d)
	}
	return out
}

// Plist is the LaunchAgent that runs `<exe> service run` at login and restarts it if it fails. only = the simulators it may
// arm; empty means every iOS simulator (`--all`, which the user has to ask for: on a shared Mac other people's sims are
// booted too).
func Plist(exe string, env map[string]string, preboot string, only []string) string {
	args := []string{exe, "service", "run"}
	for _, u := range only {
		args = append(args, "--only", u)
	}
	if len(only) == 0 {
		args = append(args, "--all")
	}
	if preboot != "" {
		args = append(args, "--preboot", preboot)
	}
	var b strings.Builder
	b.WriteString(`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>` + Label + `</string>
<key>ProgramArguments</key><array>`)
	for _, a := range args {
		b.WriteString("<string>" + xmlEscape(a) + "</string>")
	}
	// restarted only after a failure, at most every 10 s
	b.WriteString("</array>\n<key>RunAtLoad</key><true/>\n<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n<key>ThrottleInterval</key><integer>10</integer>\n")
	b.WriteString("<key>StandardOutPath</key><string>" + xmlEscape(filepath.Join(home.Logs(), "service.log")) + "</string>\n")
	b.WriteString("<key>StandardErrorPath</key><string>" + xmlEscape(filepath.Join(home.Logs(), "service.log")) + "</string>\n")
	if len(env) > 0 {
		b.WriteString("<key>EnvironmentVariables</key><dict>")
		keys := make([]string, 0, len(env))
		for k := range env {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			b.WriteString("<key>" + xmlEscape(k) + "</key><string>" + xmlEscape(env[k]) + "</string>")
		}
		b.WriteString("</dict>\n")
	}
	b.WriteString("</dict></plist>\n")
	return b.String()
}

func PlistPath() string {
	d, _ := os.UserHomeDir()
	return filepath.Join(d, "Library", "LaunchAgents", Label+".plist")
}

var envRe = regexp.MustCompile(`<key>([A-Z_]+)</key><string>([^<]*)</string>`)

// InstalledEnv reads the environment of the installed LaunchAgent (nil if none is installed): `gc` keeps the build it names.
func InstalledEnv() map[string]string {
	b, err := os.ReadFile(PlistPath())
	if err != nil {
		return nil
	}
	out := map[string]string{}
	unescape := strings.NewReplacer("&lt;", "<", "&gt;", ">", "&quot;", `"`, "&amp;", "&")
	for _, m := range envRe.FindAllStringSubmatch(string(b), -1) {
		out[m[1]] = unescape.Replace(m[2])
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
		fmt.Sprintf("It booted without stuntcam. Xcode Run won't have a camera until `stuntcam up --udid %s --reboot`.", d.UDID)
}
