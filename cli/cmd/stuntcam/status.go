// SPDX-License-Identifier: Apache-2.0

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/procenv"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
	"github.com/avinav-b/stuntcam/cli/internal/state"
)

// statusMode is how a simulator is armed, as `status` reports it (simStatus.Mode; the same strings in --json).
type statusMode string

const (
	modeUnarmed       statusMode = "unarmed"        // shut down
	modeBooting       statusMode = "booting"        // being booted armed by `up` or `exec` right now
	modeBoot          statusMode = "boot"           // its boot environment has the stub
	modeLaunchd       statusMode = "launchd"        // armed in launchd mode, the stub in launchd's environment
	modeEnv           statusMode = "env"            // per-launch arming only (`stuntcam env`, exec's runner hook)
	modeScriptArmed   statusMode = "armed"          // a current without an arm record: armed by the scripts, mode unknown
	modeBootedUnarmed statusMode = "booted-unarmed" // booted, and new launches don't load stuntcam
)

// Notes more than one of statusOf's cases give.
const (
	noteDownSinceBoot = "`stuntcam down` since this boot: its boot environment still has the stub, so `stuntcam up` " +
		"re-arms it without a reboot"
	noteRebootedSinceUp = "rebooted since `stuntcam up` (by Xcode, Simulator or a restart): run `stuntcam up --reboot` " +
		"for Xcode Run"
)

type simStatus struct {
	UDID, Name, Runtime, State string
	Mode                       statusMode
	Build                      string
	Xcode                      string `json:",omitempty"` // the build recorded at arm time (config.json)
	// what launchd_sim's environment says: "stub", "no stub", or why it couldn't be read
	BootEnv string            `json:",omitempty"`
	SimDir  string            `json:",omitempty"` // the SIMCAM_SIM_DIR the boot environment names (when it has the stub)
	ArmedAt int64             `json:",omitempty"`
	Chain   []string          `json:",omitempty"`
	Latched map[string]string `json:",omitempty"` // bundle -> features
	// bundle -> what the latch does (motion only stocks CoreMotion)
	LatchEffect map[string]string `json:",omitempty"`
	// a valid preflight-fail marker of the current build: CoreMotion stock
	PreflightFail string `json:",omitempty"`
	// bundle -> what its container-tmp fallback latches or stocks (engine: state dir not openable)
	Fallback    map[string]string `json:",omitempty"`
	FallbackDir map[string]string `json:",omitempty"` // bundle -> that fallback's directory
	// bundle -> what that fallback's markers do at the next launch ("" = nothing unless it falls back again)
	FallbackEffect map[string]string `json:",omitempty"`
	// bundle -> why its container tmp wasn't checked (not booted, not installed)
	FallbackUnchecked map[string]string `json:",omitempty"`
	Note              string            `json:",omitempty"`
}

// statusOf is what stuntcam knows about a simulator: how it is armed (Mode, Note), the build its `current` names, the
// Xcode recorded for it, its chain, latches and preflight marker. Of the sim itself it asks only its launchd
// environment, and that only for a launchd-mode record.
func statusOf(d simctl.Device) simStatus {
	s := simStatus{UDID: d.UDID, Name: d.Name, Runtime: runtimeName(d.RuntimeID), State: d.State, Mode: modeUnarmed}
	_, loader, currentErr := home.ReadCurrent(d.UDID)
	if currentErr == nil {
		s.Build = filepath.Base(filepath.Dir(loader))
	}
	rec, recErr := arm.ReadRecord(d.UDID)
	if c, err := arm.ReadConfig(d.UDID); err == nil {
		s.Xcode = c.Build
	}
	boot := s.readBootEnv(d)
	s.setMode(d, rec, recErr == nil, currentErr == nil, boot)
	s.Chain = chainOf(d.UDID)
	s.addLatches(d.UDID) // after setMode: they count for s.Build, which another sim dir's current can set
	s.PreflightFail = preflightFailOf(d.UDID, s.Build)
	return s
}

// bootArm is what a booted sim's boot environment (its launchd_sim's) says about arming. Boot mode is visible only
// there: `launchctl getenv` inside the sim reads it as empty.
type bootArm struct {
	read bool   // the environment could be read (the sim is booted and its launchd_sim found)
	stub bool   // it inserts the stub (false when unread)
	dir  string // the SIMCAM_SIM_DIR it names
}

// readBootEnv reads a booted sim's boot environment into s.BootEnv and s.SimDir.
func (s *simStatus) readBootEnv(d simctl.Device) bootArm {
	if d.State != "Booted" {
		return bootArm{}
	}
	env, err := bootEnv(d.UDID)
	if err != nil {
		s.BootEnv = "unreadable: " + err.Error()
		return bootArm{}
	}
	b := bootArm{read: true}
	b.stub, b.dir = procenv.Armed(env, artifacts.StubName)
	s.SimDir = b.dir
	s.BootEnv = "no stub"
	if b.stub {
		s.BootEnv = "stub"
	}
	return b
}

// setMode decides s.Mode and s.Note from the sim's state, its arm record (rec, if hasRecord), whether it has a
// `current`, and its boot environment. The cases are tried in order and the first that holds wins, so each one may rely
// on every earlier one having failed:
//   - a shut-down sim is unarmed, whatever its files say;
//   - a boot in progress (a live stuntcam's record) beats what the half-booted sim shows;
//   - then the boot environment, which is what new launches get: with the stub there the sim is in boot mode (or was
//     disarmed with `down` since this boot, when current is gone), whatever its record says; without it, a boot record
//     is from an earlier boot;
//   - then current and the record: current without a record is the scripts' arming, an env record is per-launch arming,
//     a missing record or current is unarmed;
//   - last, the record's own mode, checked against the sim: a boot record of an earlier boot, or a launchd record whose
//     stub is gone from launchd's environment, is unarmed.
func (s *simStatus) setMode(d simctl.Device, rec arm.Record, hasRecord, hasCurrent bool, boot bootArm) {
	switch {
	case d.State != "Booted":
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

// readOtherSimDir notes a sim boot-armed with another sim dir than this home's (an exec run's, or another home's) and
// takes its build and recorded Xcode from there.
func (s *simStatus) readOtherSimDir(d simctl.Device, dir string) {
	s.Note = "boot-armed with another directory, " + dir + " (a simulator `stuntcam exec`'s command booted?)"
	if filepath.Base(dir) == d.UDID {
		s.Note = "boot-armed under another stuntcam home: " + dir + " (SIMCAM_HOME differs from " + home.Root() + ")"
	}
	if b, err := os.ReadFile(filepath.Join(dir, "current")); err == nil {
		if _, l, err := home.ParseCurrent(string(b)); err == nil {
			s.Build = filepath.Base(filepath.Dir(l))
		}
	}
	if b, err := os.ReadFile(filepath.Join(dir, "config.json")); err == nil {
		var c arm.Config
		if json.Unmarshal(b, &c) == nil {
			s.Xcode = c.Build
		}
	}
}

// chainOf is the sim's chain file's libraries (nil without one).
func chainOf(udid string) []string {
	b, err := os.ReadFile(home.ChainFile(udid))
	if err != nil {
		return nil
	}
	var chain []string
	for _, l := range strings.Split(string(b), "\n") {
		if l = strings.TrimSpace(l); l != "" && !strings.HasPrefix(l, "#") {
			chain = append(chain, l)
		}
	}
	return chain
}

// addLatches fills s.Latched and s.LatchEffect from each app's state dir: its latched features for s.Build and its
// valid markers for s.Build.
func (s *simStatus) addLatches(udid string) {
	for _, b := range state.Bundles(udid) {
		var feats []string
		// latches count for the build `current` names (the engine's rule)
		if a, err := state.Load(udid, b, s.Build); err == nil {
			if f := a.LatchedFeatures(); len(f) > 0 {
				if s.Latched == nil {
					s.Latched = map[string]string{}
				}
				s.Latched[b] = strings.Join(f, ",")
				feats = append(feats, f...)
			}
		}
		for _, m := range state.Markers(udid, b) { // a valid marker for the current build: stock without activating
			if m.Build == s.Build && m.Valid(time.Now()) {
				if s.Latched == nil {
					s.Latched = map[string]string{}
				}
				marker := fmt.Sprintf(" (marker: %s until %s)", strings.ReplaceAll(m.Features, " ", ","),
					m.Until.Format("2006-01-02 15:04"))
				s.Latched[b] = strings.TrimPrefix(s.Latched[b]+marker, " ")
				feats = append(feats, strings.Fields(m.Features)...)
			}
		}
		if len(feats) > 0 {
			if s.LatchEffect == nil {
				s.LatchEffect = map[string]string{}
			}
			s.LatchEffect[b] = state.Effect(feats)
		}
	}
}

// preflightFailOf describes the sim's valid preflight-fail marker for build ("" if none): a real preflight failure
// stocks CoreMotion for 24 h.
func preflightFailOf(udid, build string) string {
	desc := ""
	for _, p := range state.PreflightFails(udid) {
		if p.Build == build && p.Valid(time.Now()) {
			desc = fmt.Sprintf("%s until %s", p.What, p.Until.Format("2006-01-02 15:04"))
		}
	}
	return desc
}

// statusWithFallback adds what the engine's container-tmp fallback holds (the app couldn't open its state dir, e.g. on
// 17.5): markers there keep the app stock just the same. Only `stuntcam status` asks: it costs one simctl call per
// known bundle, which the service, exec and doctor (they need only Mode) must not pay.
func statusWithFallback(d simctl.Device, s simStatus) simStatus {
	for _, b := range state.Bundles(d.UDID) {
		f, why := fallbackOf(d, b)
		if why != "" {
			if s.FallbackUnchecked == nil {
				s.FallbackUnchecked = map[string]string{}
			}
			s.FallbackUnchecked[b] = why
			continue
		}
		desc, ff := f.Read(b, s.Build).Latches(s.Build, time.Now())
		if desc == "" {
			continue
		}
		if s.Fallback == nil {
			s.Fallback, s.FallbackDir = map[string]string{}, map[string]string{}
		}
		s.Fallback[b], s.FallbackDir[b] = desc, f.Dir
		if len(ff) > 0 { // per location: a state-dir latch's line never borrows the fallback's effect
			if s.FallbackEffect == nil {
				s.FallbackEffect = map[string]string{}
			}
			s.FallbackEffect[b] = state.Effect(ff)
		}
	}
	return s
}

func cmdStatus(args []string) error {
	fs := flag.NewFlagSet("status", flag.ContinueOnError)
	asJSON := fs.Bool("json", false, "JSON output")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	ds, err := ctl.Devices()
	if err != nil {
		return err
	}
	var out []simStatus
	for _, d := range ds {
		_, known := os.Stat(home.Sim(d.UDID))
		if d.State == "Booted" || known == nil {
			out = append(out, statusWithFallback(d, statusOf(d)))
		}
	}
	if *asJSON {
		e := json.NewEncoder(os.Stdout)
		e.SetIndent("", " ")
		return e.Encode(out)
	}
	if len(out) == 0 {
		fmt.Println("no booted simulators and none armed before (stuntcam up)")
	}
	for _, s := range out {
		fmt.Printf("%s (%s, %s): %s, %s", s.Name, s.UDID, s.Runtime, s.State, s.Mode)
		if s.Build != "" {
			fmt.Printf(", build %s", s.Build)
		}
		if s.Xcode != "" {
			fmt.Printf(", Xcode %s", s.Xcode)
		}
		if s.ArmedAt > 0 {
			fmt.Printf(", armed %s ago", ago(time.Since(time.Unix(s.ArmedAt, 0))))
		}
		fmt.Println()
		if s.Note != "" {
			fmt.Println("  " + s.Note)
		}
		for _, c := range s.Chain {
			fmt.Println("  chain: " + c)
		}
		for _, b := range sortedKeys(s.Latched) {
			latches := s.Latched[b]
			if !strings.HasPrefix(latches, "(") { // a marker's "(marker: …)" has its own brackets
				latches = "(" + latches + ")"
			}
			fmt.Printf("  latched: %s %s: %s until `stuntcam sentinel clear --bundle %s`\n", b, latches, s.LatchEffect[b], b)
		}
		for _, b := range sortedKeys(s.Fallback) {
			switch eff := s.FallbackEffect[b]; {
			case eff != "":
				fmt.Printf("  in the container-tmp fallback: %s (%s; %s): %s until `stuntcam sentinel clear --bundle %s`\n",
					b, s.Fallback[b], s.FallbackDir[b], eff, b)
			case strings.Contains(s.Fallback[b], "preflight-fail"):
				fmt.Printf("  in the container-tmp fallback: %s (%s; %s): stock CoreMotion until `stuntcam sentinel clear "+
					"--bundle %s`\n", b, s.Fallback[b], s.FallbackDir[b], b)
			default: // records only: the engine reads them only when the app again can't open its state dir
				fmt.Printf("  in the container-tmp fallback: %s (%s; %s): latches its next launch if it again can't open its "+
					"state dir; clear with `stuntcam sentinel clear --bundle %s`\n", b, s.Fallback[b], s.FallbackDir[b], b)
			}
		}
		if s.PreflightFail != "" {
			fmt.Printf("  preflight failed (%s): apps get stock CoreMotion; a passing preflight or `stuntcam sentinel clear` "+
				"resets it\n", s.PreflightFail)
		}
		if len(s.FallbackUnchecked) > 0 {
			fmt.Printf("  container-tmp fallback not checked: %s\n", uncheckedText(s.FallbackUnchecked))
		}
	}
	return nil
}
