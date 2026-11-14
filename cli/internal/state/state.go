// SPDX-License-Identifier: Apache-2.0

// Package state reads what the engine records per app (engine/src/core/safety.inc): one events.log per bundle id under
// sims/<udid>/state/<bundle>/, lines "<epoch> <activation#> <kind> <feature> <build> <detail…>". Latch rules match the
// engine's: two crash or hang events of one feature, same build, within the last 20 activations and 24 h.
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
	ents, _ := os.ReadDir(home.StateDir(udid))
	var out []string
	for _, e := range ents {
		if e.IsDir() {
			out = append(out, e.Name())
		}
	}
	sort.Strings(out)
	return out
}

func Parse(lines []string) []Event {
	var out []Event
	for _, l := range lines {
		f := strings.Fields(l)
		if len(f) < 5 {
			continue
		}
		at, err1 := strconv.ParseInt(f[0], 10, 64)
		act, err2 := strconv.Atoi(f[1])
		if err1 != nil || err2 != nil {
			continue
		}
		out = append(out, Event{At: at, Activation: act, Kind: f[2], Feature: f[3], Build: f[4], Detail: strings.Join(f[5:], " ")})
	}
	return out
}

// Analyze applies the engine's rules as of now for the given build ("" = the last activation's build).
func Analyze(bundle string, all []Event, build string, now time.Time) App {
	a := App{Bundle: bundle, Latched: map[string]int{}}
	maxAct := 0
	var past []Event
	for _, e := range all {
		if e.Activation > maxAct {
			maxAct = e.Activation
		}
		if e.Kind == "clear" {
			past = past[:0]
			continue
		}
		past = append(past, e)
	}
	lastAct := -1
	for i, e := range past {
		if e.Kind == "activate" {
			a.Activations++
			lastAct = i
		}
	}
	if lastAct >= 0 {
		ev := past[lastAct]
		a.LastActivation = &ev
		if build == "" {
			build = ev.Build
		}
		traced := firstField(ev.Detail) == "traced=1" // the engine compares the field exactly ("latched=1 traced=…" isn't traced)
		ended := false
		open := map[string]string{}
		for _, e := range past[lastAct+1:] {
			switch {
			case e.Kind == "exit" || strings.HasPrefix(e.Kind, "crash"):
				ended = true
			case e.Kind == "op-begin":
				open[e.Detail] = e.Feature
			case e.Kind == "op-end":
				delete(open, e.Detail)
			}
		}
		if traced && !ended {
			for _, f := range open {
				a.UnknownDebugEnd = f
			}
		}
	}
	next := maxAct + 1
	for _, e := range past {
		if (e.Kind != "crash" && e.Kind != "hang") || e.Build != build {
			continue
		}
		if e.Activation <= next-20 || e.At < now.Add(-24*time.Hour).Unix() {
			continue
		}
		a.Latched[featureOf(e.Feature)]++
		a.Counted = append(a.Counted, e)
	}
	return a
}

// Load reads and analyzes one bundle's events on a sim.
func Load(udid, bundle, build string) (App, error) {
	return LoadFile(EventsFile(udid, bundle), bundle, build)
}

// LoadFile reads and analyzes one events.log (the state dir's, or a container-tmp fallback's).
func LoadFile(path, bundle, build string) (App, error) {
	f, err := os.Open(path)
	if err != nil {
		return App{Bundle: bundle, Latched: map[string]int{}}, err
	}
	defer f.Close()
	var lines []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		lines = append(lines, sc.Text())
	}
	return Analyze(bundle, Parse(lines), build, time.Now()), sc.Err()
}

// ClosesCamera reports whether a latch of this feature closes the camera. A `motion` latch (W4 PR B) only makes CoreMotion
// stock from the next launch; the camera is unaffected (plan D6).
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

// Marker is a file the engine writes in the bundle's state dir (W4): latched-<build> holds the latched camera features of one
// build (a valid one makes that build's next launches camera-stock without activating, and CoreMotion stock);
// latched-motion-<build> (PR B) holds `motion` (CoreMotion stock, the camera unaffected). Its mtime is the time it stops
// counting.
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
	ents, _ := os.ReadDir(dir)
	var out []Marker
	for _, e := range ents {
		n := e.Name()
		if !strings.HasPrefix(n, "latched-") || e.IsDir() {
			continue
		}
		fi, err := e.Info()
		if err != nil {
			continue
		}
		b, _ := os.ReadFile(filepath.Join(dir, n))
		m := Marker{Build: strings.TrimPrefix(n, "latched-"), Features: strings.TrimSpace(string(b)), Until: fi.ModTime()}
		if strings.HasPrefix(n, "latched-motion-") {
			m.Build, m.Motion = strings.TrimPrefix(n, "latched-motion-"), true
		}
		out = append(out, m)
	}
	return out
}

// PreflightFail is a sim's preflight-fail-<build> marker: a real preflight failure of that build, valid for 24 h (its mtime);
// while valid, that build's launches on the sim get stock CoreMotion (the camera runs its own preflight every launch). A
// passing preflight deletes it.
type PreflightFail struct {
	Build string
	What  string
	Until time.Time
}

func (p PreflightFail) Valid(now time.Time) bool { return p.Until.After(now) }

// PreflightFails lists a sim's preflight-fail-<build> markers.
func PreflightFails(udid string) []PreflightFail { return PreflightFailsIn(home.StateDir(udid)) }

// PreflightFailsIn lists the preflight-fail-<build> markers in one root (the state dir, or a container's tmp/simcam-state).
func PreflightFailsIn(root string) []PreflightFail {
	var out []PreflightFail
	ents, _ := os.ReadDir(root)
	for _, e := range ents {
		n := e.Name()
		if !strings.HasPrefix(n, "preflight-fail-") || e.IsDir() {
			continue
		}
		fi, err := e.Info()
		if err != nil {
			continue
		}
		b, _ := os.ReadFile(filepath.Join(root, n))
		out = append(out, PreflightFail{Build: strings.TrimPrefix(n, "preflight-fail-"), What: strings.TrimSpace(string(b)), Until: fi.ModTime()})
	}
	return out
}

// Clear forgets the past for one bundle (the engine's own `SIMCAM_SENTINEL=clear` writes the same line) and, as that does,
// removes the bundle's latched-* markers and the sim's preflight-fail-* markers.
func Clear(udid, bundle, why string) error {
	if bundle == "" || bundle == "." || bundle == ".." || strings.ContainsAny(bundle, "/\x00") {
		return fmt.Errorf("bad bundle id %q", bundle)
	}
	dir := filepath.Join(home.StateDir(udid), bundle)
	// the clear line first, as the engine's SCClearAtLoad does (review 2)
	p := EventsFile(udid, bundle)
	aerr := os.MkdirAll(filepath.Dir(p), 0o755)
	if aerr == nil {
		aerr = AppendClear(p, why, true) // the markers go even if this fails, as in SCClearAtLoad; the error is returned (r2 LOW 2)
	}
	if ents, err := os.ReadDir(dir); err == nil {
		for _, e := range ents {
			if strings.HasPrefix(e.Name(), "latched-") {
				os.Remove(filepath.Join(dir, e.Name()))
			}
		}
	}
	if ents, err := os.ReadDir(home.StateDir(udid)); err == nil {
		for _, e := range ents {
			if strings.HasPrefix(e.Name(), "preflight-fail-") {
				os.Remove(filepath.Join(home.StateDir(udid), e.Name()))
			}
		}
	}
	return aerr
}

func firstField(s string) string {
	if f := strings.Fields(s); len(f) > 0 {
		return f[0]
	}
	return ""
}

// featureOf folds unknown feature names into "engine", as the engine's SCFeatureIndex does.
func featureOf(f string) string {
	switch f {
	case "session", "photo", "movie", "provider", "engine", "activate", "motion": // gSCFeatures in core/safety.inc (W4: activate, motion)
		return f
	}
	return "engine"
}
