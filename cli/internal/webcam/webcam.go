// SPDX-License-Identifier: Apache-2.0

// Package webcam is the CLI side of the `webcam` source: it finds the Mac cameras, starts and stops the camera helper
// ("Stuntcam Camera.app") and keeps track of which simulators use it. The files it reads and writes are defined in
// docs/webcam-ring.md: the pid file (§8), the default camera (§8a) and the ring's shm name (§1).
package webcam

import (
	"bufio"
	"errors"
	"fmt"
	"hash/fnv"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// EngineRings are the ring versions the engine this CLI ships with accepts (engine/src/providers/webcam.inc kWcRings).
var EngineRings = []int{1}

const (
	AppName      = "Stuntcam Camera.app"
	ExeRel       = "Contents/MacOS/stuntcam-camera" // the helper's executable inside the bundle
	SyntheticID  = "synthetic"                      // the uniqueID of the helper's synthetic pattern (no camera, no permission)
	SettingsPath = "System Settings › Privacy & Security › Camera › Stuntcam Camera"
)

// CamHash is <cam> of the ring name and the pid file: the low 32 bits of FNV-1a 64 over home, a 0 byte, then the
// camera's uniqueID, as 8 lowercase hex digits.
func CamHash(home, uniqueID string) string {
	hash := fnv.New64a()
	hash.Write([]byte(home))
	hash.Write([]byte{0})
	hash.Write([]byte(uniqueID))
	return fmt.Sprintf("%08x", uint32(hash.Sum64()))
}

// ShmName is the ring's POSIX shm name for this uid, camera hash and ring version.
func ShmName(uid int, cam string, version int) string {
	return fmt.Sprintf("/scw.%d.%s.v%d", uid, cam, version)
}

// Dir is $SIMCAM_HOME/webcam: pid files, locks, reader files and the default camera.
func Dir(home string) string               { return filepath.Join(home, "webcam") }
func PidPath(home, cam string) string      { return filepath.Join(Dir(home), cam+".pid") }
func DefaultPath(home string) string       { return filepath.Join(Dir(home), "default") }
func LogPath(home, cam string) string      { return filepath.Join(home, "logs", "webcam-"+cam+".log") }
func HelperRoot(home string) string        { return filepath.Join(home, "helper") }
func SessionPath(home, udid string) string { return filepath.Join(home, "sims", udid, "webcam") }

// PidFile is a parsed pid file (docs/webcam-ring.md §8).
type PidFile struct {
	Pid     int
	Sha     string
	Rings   []int
	Auth    string // notDetermined | prompting | authorized | denied
	State   string // starting | prompting | running | idle | denied | stopped | superseded (init only while creating)
	Camera  string
	Started int64 // Unix seconds, 0 if absent
}

var (
	shaRe   = regexp.MustCompile(`^[0-9a-f]{12}$`)
	authOK  = map[string]bool{"notDetermined": true, "prompting": true, "authorized": true, "denied": true}
	stateOK = map[string]bool{
		"init": true, "starting": true, "prompting": true, "running": true,
		"idle": true, "denied": true, "stopped": true, "superseded": true,
	}
)

// The bounds ParsePid accepts; a value outside them makes the pid file malformed.
const (
	maxPid          = math.MaxInt32 // a pid_t is a 32-bit signed integer
	maxPidFileRings = 8             // ring versions one helper may serve
	maxRingVersion  = 65535
	cameraNameLimit = 128 // a camera= value is shorter than this many bytes
)

// ParsePid parses a pid file: the first line exactly "stuntcam-webcam 1", then key=value lines. A known key twice, a
// malformed value or a missing required key is an error; unknown keys are ignored.
func ParsePid(data []byte) (PidFile, error) {
	var parsed PidFile
	text := string(data)
	if !strings.HasSuffix(text, "\n") {
		return parsed, errors.New("pid file: not newline-terminated")
	}

	lines := strings.Split(strings.TrimSuffix(text, "\n"), "\n")
	if lines[0] != "stuntcam-webcam 1" {
		return parsed, fmt.Errorf("pid file: first line %q", lines[0])
	}

	seen := map[string]bool{}
	for _, line := range lines[1:] {
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			return parsed, fmt.Errorf("pid file: line %q", line)
		}

		known, err := parsed.setField(key, value)
		if err != nil {
			return parsed, err
		}

		if !known {
			continue
		}

		if seen[key] {
			return parsed, fmt.Errorf("pid file: %s twice", key)
		}

		seen[key] = true
	}

	for _, key := range []string{"pid", "sha", "rings", "auth", "state"} {
		if !seen[key] {
			return parsed, fmt.Errorf("pid file: no %s", key)
		}
	}

	return parsed, nil
}

// setField stores the value of one pid-file line in p, after checking it. It reports whether the key is known: an
// unknown key changes nothing.
func (p *PidFile) setField(key, value string) (bool, error) {
	switch key {
	case "pid":
		n, ok := canonicalInt(value, maxPid)
		if !ok {
			return true, fmt.Errorf("pid file: pid=%q", value)
		}

		p.Pid = n
	case "sha":
		if !shaRe.MatchString(value) {
			return true, fmt.Errorf("pid file: sha=%q", value)
		}

		p.Sha = value
	case "rings":
		return true, p.setRings(value)
	case "auth":
		if !authOK[value] {
			return true, fmt.Errorf("pid file: auth=%q", value)
		}

		p.Auth = value
	case "state":
		if !stateOK[value] {
			return true, fmt.Errorf("pid file: state=%q", value)
		}

		p.State = value
	case "camera":
		if value == "" || len(value) >= cameraNameLimit || !printable(value) {
			return true, fmt.Errorf("pid file: camera=%q", value)
		}

		p.Camera = value
	case "started":
		n, err := strconv.ParseInt(value, 10, 64)
		if err != nil || n < 0 {
			return true, fmt.Errorf("pid file: started=%q", value)
		}

		p.Started = n
	default:
		return false, nil
	}

	return true, nil
}

// setRings appends the ring versions of a rings= value (such as "1,2") to p.Rings: at most maxPidFileRings, each from
// 1 to maxRingVersion. A bad version stops it there, with an error.
func (p *PidFile) setRings(value string) error {
	parts := strings.Split(value, ",")
	if len(parts) > maxPidFileRings {
		return fmt.Errorf("pid file: rings=%q", value)
	}

	for _, ring := range parts {
		n, ok := canonicalInt(ring, maxRingVersion)
		if !ok {
			return fmt.Errorf("pid file: rings=%q", value)
		}

		p.Rings = append(p.Rings, n)
	}

	return nil
}

// canonicalInt parses s as an integer from 1 to limit, written the canonical way: no sign, no leading zeros, no spaces.
func canonicalInt(s string, limit int) (int, bool) {
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 || n > limit || strconv.Itoa(n) != s {
		return 0, false
	}

	return n, true
}

func printable(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < 0x20 || s[i] == 0x7f {
			return false
		}
	}
	return true
}

// SharedRing is the newest ring version both the helper and the engine know, or 0.
func SharedRing(helper, engine []int) int {
	best := 0
	for _, h := range helper {
		for _, e := range engine {
			if h == e && h > best {
				best = h
			}
		}
	}
	return best
}

func ringsText(r []int) string {
	var s []string
	for _, v := range r {
		s = append(s, "v"+strconv.Itoa(v))
	}
	return strings.Join(s, ",")
}

// Camera is one line of the helper's --list output.
type Camera struct {
	ID, Type, Name      string
	Default, Continuity bool
}

// ParseList parses `--list` output: "stuntcam-cameras 1", then uniqueID, default, continuity, type, name per line,
// tab-separated.
func ParseList(data []byte) ([]Camera, error) {
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	if !scanner.Scan() || scanner.Text() != "stuntcam-cameras 1" {
		return nil, errors.New("camera list: bad first line")
	}

	var cameras []Camera
	isFlag := func(field string) bool { return field == "0" || field == "1" }
	for scanner.Scan() {
		fields := strings.SplitN(scanner.Text(), "\t", 5)
		if len(fields) != 5 || fields[0] == "" || !isFlag(fields[1]) || !isFlag(fields[2]) {
			return nil, fmt.Errorf("camera list: line %q", scanner.Text())
		}

		cameras = append(cameras, Camera{ID: fields[0], Default: fields[1] == "1", Continuity: fields[2] == "1",
			Type: fields[3], Name: fields[4]})
	}

	return cameras, nil
}

// Resolve picks the camera NAME means: "" is the helper's default; otherwise an exact uniqueID, else a localized name
// (case-insensitive), else the one camera whose name contains NAME. "synthetic" is the test pattern.
func Resolve(cameras []Camera, name string) (Camera, error) {
	if name == SyntheticID {
		return Camera{ID: SyntheticID, Name: "Synthetic"}, nil
	}

	if name == "" {
		for _, c := range cameras {
			if c.Default {
				return c, nil
			}
		}

		return Camera{}, fmt.Errorf("no default camera (no built-in or external camera found)%s", listText(cameras))
	}

	for _, c := range cameras {
		if c.ID == name {
			return c, nil
		}
	}

	var exact, part []Camera
	for _, c := range cameras {
		if strings.EqualFold(c.Name, name) {
			exact = append(exact, c)
		} else if strings.Contains(strings.ToLower(c.Name), strings.ToLower(name)) {
			part = append(part, c)
		}
	}

	for _, matches := range [][]Camera{exact, part} {
		switch len(matches) {
		case 0:
			continue
		case 1:
			return matches[0], nil
		default:
			return Camera{}, fmt.Errorf("%q matches %d cameras: name one by uniqueID%s", name, len(matches),
				listText(matches))
		}
	}

	return Camera{}, fmt.Errorf("no camera matches %q%s", name, listText(cameras))
}

func listText(cameras []Camera) string {
	if len(cameras) == 0 {
		return ""
	}

	var text strings.Builder
	text.WriteString("; cameras:")
	for _, camera := range cameras {
		fmt.Fprintf(&text, "\n  %s  %s", camera.ID, camera.Name)
		if camera.Default {
			text.WriteString(" (default)")
		}

		if camera.Continuity {
			text.WriteString(" (Continuity Camera: used only when named)")
		}
	}

	return text.String()
}

// ReadSession lists the cameras a simulator's webcam session names (the helpers it relies on).
func ReadSession(home, udid string) []string {
	data, err := os.ReadFile(SessionPath(home, udid))
	if err != nil {
		return nil
	}

	var cams []string
	for _, line := range strings.Split(string(data), "\n") {
		if camera, ok := strings.CutPrefix(line, "camera="); ok && camera != "" {
			cams = append(cams, camera)
		}
	}

	sort.Strings(cams)
	return cams
}

// SessionUsers lists the simulators (other than except) whose session names camera id.
func SessionUsers(home, id, except string) []string {
	entries, _ := os.ReadDir(filepath.Join(home, "sims")) // no dir: no sessions
	var out []string
	for _, entry := range entries {
		if entry.Name() == except {
			continue
		}

		for _, camera := range ReadSession(home, entry.Name()) {
			if camera == id {
				out = append(out, entry.Name())
				break
			}
		}
	}

	return out
}
