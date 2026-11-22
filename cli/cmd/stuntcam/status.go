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
	modeScriptArmed   statusMode = "armed"          // current but no arm record: armed by the scripts, mode unknown
	modeBootedUnarmed statusMode = "booted-unarmed" // booted, and new launches don't load stuntcam
)

// Notes more than one of statusOf's cases give.
const (
	noteDownSinceBoot = "`stuntcam down` since this boot: its boot environment still has the stub, so `stuntcam up` " +
		"re-arms it without a reboot"
	noteRebootedSinceUp = "rebooted since `stuntcam up` (by Xcode, Simulator or a restart): run " +
		"`stuntcam up --reboot` for Xcode Run"
)

// simStatus is what `status` reports for one simulator; --json prints it field by field, in this order.
type simStatus struct {
	UDID    string
	Name    string
	Runtime string
	State   string
	Mode    statusMode
	Build   string
	Xcode   string `json:",omitempty"` // the build recorded at arm time (config.json)
	// what launchd_sim's environment says: "stub", "no stub", or why it couldn't be read
	BootEnv string `json:",omitempty"`
	// the SIMCAM_SIM_DIR the boot environment names (when it has the stub)
	SimDir  string            `json:",omitempty"`
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
	// The picture its apps show, as typed in its control file by `stuntcam source` or `up --source`. For qr:TEXT the
	// text is decoded.
	// It is empty when the sim isn't armed, or when the file names no source. Apps then show the boot environment's
	// SIMCAM_SOURCE, or else the scene.
	Source string `json:",omitempty"`
}

// statusOf is what stuntcam knows about a simulator: how it is armed (Mode, Note), the build its `current` names, the
// Xcode recorded for it, its chain, latches and preflight marker. Of the sim itself it asks only its launchd
// environment, and that only for a launchd-mode record.
func statusOf(device simctl.Device) simStatus {
	status := simStatus{
		UDID:    device.UDID,
		Name:    device.Name,
		Runtime: runtimeName(device.RuntimeID),
		State:   string(device.State),
		Mode:    modeUnarmed,
	}

	_, loader, currentErr := home.ReadCurrent(device.UDID)
	if currentErr == nil {
		status.Build = filepath.Base(filepath.Dir(loader))
	}

	record, recordErr := arm.ReadRecord(device.UDID)
	if config, err := arm.ReadConfig(device.UDID); err == nil {
		status.Xcode = config.Build
	}

	boot := status.readBootEnv(device)
	status.setMode(device, record, recordErr == nil, currentErr == nil, boot)

	appsPollControlFile := status.Mode == modeBoot || status.Mode == modeLaunchd || status.Mode == modeEnv
	if appsPollControlFile {
		status.Source = typedSource(controlValue(device.UDID, "source"))
	}

	status.Chain = chainOf(device.UDID)
	status.addLatches(device.UDID) // after setMode: latches count for status.Build, which another sim dir can set
	status.PreflightFail = preflightFailOf(device.UDID, status.Build)
	return status
}

// bootArm is what a booted sim's boot environment (its launchd_sim's) says about arming. Boot mode is visible only
// there: `launchctl getenv` inside the sim reads it as empty.
type bootArm struct {
	read bool   // the environment could be read (the sim is booted and its launchd_sim found)
	stub bool   // it inserts the stub (false when unread)
	dir  string // the SIMCAM_SIM_DIR it names
}

// readBootEnv reads a booted sim's boot environment into s.BootEnv and s.SimDir.
func (s *simStatus) readBootEnv(device simctl.Device) bootArm {
	if device.State != simctl.Booted {
		return bootArm{}
	}

	env, err := bootEnv(device.UDID)
	if err != nil {
		s.BootEnv = "unreadable: " + err.Error()
		return bootArm{}
	}

	found := bootArm{read: true}
	found.stub, found.dir = procenv.Armed(env, artifacts.StubName)
	s.SimDir = found.dir
	s.BootEnv = "no stub"
	if found.stub {
		s.BootEnv = "stub"
	}

	return found
}

// setMode decides s.Mode and s.Note, and s.ArmedAt when the record's arming is in effect, from the sim's state, its
// arm record (record, if hasRecord), whether it has a `current`, and its boot environment. Each step returns once it
// has decided, so a later step starts from what the earlier ones ruled out.
func (s *simStatus) setMode(device simctl.Device, record arm.Record, hasRecord, hasCurrent bool, boot bootArm) {
	hasBootRecord := hasRecord && record.Mode == arm.Boot

	// A shut-down sim is unarmed, whatever its files say.
	if device.State != simctl.Booted {
		s.Mode = modeUnarmed
		if hasBootRecord {
			s.Note = "boot-mode arming ends at shutdown; `stuntcam up` boots it armed"
		}

		return
	}

	// A boot in progress (a live stuntcam's record) beats what the half-booted sim shows.
	if hasRecord && arm.InProgress(record) {
		s.Mode = modeBooting
		s.Note = "being booted armed by `stuntcam up` or `exec` right now"
		return
	}

	// The boot environment is what new launches get, so once read it decides over the files.
	bootEnvHasStub := boot.read && boot.stub
	if bootEnvHasStub {
		s.setModeFromBootStub(device, record, hasBootRecord, hasCurrent, boot.dir)
		return
	}

	bootEnvWithoutStub := boot.read && !boot.stub
	if bootEnvWithoutStub && hasBootRecord {
		s.Mode = modeBootedUnarmed
		s.Note = noteRebootedSinceUp // the boot record is from an earlier boot
		return
	}

	// The boot environment is unread, or it has no stub and the record (if any) isn't boot mode's.
	s.setModeFromFiles(device, record, hasRecord, hasCurrent)
}

// setModeFromBootStub decides the mode of a booted sim whose boot environment has the stub: it is in boot mode
// whatever its record says, unless `stuntcam down` has removed its `current` since this boot.
func (s *simStatus) setModeFromBootStub(device simctl.Device, record arm.Record, hasBootRecord, hasCurrent bool,
	simDir string) {
	armedFromOtherDir := simDir != "" && filepath.Clean(simDir) != home.Sim(device.UDID)
	if armedFromOtherDir {
		s.Mode = modeBoot
		s.readOtherSimDir(device, simDir)
		return
	}

	// Armed from this home's sim dir.
	if !hasCurrent {
		s.Mode = modeBootedUnarmed
		s.Note = noteDownSinceBoot
		return
	}

	s.Mode = modeBoot
	if hasBootRecord {
		s.ArmedAt = record.At
	} else {
		s.Note = "boot-armed outside the stuntcam command (scripts/boot.sh?)"
	}
}

// setModeFromFiles decides the mode of a booted sim from its `current` and arm record, when the boot environment
// can't: it is unread, or it has no stub and the record (if any) isn't boot mode's.
func (s *simStatus) setModeFromFiles(device simctl.Device, record arm.Record, hasRecord, hasCurrent bool) {
	if !hasRecord {
		s.Mode = modeBootedUnarmed
		if hasCurrent {
			s.Mode = modeScriptArmed
			s.Note = "armed outside the stuntcam command (scripts/boot.sh or launchd-arm.sh?): mode unknown"
		}

		return
	}

	if !hasCurrent {
		s.Mode = modeBootedUnarmed
		if arm.SameBoot(record, true, device.LastBooted) { // a boot record of this boot (its boot env unread)
			s.Note = noteDownSinceBoot
		}

		return
	}

	s.setModeFromRecord(device, record)
}

// setModeFromRecord decides the mode of a booted sim that has both a record and `current`, from the record's own
// mode checked against the sim: a boot record of an earlier boot, or a launchd record whose stub is gone from
// launchd's environment, is unarmed.
func (s *simStatus) setModeFromRecord(device simctl.Device, record arm.Record) {
	switch record.Mode {
	case arm.Env:
		s.Mode = modeEnv
		s.Note = "per-launch arming only (`stuntcam env`): launches without its exports get no camera"

	case arm.Boot:
		if !arm.SameBoot(record, true, device.LastBooted) {
			s.Mode = modeBootedUnarmed
			s.Note = noteRebootedSinceUp
			return
		}

		s.Mode = modeBoot
		s.ArmedAt = record.At

	case arm.Launchd:
		s.Mode = modeLaunchd
		s.ArmedAt = record.At // kept when the stub is gone: it says when the sim was armed
		if !launchdHasStub(device.UDID) {
			s.Mode = modeBootedUnarmed
			s.Note = "the stub is no longer in the simulator's launchd environment"
		}

	default: // a mode this stuntcam doesn't know: reported as the record names it
		s.Mode = statusMode(record.Mode)
		s.ArmedAt = record.At
	}
}

// launchdHasStub reports whether DYLD_INSERT_LIBRARIES in the sim's launchd environment names the stub. A failed read
// is not an error here: its output (usually none) is checked the same way.
func launchdHasStub(udid string) bool {
	inserted, _ := ctl.Getenv(udid, "DYLD_INSERT_LIBRARIES")
	return strings.Contains(inserted, artifacts.StubName)
}

// readOtherSimDir notes a sim boot-armed with another sim dir than this home's (an exec run's, or another home's) and
// takes its build and recorded Xcode from there.
func (s *simStatus) readOtherSimDir(device simctl.Device, dir string) {
	s.Note = "boot-armed with another directory, " + dir + " (a simulator `stuntcam exec`'s command booted?)"
	if filepath.Base(dir) == device.UDID {
		s.Note = "boot-armed under another stuntcam home: " + dir + " (SIMCAM_HOME differs from " + home.Root() + ")"
	}

	if data, err := os.ReadFile(filepath.Join(dir, "current")); err == nil {
		if _, loader, err := home.ParseCurrent(string(data)); err == nil {
			s.Build = filepath.Base(filepath.Dir(loader))
		}
	}

	if data, err := os.ReadFile(filepath.Join(dir, "config.json")); err == nil {
		var config arm.Config
		if json.Unmarshal(data, &config) == nil {
			s.Xcode = config.Build
		}
	}
}

// chainOf is the sim's chain file's libraries (nil without one).
func chainOf(udid string) []string {
	data, err := os.ReadFile(home.ChainFile(udid))
	if err != nil {
		return nil
	}

	var chain []string
	for _, line := range strings.Split(string(data), "\n") {
		if line = strings.TrimSpace(line); line != "" && !strings.HasPrefix(line, "#") {
			chain = append(chain, line)
		}
	}

	return chain
}

// addLatches fills s.Latched and s.LatchEffect from each app's state dir: its latched features for s.Build and its
// valid markers for s.Build.
func (s *simStatus) addLatches(udid string) {
	for _, bundle := range state.Bundles(udid) {
		var features []string
		// latches count for the build `current` names (the engine's rule)
		if bundleState, err := state.Load(udid, bundle, s.Build); err == nil {
			if latched := bundleState.LatchedFeatures(); len(latched) > 0 {
				if s.Latched == nil {
					s.Latched = map[string]string{}
				}

				s.Latched[bundle] = strings.Join(latched, ",")
				features = append(features, latched...)
			}
		}

		// a valid marker for the current build: stock without activating
		for _, marker := range state.Markers(udid, bundle) {
			if marker.Build == s.Build && marker.Valid(time.Now()) {
				if s.Latched == nil {
					s.Latched = map[string]string{}
				}

				markerText := fmt.Sprintf(" (marker: %s until %s)", strings.ReplaceAll(marker.Features, " ", ","),
					marker.Until.Format("2006-01-02 15:04"))
				s.Latched[bundle] = strings.TrimPrefix(s.Latched[bundle]+markerText, " ")
				features = append(features, strings.Fields(marker.Features)...)
			}
		}

		if len(features) > 0 {
			if s.LatchEffect == nil {
				s.LatchEffect = map[string]string{}
			}

			s.LatchEffect[bundle] = state.Effect(features)
		}
	}
}

// preflightFailOf describes the sim's valid preflight-fail marker for build ("" if none): a real preflight failure
// stocks CoreMotion for 24 h.
func preflightFailOf(udid, build string) string {
	desc := ""
	for _, fail := range state.PreflightFails(udid) {
		if fail.Build == build && fail.Valid(time.Now()) {
			desc = fmt.Sprintf("%s until %s", fail.What, fail.Until.Format("2006-01-02 15:04"))
		}
	}
	return desc
}

// statusWithFallback adds what the engine's container-tmp fallback holds. An app uses the fallback when it can't open
// its state dir, as on 17.5. Markers there keep the app stock just the same.
// Only `stuntcam status` asks, because it costs one simctl call per known bundle. The service, exec and doctor need
// only Mode, and must not pay that.
func statusWithFallback(device simctl.Device, status simStatus) simStatus {
	finder := newFallbackFinder(device)
	for _, bundle := range state.Bundles(device.UDID) {
		fallback, reason := finder.find(bundle)
		if reason != nil {
			if status.FallbackUnchecked == nil {
				status.FallbackUnchecked = map[string]string{}
			}

			status.FallbackUnchecked[bundle] = reason.Error()
			continue
		}

		desc, latched := fallback.Read(bundle, status.Build).Latches(status.Build, time.Now())
		if desc == "" {
			continue
		}

		if status.Fallback == nil {
			status.Fallback = map[string]string{}
			status.FallbackDir = map[string]string{}
		}

		status.Fallback[bundle] = desc
		status.FallbackDir[bundle] = fallback.Dir
		if len(latched) > 0 { // per location: a state-dir latch's line never borrows the fallback's effect
			if status.FallbackEffect == nil {
				status.FallbackEffect = map[string]string{}
			}

			status.FallbackEffect[bundle] = state.Effect(latched)
		}
	}

	return status
}

func cmdStatus(args []string) error {
	flags := flag.NewFlagSet("status", flag.ContinueOnError)
	asJSON := flags.Bool("json", false, "JSON output")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	devices, err := ctl.Devices()
	if err != nil {
		return err
	}

	// every booted sim, and every sim stuntcam has a directory for
	var statuses []simStatus
	for _, device := range devices {
		_, statErr := os.Stat(home.Sim(device.UDID))
		known := statErr == nil
		if device.State == simctl.Booted || known {
			statuses = append(statuses, statusWithFallback(device, statusOf(device)))
		}
	}

	if *asJSON {
		encoder := json.NewEncoder(os.Stdout)
		encoder.SetIndent("", " ")
		return encoder.Encode(statuses)
	}

	if len(statuses) == 0 {
		fmt.Println("no booted simulators and none armed before (stuntcam up)")
	}

	for _, status := range statuses {
		printStatus(status)
	}

	return nil
}

// printStatus prints one simulator's status: a summary line, then an indented line for each thing that affects it.
func printStatus(status simStatus) {
	fmt.Printf("%s (%s, %s): %s, %s", status.Name, status.UDID, status.Runtime, status.State, status.Mode)
	if status.Build != "" {
		fmt.Printf(", build %s", status.Build)
	}

	if status.Xcode != "" {
		fmt.Printf(", Xcode %s", status.Xcode)
	}

	if status.ArmedAt > 0 {
		fmt.Printf(", armed %s ago", ago(time.Since(time.Unix(status.ArmedAt, 0))))
	}

	fmt.Println()

	if status.Note != "" {
		fmt.Println("  " + status.Note)
	}

	if status.Source != "" {
		fmt.Println("  source: " + sourceText(status.Source))
	}

	for _, library := range status.Chain {
		fmt.Println("  chain: " + library)
	}

	for _, bundle := range sortedKeys(status.Latched) {
		latches := status.Latched[bundle]
		if !strings.HasPrefix(latches, "(") { // a marker's "(marker: …)" has its own brackets
			latches = "(" + latches + ")"
		}

		fmt.Printf("  latched: %s %s: %s until `stuntcam sentinel clear --bundle %s`\n", bundle, latches,
			status.LatchEffect[bundle], bundle)
	}

	for _, bundle := range sortedKeys(status.Fallback) {
		printFallback(status, bundle)
	}

	if status.PreflightFail != "" {
		fmt.Printf("  preflight failed (%s): apps get stock CoreMotion; a passing preflight or "+
			"`stuntcam sentinel clear` resets it\n", status.PreflightFail)
	}

	if len(status.FallbackUnchecked) > 0 {
		fmt.Printf("  container-tmp fallback not checked: %s\n", uncheckedText(status.FallbackUnchecked))
	}
}

// printFallback prints what bundle's container-tmp fallback holds and what it does.
func printFallback(status simStatus, bundle string) {
	held := status.Fallback[bundle]
	dir := status.FallbackDir[bundle]
	switch effect := status.FallbackEffect[bundle]; {
	case effect != "":
		fmt.Printf("  in the container-tmp fallback: %s (%s; %s): %s until "+
			"`stuntcam sentinel clear --bundle %s`\n", bundle, held, dir, effect, bundle)
	case strings.Contains(held, "preflight-fail"):
		fmt.Printf("  in the container-tmp fallback: %s (%s; %s): stock CoreMotion until "+
			"`stuntcam sentinel clear --bundle %s`\n", bundle, held, dir, bundle)
	default: // records only: the engine reads them only when the app again can't open its state dir
		fmt.Printf("  in the container-tmp fallback: %s (%s; %s): latches its next launch if it again can't "+
			"open its state dir; clear with `stuntcam sentinel clear --bundle %s`\n", bundle, held, dir, bundle)
	}
}
