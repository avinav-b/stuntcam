// SPDX-License-Identifier: Apache-2.0

// Package trace reads the engine's op ring (engine/src/core/trace.inc): one trace-<pid>.txt per app process in the app's state
// dir ($SIMCAM_HOME/sims/<udid>/state/<bundle>/) or its container-tmp fallback (<data container>/tmp/simcam-state/<bundle>/),
// rewritten whole by the app while it runs. `stuntcam trace` prints it as a table or as the JSON in Report (cli/README.md).
package trace

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// RingSize is the engine's TR_N: the file holds at most the last RingSize ops.
const RingSize = 64

// Schema is the version of the JSON Report (cli/README.md documents it); bumped only for incompatible changes.
const Schema = 1

// PhotoStages is the order of one photo's ops in the ring (engine/src/core/trace.inc; design §4.5 "shutter -> slot 12 ->
// still complete -> timestamp"). slot12 is Fig slot 12, IrisStillImageSinkCaptureImage. timestamp is the instant the photo
// carries (AVCapturePhoto.timestamp), taken with its source frame, so it comes before the encode.
var PhotoStages = []string{"shutter", "slot12", "will-capture", "timestamp", "did-capture", "encode-start", "encoded", "still-complete", "did-finish"}

func stageIndex(name string) int {
	for i, s := range PhotoStages {
		if s == name {
			return i
		}
	}
	return -1
}

// Op is one ring entry. SettingsID is -1 for ops without one.
type Op struct {
	Index      uint64
	WallNs     int64  // CLOCK_REALTIME, ns since 1970
	Mach       uint64 // mach_absolute_time()
	Name       string
	SettingsID int64
	Arg        int64
}

// File is one parsed trace-<pid>.txt.
type File struct {
	Path        string
	Location    string // "state-dir" or "container-tmp"
	Pid         int
	Bundle      string
	Build       string
	Numer       uint64
	Denom       uint64
	WrittenNs   int64
	WrittenMach uint64
	Recorded    uint64
	Ops         []Op
}

var (
	ErrNotTrace   = errors.New("not a stuntcam trace file")
	ErrIncomplete = errors.New("trace file has no end line (cut short)")
)

// Parse reads one trace file's bytes.
func Parse(b []byte) (File, error) {
	var f File
	sc := bufio.NewScanner(bytes.NewReader(b))
	sc.Buffer(make([]byte, 64*1024), 1<<20)
	first, end := true, false
	for sc.Scan() {
		l := sc.Text()
		if first {
			if l != "stuntcam-trace 1" {
				if v, ok := strings.CutPrefix(l, "stuntcam-trace "); ok { // a newer engine's format
					return f, fmt.Errorf("%w (format %s; this stuntcam reads format 1: update the CLI)", ErrNotTrace, strings.TrimSpace(v))
				}
				return f, ErrNotTrace
			}
			first = false
			continue
		}
		fs := strings.Fields(l)
		if len(fs) == 0 {
			continue
		}
		var err error
		switch fs[0] {
		case "pid":
			if len(fs) == 2 {
				f.Pid, err = strconv.Atoi(fs[1])
			}
		case "bundle":
			if len(fs) == 2 {
				f.Bundle = fs[1]
			}
		case "build":
			if len(fs) == 2 {
				f.Build = fs[1]
			}
		case "timebase":
			if len(fs) == 3 {
				f.Numer, err = strconv.ParseUint(fs[1], 10, 64)
				if err == nil {
					f.Denom, err = strconv.ParseUint(fs[2], 10, 64)
				}
			}
		case "written":
			if len(fs) == 3 {
				f.WrittenNs, err = strconv.ParseInt(fs[1], 10, 64)
				if err == nil {
					f.WrittenMach, err = strconv.ParseUint(fs[2], 10, 64)
				}
			}
		case "recorded":
			if len(fs) == 2 {
				f.Recorded, err = strconv.ParseUint(fs[1], 10, 64)
			}
		case "op":
			if len(fs) != 7 {
				return f, fmt.Errorf("bad op line %q", l)
			}
			var o Op
			o.Index, err = strconv.ParseUint(fs[1], 10, 64)
			var e2, e3, e4, e5 error
			o.WallNs, e2 = strconv.ParseInt(fs[2], 10, 64)
			o.Mach, e3 = strconv.ParseUint(fs[3], 10, 64)
			o.Name = fs[4]
			o.SettingsID, e4 = strconv.ParseInt(fs[5], 10, 64)
			o.Arg, e5 = strconv.ParseInt(fs[6], 10, 64)
			if err = errors.Join(err, e2, e3, e4, e5); err == nil {
				f.Ops = append(f.Ops, o)
			}
		case "end":
			end = true
		}
		if err != nil {
			return f, fmt.Errorf("bad line %q: %w", l, err)
		}
	}
	if first {
		return f, ErrNotTrace
	}
	if !end {
		return f, ErrIncomplete
	}
	if f.Denom == 0 {
		f.Numer, f.Denom = 1, 1
	}
	return f, nil
}

// HostNs converts mach ticks to host-clock nanoseconds (what CMClockGetHostTimeClock and AVCapturePhoto.timestamp read).
func (f File) HostNs(mach uint64) int64 {
	n, d := f.Numer, f.Denom
	if d == 0 {
		n, d = 1, 1
	}
	return int64(mach/d*n + mach%d*n/d)
}

// Written is when the app last rewrote the file.
func (f File) Written() time.Time { return time.Unix(0, f.WrittenNs) }

// Location is one directory an app may keep its traces in.
type Location struct {
	Dir  string
	Kind string // "state-dir" or "container-tmp"
}

// Find lists and parses every trace-*.txt in the locations, newest written first. Files that aren't regular files (a symlink:
// an app can write its own container, and the CLI never follows a link out of it) or don't parse are skipped and named in
// skipped.
func Find(locs []Location) (files []File, skipped []string) {
	for _, l := range locs {
		ents, err := os.ReadDir(l.Dir)
		if err != nil {
			continue
		}
		for _, e := range ents {
			n := e.Name()
			if !strings.HasPrefix(n, "trace-") || !strings.HasSuffix(n, ".txt") {
				continue
			}
			p := filepath.Join(l.Dir, n)
			fi, err := os.Lstat(p)
			if err != nil || !fi.Mode().IsRegular() {
				skipped = append(skipped, p+": not a regular file")
				continue
			}
			b, err := os.ReadFile(p)
			if err != nil {
				skipped = append(skipped, p+": "+err.Error())
				continue
			}
			f, err := Parse(b)
			if err != nil {
				skipped = append(skipped, p+": "+err.Error())
				continue
			}
			f.Path, f.Location = p, l.Kind
			files = append(files, f)
		}
	}
	sort.SliceStable(files, func(i, j int) bool { return files[i].WrittenNs > files[j].WrittenNs })
	return files, skipped
}

// Pick returns the newest file, or the one of pid (pid > 0).
func Pick(files []File, pid int) (File, []File, error) {
	if len(files) == 0 {
		return File{}, nil, errors.New("no trace")
	}
	if pid <= 0 {
		return files[0], files[1:], nil
	}
	var others []File
	var got *File
	for i := range files {
		if files[i].Pid == pid && got == nil {
			got = &files[i]
		} else {
			others = append(others, files[i])
		}
	}
	if got == nil {
		var ps []string
		for _, f := range files {
			ps = append(ps, strconv.Itoa(f.Pid))
		}
		return File{}, nil, fmt.Errorf("no trace of pid %d (traces: pid %s)", pid, strings.Join(ps, ", "))
	}
	return *got, others, nil
}

// ---- the report (the JSON schema; the table prints the same) ----

type Timebase struct {
	Numer uint64 `json:"numer"`
	Denom uint64 `json:"denom"`
}

type ReportOp struct {
	Index      uint64   `json:"index"`
	Op         string   `json:"op"`
	Wall       string   `json:"wall"`    // RFC 3339, ns, UTC
	WallNs     int64    `json:"wall_ns"` // CLOCK_REALTIME
	Mach       uint64   `json:"mach"`
	HostNs     int64    `json:"host_ns"`     // host-clock ns (AVCapturePhoto.timestamp's clock)
	SettingsID *int64   `json:"settings_id"` // null: not a photo or movie op
	Arg        int64    `json:"arg"`
	DeltaMs    *float64 `json:"delta_ms"` // photo ops: from that photo's shutter (else its slot12); null otherwise
}

type ReportPhoto struct {
	SettingsID int64    `json:"settings_id"`
	Stages     []string `json:"stages"`    // in ring order
	InOrder    bool     `json:"in_order"`  // stages appear in PhotoStages order, each at most once
	Monotonic  bool     `json:"monotonic"` // host times never go back
	Complete   bool     `json:"complete"`  // shutter, slot12, still-complete and (unless it failed) timestamp are all there
	Error      int64    `json:"error"`     // still-complete's ErrorStatus (0 = delivered)
	// host-clock ns of each stage that is there (absent stages are omitted)
	HostNs map[string]int64 `json:"host_ns"`
	// ms from the shutter (or slot12 without a shutter) to each later stage
	Ms map[string]float64 `json:"ms"`
	// timestamp - slot12 in ms (design §9 asks 0..50); null without both
	TimestampMinusSlot12Ms *float64 `json:"timestamp_minus_slot12_ms"`
	TimestampS             *float64 `json:"timestamp_s"` // the photo's AVCapturePhoto.timestamp, seconds (arg of the timestamp op)
}

type ReportOther struct {
	Pid      int    `json:"pid"`
	File     string `json:"file"`
	Location string `json:"location"`
	Written  string `json:"written"`
}

type Report struct {
	Schema    int           `json:"schema"`
	UDID      string        `json:"udid"`
	Simulator string        `json:"simulator"`
	Bundle    string        `json:"bundle"`
	Pid       int           `json:"pid"`
	Running   *bool         `json:"running"` // that pid is the app's running process on the simulator; null: couldn't tell
	Build     string        `json:"build"`
	File      string        `json:"file"`
	Location  string        `json:"location"`
	Written   string        `json:"written"`
	WrittenNs int64         `json:"written_ns"`
	Recorded  uint64        `json:"recorded"`
	RingSize  int           `json:"ring_size"`
	Timebase  Timebase      `json:"timebase"`
	Ops       []ReportOp    `json:"ops"`
	Photos    []ReportPhoto `json:"photos"`
	Others    []ReportOther `json:"others"`
}

func rfc(ns int64) string { return time.Unix(0, ns).UTC().Format(time.RFC3339Nano) }
func ms(ns int64) float64 { return math.Round(float64(ns)/1e3) / 1e3 } // ns -> ms, µs resolution

// Build makes the report of f (others: the bundle's other traces, newest first).
func Build(f File, others []File, udid, sim string, running *bool) Report {
	r := Report{Schema: Schema, UDID: udid, Simulator: sim, Bundle: f.Bundle, Pid: f.Pid, Running: running, Build: f.Build,
		File: f.Path, Location: f.Location, Written: rfc(f.WrittenNs), WrittenNs: f.WrittenNs, Recorded: f.Recorded,
		RingSize: RingSize, Timebase: Timebase{f.Numer, f.Denom}, Ops: []ReportOp{}, Photos: []ReportPhoto{}, Others: []ReportOther{}}
	type acc struct {
		p      ReportPhoto
		origin int64
		hasOr  bool
		last   int64
		maxIdx int
	}
	photos := map[int64]*acc{}
	var order []int64
	origin := func(sid int64) (int64, bool) {
		a := photos[sid]
		if a == nil {
			return 0, false
		}
		return a.origin, a.hasOr
	}
	// first pass: the photos' stages and origins (shutter, else slot12)
	for _, o := range f.Ops {
		if stageIndex(o.Name) < 0 || o.SettingsID < 0 {
			continue
		}
		a := photos[o.SettingsID]
		if a == nil {
			a = &acc{p: ReportPhoto{SettingsID: o.SettingsID, InOrder: true, Monotonic: true, HostNs: map[string]int64{}, Ms: map[string]float64{}}, maxIdx: -1}
			photos[o.SettingsID] = a
			order = append(order, o.SettingsID)
		}
		h := f.HostNs(o.Mach)
		if !a.hasOr && (o.Name == "shutter" || o.Name == "slot12") {
			a.origin, a.hasOr = h, true
		}
		if _, dup := a.p.HostNs[o.Name]; dup {
			a.p.InOrder = false
		}
		if i := stageIndex(o.Name); i <= a.maxIdx {
			a.p.InOrder = false
		} else {
			a.maxIdx = i
		}
		if len(a.p.Stages) > 0 && h < a.last {
			a.p.Monotonic = false
		}
		a.last = h
		a.p.Stages = append(a.p.Stages, o.Name)
		a.p.HostNs[o.Name] = h
		if o.Name == "still-complete" {
			a.p.Error = o.Arg
		}
		if o.Name == "timestamp" {
			s := float64(o.Arg) / 1e9
			a.p.TimestampS = &s
		}
	}
	for _, o := range f.Ops {
		ro := ReportOp{Index: o.Index, Op: o.Name, Wall: rfc(o.WallNs), WallNs: o.WallNs, Mach: o.Mach, HostNs: f.HostNs(o.Mach), Arg: o.Arg}
		if o.SettingsID >= 0 {
			sid := o.SettingsID
			ro.SettingsID = &sid
			if stageIndex(o.Name) >= 0 {
				if or, ok := origin(sid); ok {
					d := ms(ro.HostNs - or)
					ro.DeltaMs = &d
				}
			}
		}
		r.Ops = append(r.Ops, ro)
	}
	for _, sid := range order {
		a := photos[sid]
		p := a.p
		_, sh := p.HostNs["shutter"]
		_, s12 := p.HostNs["slot12"]
		_, sc := p.HostNs["still-complete"]
		_, ts := p.HostNs["timestamp"]
		p.Complete = sh && s12 && sc && (ts || p.Error != 0)
		if a.hasOr {
			for k, v := range p.HostNs {
				p.Ms[k] = ms(v - a.origin)
			}
		}
		if p.Error != 0 { // the engine stamps the grab before the photo queue fails it: nothing was delivered
			p.TimestampS = nil
		} else if ts && s12 {
			d := ms(p.HostNs["timestamp"] - p.HostNs["slot12"])
			p.TimestampMinusSlot12Ms = &d
		}
		r.Photos = append(r.Photos, p)
	}
	for _, o := range others {
		r.Others = append(r.Others, ReportOther{Pid: o.Pid, File: o.Path, Location: o.Location, Written: rfc(o.WrittenNs)})
	}
	return r
}

// info is the table's last column: what an op's arg means
func info(o ReportOp) string {
	switch {
	case o.Op == "encoded":
		return fmt.Sprintf("%d bytes", o.Arg)
	case o.Op == "timestamp":
		return fmt.Sprintf("photo timestamp %.6f s", float64(o.Arg)/1e9)
	case o.Op == "still-complete" && o.Arg != 0:
		return fmt.Sprintf("error %d", o.Arg)
	case o.Op == "session-start":
		return fmt.Sprintf("configuration %d", o.Arg)
	case o.Op == "movie-stop" && o.Arg != 0:
		return fmt.Sprintf("status %d", o.Arg)
	case o.Op == "crash":
		return fmt.Sprintf("signal %d", o.Arg)
	}
	return ""
}

func ago(d time.Duration) string {
	switch {
	case d < 0:
		return "just now"
	case d < time.Minute:
		return fmt.Sprintf("%.1f s ago", d.Seconds())
	case d < time.Hour:
		return fmt.Sprintf("%d min ago", int(d.Minutes()))
	case d < 48*time.Hour:
		return fmt.Sprintf("%.1f h ago", d.Hours())
	}
	return fmt.Sprintf("%d days ago", int(d.Hours()/24))
}

// Human prints the table: op, host time (wall clock, loc), Δ ms from the photo's shutter, settings id, and what the arg means;
// then one line per photo and the other traces.
func Human(w io.Writer, r Report, now time.Time, loc *time.Location) {
	state := "not running"
	if r.Running == nil {
		state = "running state unknown"
	} else if *r.Running {
		state = "running"
	}
	where := map[string]string{"state-dir": "state dir", "container-tmp": "container-tmp fallback"}[r.Location]
	written := time.Unix(0, r.WrittenNs).In(loc)
	fmt.Fprintf(w, "%s pid %d (%s) on %s (%s), build %s\n", r.Bundle, r.Pid, state, r.Simulator, r.UDID, r.Build)
	shown := len(r.Ops)
	fmt.Fprintf(w, "%s (%s), written %s (%s); %d ops recorded, the last %d shown\n", r.File, where, written.Format("2006-01-02 15:04:05.000"), ago(now.Sub(written)), r.Recorded, shown)
	fmt.Fprintf(w, "%5s  %-15s %-12s %10s %9s  %s\n", "#", "op", "host time", "Δ ms", "settings", "")
	for _, o := range r.Ops {
		d, sid := "", "-"
		if o.DeltaMs != nil {
			d = fmt.Sprintf("%.3f", *o.DeltaMs)
		}
		if o.SettingsID != nil {
			sid = strconv.FormatInt(*o.SettingsID, 10)
		}
		fmt.Fprintf(w, "%5d  %-15s %-12s %10s %9s  %s\n", o.Index, o.Op, time.Unix(0, o.WallNs).In(loc).Format("15:04:05.000"), d, sid, info(o))
	}
	if len(r.Photos) > 0 {
		ok := 0
		for _, p := range r.Photos {
			if p.InOrder && p.Monotonic && p.Complete {
				ok++
			}
		}
		fmt.Fprintf(w, "photos: %d, %d complete with every stage in order and host times monotonic\n", len(r.Photos), ok)
		for _, p := range r.Photos {
			var parts []string
			if v, ok := p.Ms["slot12"]; ok {
				parts = append(parts, fmt.Sprintf("slot12 +%.1f ms", v))
			}
			if p.TimestampMinusSlot12Ms != nil {
				parts = append(parts, fmt.Sprintf("timestamp - slot12 %.1f ms", *p.TimestampMinusSlot12Ms))
			}
			if v, ok := p.Ms["still-complete"]; ok {
				parts = append(parts, fmt.Sprintf("still-complete +%.1f ms", v))
			}
			if v, ok := p.Ms["did-finish"]; ok {
				parts = append(parts, fmt.Sprintf("did-finish +%.1f ms", v))
			}
			var notes []string
			if !p.Complete {
				notes = append(notes, "incomplete (in flight, overwritten, or failed before the shutter hook)")
			}
			if !p.InOrder {
				notes = append(notes, "OUT OF ORDER")
			}
			if !p.Monotonic {
				notes = append(notes, "TIME WENT BACK")
			}
			if p.Error != 0 {
				notes = append(notes, fmt.Sprintf("error %d", p.Error))
			}
			if len(notes) > 0 {
				parts = append(parts, strings.Join(notes, ", "))
			}
			fmt.Fprintf(w, "  %d: %s\n", p.SettingsID, strings.Join(parts, ", "))
		}
	}
	for _, o := range r.Others {
		t, _ := time.Parse(time.RFC3339Nano, o.Written)
		fmt.Fprintf(w, "older trace: pid %d, written %s (--pid %d)\n", o.Pid, t.In(loc).Format("2006-01-02 15:04:05"), o.Pid)
	}
}

// Running reports whether `launchctl list` (run in the simulator) shows pid as bundle's app process: a line
// "<pid> <status> UIKitApplication:<bundle>[…]".
func Running(launchctlList, bundle string, pid int) bool {
	for _, l := range strings.Split(launchctlList, "\n") {
		fs := strings.Fields(l)
		if len(fs) < 3 {
			continue
		}
		lab := fs[len(fs)-1]
		if lab != "UIKitApplication:"+bundle && !strings.HasPrefix(lab, "UIKitApplication:"+bundle+"[") {
			continue
		}
		if p, err := strconv.Atoi(fs[0]); err == nil && p == pid {
			return true
		}
	}
	return false
}
