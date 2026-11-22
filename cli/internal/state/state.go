// SPDX-License-Identifier: Apache-2.0

// Package state reads what the engine records per app (its crash sentinel, engine/src/core/safety.inc and the files
// after it): one events.log per bundle id under sims/<udid>/state/<bundle>/, lines "<epoch> <activation#> <kind>
// <feature> <build> <detail…>". Latch rules match the engine's: two crash or hang events of one feature, same build,
// within the last 20 activations and 24 h.
package state

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

type Event struct {
	At         int64
	Activation int
	Kind       string
	Feature    string
	Build      string
	Detail     string
}

// App is one bundle's sentinel state.
type App struct {
	Bundle          string
	Activations     int            // since the last clear
	LastActivation  *Event         // the most recent activate line
	Latched         map[string]int // feature -> counted crash/hang events (>= 2 means latched at the next activation)
	Counted         []Event        // the events that count toward a latch
	UnknownDebugEnd string         // feature of an op a traced run ended inside (reported, never counted)
}

func EventsFile(udid, bundle string) string {
	return filepath.Join(home.StateDir(udid), bundle, "events.log")
}

// Bundles lists the apps that have state on a sim.
func Bundles(udid string) []string {
	entries, _ := os.ReadDir(home.StateDir(udid)) // no state dir: no bundles
	var out []string
	for _, entry := range entries {
		if entry.IsDir() {
			out = append(out, entry.Name())
		}
	}
	sort.Strings(out)
	return out
}

func Parse(lines []string) []Event {
	var out []Event
	for _, line := range lines {
		fields := strings.Fields(line)
		if len(fields) < 5 {
			continue
		}

		at, err1 := strconv.ParseInt(fields[0], 10, 64)
		act, err2 := strconv.Atoi(fields[1])
		if err1 != nil || err2 != nil {
			continue
		}

		out = append(out, Event{At: at, Activation: act, Kind: fields[2], Feature: fields[3], Build: fields[4],
			Detail: strings.Join(fields[5:], " ")})
	}

	return out
}

// The window in which the engine counts a build's crashes and hangs towards latching a feature: the last
// latchWindowActivations activations, and no older than latchWindowAge.
const (
	latchWindowActivations = 20
	latchWindowAge         = 24 * time.Hour
)

// Analyze applies the engine's rules as of now for the given build ("" = the last activation's build).
func Analyze(bundle string, all []Event, build string, now time.Time) App {
	app := App{Bundle: bundle, Latched: map[string]int{}}
	maxAct := 0
	var past []Event
	for _, event := range all {
		if event.Activation > maxAct {
			maxAct = event.Activation
		}

		if event.Kind == "clear" {
			past = past[:0]
			continue
		}

		past = append(past, event)
	}

	lastAct := -1
	for i, event := range past {
		if event.Kind == "activate" {
			app.Activations++
			lastAct = i
		}
	}

	if lastAct >= 0 {
		activation := past[lastAct]
		app.LastActivation = &activation
		if build == "" {
			build = activation.Build
		}

		// the engine compares the field exactly ("latched=1 traced=…" isn't traced)
		traced := firstField(activation.Detail) == "traced=1"
		ended := false
		open := map[string]string{}
		for _, event := range past[lastAct+1:] {
			switch {
			case event.Kind == "exit" || strings.HasPrefix(event.Kind, "crash"):
				ended = true
			case event.Kind == "op-begin":
				open[event.Detail] = event.Feature
			case event.Kind == "op-end":
				delete(open, event.Detail)
			}
		}

		if traced && !ended {
			for _, feature := range open {
				app.UnknownDebugEnd = feature
			}
		}
	}

	next := maxAct + 1
	for _, event := range past {
		if (event.Kind != "crash" && event.Kind != "hang") || event.Build != build {
			continue
		}

		if event.Activation <= next-latchWindowActivations || event.At < now.Add(-latchWindowAge).Unix() {
			continue
		}

		app.Latched[featureOf(event.Feature)]++
		app.Counted = append(app.Counted, event)
	}

	return app
}

// Load reads and analyzes one bundle's events on a sim.
func Load(udid, bundle, build string) (App, error) {
	return LoadFile(EventsFile(udid, bundle), bundle, build)
}

// LoadFile reads and analyzes one events.log (the state dir's, or a container-tmp fallback's).
func LoadFile(path, bundle, build string) (App, error) {
	file, err := os.Open(path)
	if err != nil {
		return App{Bundle: bundle, Latched: map[string]int{}}, err
	}
	defer file.Close()

	var lines []string
	sc := bufio.NewScanner(file)
	for sc.Scan() {
		lines = append(lines, sc.Text())
	}

	return Analyze(bundle, Parse(lines), build, time.Now()), sc.Err()
}

// ClosesCamera reports whether a latch of this feature closes the camera. A `motion` latch only makes CoreMotion
// stock from the next launch; the camera is unaffected.
func ClosesCamera(feature string) bool { return feature != "motion" }

// Effect says in words what a set of latched features does to the app's next launches.
func Effect(features []string) string {
	camera, motion := false, false
	for _, f := range features {
		if ClosesCamera(f) {
			camera = true
		} else {
			motion = true
		}
	}

	switch {
	case camera:
		return "no camera and stock CoreMotion"
	case motion:
		return "stock CoreMotion (the camera is unaffected)"
	}

	return "nothing"
}

// LatchedFeatures lists features that will be latched at the app's next activation.
func (a App) LatchedFeatures() []string {
	var out []string
	for f, n := range a.Latched {
		if n >= 2 {
			out = append(out, f)
		}
	}
	sort.Strings(out)
	return out
}

// Marker is a file the engine writes in the bundle's state dir. Its mtime is the time it stops counting.
//   - latched-<build> holds the latched camera features of one build. While it is valid, that build's next launches
//     get the stock camera without activating, and stock CoreMotion.
//   - latched-motion-<build> holds `motion`. It makes CoreMotion stock and leaves the camera alone.
type Marker struct {
	Build    string
	Features string
	Until    time.Time
	Motion   bool // latched-motion-<build>
}

// Name is the marker's file name.
func (m Marker) Name() string {
	if m.Motion {
		return "latched-motion-" + m.Build
	}
	return "latched-" + m.Build
}

// Effect says what a valid marker does to its build's launches.
func (m Marker) Effect() string {
	if m.Motion {
		return "launches of this build get stock CoreMotion; the camera is unaffected"
	}
	return "launches of this build start stock (no camera, stock CoreMotion), no activation"
}

func (m Marker) Valid(now time.Time) bool { return m.Until.After(now) }

// Markers lists a bundle's latched-<build> and latched-motion-<build> markers.
func Markers(udid, bundle string) []Marker {
	return MarkersIn(filepath.Join(home.StateDir(udid), bundle))
}

// MarkersIn lists the latched-* markers in one bundle directory (the state dir's or a container-tmp fallback's).
func MarkersIn(dir string) []Marker {
	entries, _ := os.ReadDir(dir) // no dir: no markers
	var out []Marker
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, "latched-") || entry.IsDir() {
			continue
		}

		info, err := entry.Info()
		if err != nil {
			continue
		}

		data, _ := os.ReadFile(filepath.Join(dir, name)) // an unreadable marker still counts, with no features
		marker := Marker{Build: strings.TrimPrefix(name, "latched-"), Features: strings.TrimSpace(string(data)),
			Until: info.ModTime()}
		if strings.HasPrefix(name, "latched-motion-") {
			marker.Build, marker.Motion = strings.TrimPrefix(name, "latched-motion-"), true
		}

		out = append(out, marker)
	}

	return out
}

// PreflightFail is a sim's preflight-fail-<build> marker. It records a real preflight failure of that build, and is
// valid for 24 h, until its mtime.
// While it is valid, that build's launches on the sim get stock CoreMotion. The camera runs its own preflight at every
// launch.
// A passing preflight deletes it.
type PreflightFail struct {
	Build string
	What  string
	Until time.Time
}

func (p PreflightFail) Valid(now time.Time) bool { return p.Until.After(now) }

// PreflightFails lists a sim's preflight-fail-<build> markers.
func PreflightFails(udid string) []PreflightFail { return PreflightFailsIn(home.StateDir(udid)) }

// PreflightFailsIn lists the preflight-fail-<build> markers in one root (the state dir, or a container's
// tmp/simcam-state).
func PreflightFailsIn(root string) []PreflightFail {
	var out []PreflightFail
	entries, _ := os.ReadDir(root) // no dir: no markers
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, "preflight-fail-") || entry.IsDir() {
			continue
		}

		info, err := entry.Info()
		if err != nil {
			continue
		}

		data, _ := os.ReadFile(filepath.Join(root, name)) // an unreadable marker still counts, with no text
		out = append(out, PreflightFail{
			Build: strings.TrimPrefix(name, "preflight-fail-"),
			What:  strings.TrimSpace(string(data)),
			Until: info.ModTime(),
		})
	}

	return out
}

// Clear forgets the past for one bundle (the engine's own `SIMCAM_SENTINEL=clear` writes the same line) and, as that
// does, removes the bundle's latched-* markers and the sim's preflight-fail-* markers.
func Clear(udid, bundle, reason string) error {
	if bundle == "" || bundle == "." || bundle == ".." || strings.ContainsAny(bundle, "/\x00") {
		return fmt.Errorf("bad bundle id %q", bundle)
	}

	dir := filepath.Join(home.StateDir(udid), bundle)
	// the clear line first, as the engine's SCClearAtLoad does
	eventsPath := EventsFile(udid, bundle)
	appendErr := os.MkdirAll(filepath.Dir(eventsPath), 0o755)
	if appendErr == nil {
		// the markers go even if this fails, as in SCClearAtLoad; the error is returned
		appendErr = AppendClear(eventsPath, reason, true)
	}

	if entries, err := os.ReadDir(dir); err == nil {
		for _, entry := range entries {
			if strings.HasPrefix(entry.Name(), "latched-") {
				// best effort, as in the engine's SCClearAtLoad
				os.Remove(filepath.Join(dir, entry.Name()))
			}
		}
	}

	if entries, err := os.ReadDir(home.StateDir(udid)); err == nil {
		for _, entry := range entries {
			if strings.HasPrefix(entry.Name(), "preflight-fail-") {
				// best effort, as in the engine's SCClearAtLoad
				os.Remove(filepath.Join(home.StateDir(udid), entry.Name()))
			}
		}
	}

	return appendErr
}

func firstField(s string) string {
	if fields := strings.Fields(s); len(fields) > 0 {
		return fields[0]
	}
	return ""
}

// featureOf folds unknown feature names into "engine", as the engine's SCFeatureIndex does.
func featureOf(feature string) string {
	switch feature {
	case "session", "photo", "movie", "provider", "engine", "activate", "motion": // kFeatureNames in core/sentinel.h
		return feature
	}
	return "engine"
}
