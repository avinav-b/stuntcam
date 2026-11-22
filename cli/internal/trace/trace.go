// SPDX-License-Identifier: Apache-2.0

// Package trace reads the engine's op ring (engine/src/core/trace.inc).
// There is one trace-<pid>.txt per app process. It is in the app's state dir ($SIMCAM_HOME/sims/<udid>/state/<bundle>/)
// or in its container-tmp fallback (<data container>/tmp/simcam-state/<bundle>/). The app rewrites it whole while it
// runs.
// `stuntcam trace` prints it as a table, or as the JSON in Report (cli/README.md).
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
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"
)

// RingSize is the engine's TR_RING_SIZE: the file holds at most the last RingSize ops.
const RingSize = 64

// Schema is the version of the JSON Report (cli/README.md documents it); bumped only for incompatible changes.
const Schema = 1

// PhotoStages is the order of one photo's ops in the ring, as engine/src/core/trace.inc records them (shutter, then
// slot 12, then still complete, then the timestamp). slot12 is Fig slot 12, IrisStillImageSinkCaptureImage. timestamp
// is the instant the photo carries (AVCapturePhoto.timestamp), taken with its source frame, so it comes before the
// encode.
var PhotoStages = []string{
	"shutter", "slot12", "will-capture", "timestamp", "did-capture", "encode-start", "encoded", "still-complete",
	"did-finish",
}

// stageIndex is name's place in PhotoStages, -1 for an op that isn't a photo stage.
func stageIndex(name string) int { return slices.Index(PhotoStages, name) }

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

// maxLine is the longest line Parse reads; a longer one ends the scan (the file then counts as cut short).
const maxLine = 1 << 20

// Parse reads one trace file's bytes.
func Parse(data []byte) (File, error) {
	var file File
	scanner := bufio.NewScanner(bytes.NewReader(data))
	scanner.Buffer(make([]byte, 64*1024), maxLine)
	first, end := true, false
	for scanner.Scan() {
		line := scanner.Text()
		if first {
			if err := checkFormat(line); err != nil {
				return file, err
			}

			first = false
			continue
		}

		fields := strings.Fields(line)
		switch {
		case len(fields) == 0:
			continue
		case fields[0] == "op" && len(fields) != 7:
			return file, fmt.Errorf("bad op line %q", line)
		case fields[0] == "end":
			end = true
			continue
		}

		if err := file.parseField(fields); err != nil {
			return file, fmt.Errorf("bad line %q: %w", line, err)
		}
	}

	if first {
		return file, ErrNotTrace
	}

	if !end {
		return file, ErrIncomplete
	}

	if file.Denom == 0 {
		file.Numer, file.Denom = 1, 1
	}

	return file, nil
}

// checkFormat checks a trace file's first line: format 1.
func checkFormat(line string) error {
	if line == "stuntcam-trace 1" {
		return nil
	}
	if v, ok := strings.CutPrefix(line, "stuntcam-trace "); ok { // a newer engine's format
		return fmt.Errorf("%w (format %s; this stuntcam reads format 1: update the CLI)", ErrNotTrace,
			strings.TrimSpace(v))
	}
	return ErrNotTrace
}

// parseField reads one header or op line (its fields) into f. A header line with an unexpected number of fields, or an
// unknown line, is ignored.
func (f *File) parseField(fields []string) error {
	var err error
	switch fields[0] {
	case "pid":
		if len(fields) == 2 {
			f.Pid, err = strconv.Atoi(fields[1])
		}
	case "bundle":
		if len(fields) == 2 {
			f.Bundle = fields[1]
		}
	case "build":
		if len(fields) == 2 {
			f.Build = fields[1]
		}
	case "timebase":
		if len(fields) == 3 {
			f.Numer, err = strconv.ParseUint(fields[1], 10, 64)
			if err == nil {
				f.Denom, err = strconv.ParseUint(fields[2], 10, 64)
			}
		}
	case "written":
		if len(fields) == 3 {
			f.WrittenNs, err = strconv.ParseInt(fields[1], 10, 64)
			if err == nil {
				f.WrittenMach, err = strconv.ParseUint(fields[2], 10, 64)
			}
		}
	case "recorded":
		if len(fields) == 2 {
			f.Recorded, err = strconv.ParseUint(fields[1], 10, 64)
		}
	case "op":
		var o Op
		if o, err = parseOp(fields); err == nil {
			f.Ops = append(f.Ops, o)
		}
	}

	return err
}

// parseOp reads an op line's 7 fields: op <index> <wall ns> <mach> <name> <settings id> <arg>.
func parseOp(fields []string) (Op, error) {
	var op Op
	var errs [5]error
	op.Index, errs[0] = strconv.ParseUint(fields[1], 10, 64)
	op.WallNs, errs[1] = strconv.ParseInt(fields[2], 10, 64)
	op.Mach, errs[2] = strconv.ParseUint(fields[3], 10, 64)
	op.Name = fields[4]
	op.SettingsID, errs[3] = strconv.ParseInt(fields[5], 10, 64)
	op.Arg, errs[4] = strconv.ParseInt(fields[6], 10, 64)
	return op, errors.Join(errs[:]...)
}

// HostNs converts mach ticks to host-clock nanoseconds (what CMClockGetHostTimeClock and AVCapturePhoto.timestamp
// read).
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

// Find lists and parses every trace-*.txt in the locations, newest written first. Files that aren't regular files (a
// symlink: an app can write its own container, and the CLI never follows a link out of it) or don't parse are skipped
// and named in skipped.
func Find(locs []Location) (files []File, skipped []string) {
	for _, loc := range locs {
		ents, err := os.ReadDir(loc.Dir)
		if err != nil {
			continue
		}

		for _, entry := range ents {
			name := entry.Name()
			if !strings.HasPrefix(name, "trace-") || !strings.HasSuffix(name, ".txt") {
				continue
			}

			path := filepath.Join(loc.Dir, name)
			fileInfo, err := os.Lstat(path)
			if err != nil || !fileInfo.Mode().IsRegular() {
				skipped = append(skipped, path+": not a regular file")
				continue
			}

			data, err := os.ReadFile(path)
			if err != nil {
				skipped = append(skipped, path+": "+err.Error())
				continue
			}

			file, err := Parse(data)
			if err != nil {
				skipped = append(skipped, path+": "+err.Error())
				continue
			}

			file.Path, file.Location = path, loc.Kind
			files = append(files, file)
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
	// timestamp - slot12 in ms (the acceptance run requires 0..50); null without both
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

// Build makes the report of file (others: the bundle's other traces, newest first).
func Build(file File, others []File, udid, sim string, running *bool) Report {
	report := Report{
		Schema:    Schema,
		UDID:      udid,
		Simulator: sim,
		Bundle:    file.Bundle,
		Pid:       file.Pid,
		Running:   running,
		Build:     file.Build,
		File:      file.Path,
		Location:  file.Location,
		Written:   rfc(file.WrittenNs),
		WrittenNs: file.WrittenNs,
		Recorded:  file.Recorded,
		RingSize:  RingSize,
		Timebase:  Timebase{file.Numer, file.Denom},
		Ops:       []ReportOp{},
		Photos:    []ReportPhoto{},
		Others:    []ReportOther{},
	}

	photos, order := collectPhotos(file)
	for _, op := range file.Ops {
		report.Ops = append(report.Ops, reportOp(file, op, photos))
	}

	for _, sid := range order {
		report.Photos = append(report.Photos, photos[sid].finish())
	}

	for _, other := range others {
		report.Others = append(report.Others,
			ReportOther{Pid: other.Pid, File: other.Path, Location: other.Location, Written: rfc(other.WrittenNs)})
	}

	return report
}

// photoAcc collects one photo's stages from the ring.
type photoAcc struct {
	photo     ReportPhoto
	origin    int64 // host ns of its shutter, else its slot12
	hasOrigin bool
	last      int64 // host ns of the last stage seen
	maxStage  int   // PhotoStages index of the latest stage seen so far
}

// collectPhotos goes through file's photo ops (settings id and a PhotoStages name) once: each photo's stages, host
// times, origin, order and error. order is the photos' settings ids in ring order.
func collectPhotos(file File) (photos map[int64]*photoAcc, order []int64) {
	photos = map[int64]*photoAcc{}
	for _, op := range file.Ops {
		if stageIndex(op.Name) < 0 || op.SettingsID < 0 {
			continue
		}

		acc := photos[op.SettingsID]
		if acc == nil {
			acc = &photoAcc{
				photo: ReportPhoto{
					SettingsID: op.SettingsID,
					InOrder:    true,
					Monotonic:  true,
					HostNs:     map[string]int64{},
					Ms:         map[string]float64{},
				},
				maxStage: -1,
			}

			photos[op.SettingsID] = acc
			order = append(order, op.SettingsID)
		}

		acc.add(op, file.HostNs(op.Mach))
	}

	return photos, order
}

// add records one stage of the photo, at host time hostNs.
func (a *photoAcc) add(op Op, hostNs int64) {
	if !a.hasOrigin && (op.Name == "shutter" || op.Name == "slot12") {
		a.origin, a.hasOrigin = hostNs, true
	}

	if _, dup := a.photo.HostNs[op.Name]; dup {
		a.photo.InOrder = false
	}

	if i := stageIndex(op.Name); i <= a.maxStage {
		a.photo.InOrder = false
	} else {
		a.maxStage = i
	}

	if len(a.photo.Stages) > 0 && hostNs < a.last {
		a.photo.Monotonic = false
	}

	a.last = hostNs
	a.photo.Stages = append(a.photo.Stages, op.Name)
	a.photo.HostNs[op.Name] = hostNs
	if op.Name == "still-complete" {
		a.photo.Error = op.Arg
	}

	if op.Name == "timestamp" {
		seconds := float64(op.Arg) / 1e9
		a.photo.TimestampS = &seconds
	}
}

// reportOp is one op's report line; a photo op's DeltaMs counts from its photo's origin.
func reportOp(file File, op Op, photos map[int64]*photoAcc) ReportOp {
	report := ReportOp{
		Index:  op.Index,
		Op:     op.Name,
		Wall:   rfc(op.WallNs),
		WallNs: op.WallNs,
		Mach:   op.Mach,
		HostNs: file.HostNs(op.Mach),
		Arg:    op.Arg,
	}

	if op.SettingsID < 0 {
		return report
	}

	sid := op.SettingsID
	report.SettingsID = &sid
	if stageIndex(op.Name) >= 0 {
		if a := photos[sid]; a != nil && a.hasOrigin {
			delta := ms(report.HostNs - a.origin)
			report.DeltaMs = &delta
		}
	}

	return report
}

// finish completes the photo's report: whether it is complete, its stages in ms from the origin, timestamp - slot12.
func (a *photoAcc) finish() ReportPhoto {
	photo := a.photo
	_, hasShutter := photo.HostNs["shutter"]
	_, hasSlot12 := photo.HostNs["slot12"]
	_, hasComplete := photo.HostNs["still-complete"]
	_, hasTimestamp := photo.HostNs["timestamp"]
	photo.Complete = hasShutter && hasSlot12 && hasComplete && (hasTimestamp || photo.Error != 0)
	if a.hasOrigin {
		for k, v := range photo.HostNs {
			photo.Ms[k] = ms(v - a.origin)
		}
	}

	if photo.Error != 0 { // the engine stamps the grab before the photo queue fails it: nothing was delivered
		photo.TimestampS = nil
	} else if hasTimestamp && hasSlot12 {
		delta := ms(photo.HostNs["timestamp"] - photo.HostNs["slot12"])
		photo.TimestampMinusSlot12Ms = &delta
	}

	return photo
}

// info is the table's last column: what an op's arg means
func info(op ReportOp) string {
	switch {
	case op.Op == "encoded":
		return fmt.Sprintf("%d bytes", op.Arg)
	case op.Op == "timestamp":
		return fmt.Sprintf("photo timestamp %.6f s", float64(op.Arg)/1e9)
	case op.Op == "still-complete" && op.Arg != 0:
		return fmt.Sprintf("error %d", op.Arg)
	case op.Op == "session-start":
		return fmt.Sprintf("configuration %d", op.Arg)
	case op.Op == "movie-stop" && op.Arg != 0:
		return fmt.Sprintf("status %d", op.Arg)
	case op.Op == "crash":
		return fmt.Sprintf("signal %d", op.Arg)
	}

	return ""
}

func ago(age time.Duration) string {
	switch {
	case age < 0:
		return "just now"
	case age < time.Minute:
		return fmt.Sprintf("%.1f s ago", age.Seconds())
	case age < time.Hour:
		return fmt.Sprintf("%d min ago", int(age.Minutes()))
	case age < 48*time.Hour:
		return fmt.Sprintf("%.1f h ago", age.Hours())
	}

	return fmt.Sprintf("%d days ago", int(age.Hours()/24))
}

// Human prints the table: op, host time (wall clock, loc), Δ ms from the photo's shutter, settings id, and what the arg
// means; then one line per photo and the other traces.
func Human(w io.Writer, report Report, now time.Time, loc *time.Location) {
	humanHeader(w, report, now, loc)
	humanOps(w, report.Ops, loc)
	humanPhotos(w, report.Photos)
	humanOthers(w, report.Others, loc)
}

// runningText is the header's word for whether the app is running (nil: unknown).
func runningText(running *bool) string {
	switch {
	case running == nil:
		return "running state unknown"
	case *running:
		return "running"
	}

	return "not running"
}

// humanHeader prints which app and trace file the table is from: its pid, simulator and build, then the file, where it
// was found, when it was written and how many ops it holds.
func humanHeader(w io.Writer, report Report, now time.Time, loc *time.Location) {
	where := map[string]string{"state-dir": "state dir", "container-tmp": "container-tmp fallback"}[report.Location]
	written := time.Unix(0, report.WrittenNs).In(loc)
	fmt.Fprintf(w, "%s pid %d (%s) on %s (%s), build %s\n", report.Bundle, report.Pid, runningText(report.Running),
		report.Simulator, report.UDID, report.Build)
	shown := len(report.Ops)
	fmt.Fprintf(w, "%s (%s), written %s (%s); %d ops recorded, the last %d shown\n",
		report.File, where, written.Format("2006-01-02 15:04:05.000"), ago(now.Sub(written)), report.Recorded, shown)
}

// humanOps prints the column headings and one row per op.
func humanOps(w io.Writer, ops []ReportOp, loc *time.Location) {
	fmt.Fprintf(w, "%5s  %-15s %-12s %10s %9s  %s\n", "#", "op", "host time", "Δ ms", "settings", "")
	for _, op := range ops {
		delta, sid := "", "-"
		if op.DeltaMs != nil {
			delta = fmt.Sprintf("%.3f", *op.DeltaMs)
		}

		if op.SettingsID != nil {
			sid = strconv.FormatInt(*op.SettingsID, 10)
		}

		wall := time.Unix(0, op.WallNs).In(loc).Format("15:04:05.000")
		fmt.Fprintf(w, "%5d  %-15s %-12s %10s %9s  %s\n", op.Index, op.Op, wall, delta, sid, info(op))
	}
}

// humanPhotos prints how many photos are whole, then one line per photo (photoSummary); nothing if there are none.
func humanPhotos(w io.Writer, photos []ReportPhoto) {
	if len(photos) == 0 {
		return
	}

	whole := 0
	for _, photo := range photos {
		if photo.InOrder && photo.Monotonic && photo.Complete {
			whole++
		}
	}

	fmt.Fprintf(w, "photos: %d, %d complete with every stage in order and host times monotonic\n", len(photos), whole)
	for _, photo := range photos {
		fmt.Fprintf(w, "  %d: %s\n", photo.SettingsID, photoSummary(photo))
	}
}

// photoSummary is a photo's line: the stage times it has, then what is wrong with it, if anything.
func photoSummary(photo ReportPhoto) string {
	var parts []string
	if v, ok := photo.Ms["slot12"]; ok {
		parts = append(parts, fmt.Sprintf("slot12 +%.1f ms", v))
	}

	if photo.TimestampMinusSlot12Ms != nil {
		parts = append(parts, fmt.Sprintf("timestamp - slot12 %.1f ms", *photo.TimestampMinusSlot12Ms))
	}

	if v, ok := photo.Ms["still-complete"]; ok {
		parts = append(parts, fmt.Sprintf("still-complete +%.1f ms", v))
	}

	if v, ok := photo.Ms["did-finish"]; ok {
		parts = append(parts, fmt.Sprintf("did-finish +%.1f ms", v))
	}

	var notes []string
	if !photo.Complete {
		notes = append(notes, "incomplete (in flight, overwritten, or failed before the shutter hook)")
	}

	if !photo.InOrder {
		notes = append(notes, "OUT OF ORDER")
	}

	if !photo.Monotonic {
		notes = append(notes, "TIME WENT BACK")
	}

	if photo.Error != 0 {
		notes = append(notes, fmt.Sprintf("error %d", photo.Error))
	}

	if len(notes) > 0 {
		parts = append(parts, strings.Join(notes, ", "))
	}

	return strings.Join(parts, ", ")
}

// humanOthers prints one line per older trace of the same app, with the --pid that shows it.
func humanOthers(w io.Writer, others []ReportOther, loc *time.Location) {
	for _, other := range others {
		otherWritten, _ := time.Parse(time.RFC3339Nano, other.Written) // unparsable: printed as the zero time
		fmt.Fprintf(w, "older trace: pid %d, written %s (--pid %d)\n",
			other.Pid, otherWritten.In(loc).Format("2006-01-02 15:04:05"), other.Pid)
	}
}

// Running reports whether `launchctl list` (run in the simulator) shows pid as bundle's app process: a line
// "<pid> <status> UIKitApplication:<bundle>[…]".
func Running(launchctlList, bundle string, pid int) bool {
	for _, line := range strings.Split(launchctlList, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 3 {
			continue
		}

		label := fields[len(fields)-1]
		if label != "UIKitApplication:"+bundle && !strings.HasPrefix(label, "UIKitApplication:"+bundle+"[") {
			continue
		}

		if listed, err := strconv.Atoi(fields[0]); err == nil && listed == pid {
			return true
		}
	}

	return false
}
