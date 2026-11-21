// SPDX-License-Identifier: Apache-2.0

// Package webcam is the CLI side of the `webcam` source: it finds the Mac cameras, starts and stops the camera helper
// ("Stuntcam Camera.app") and keeps track of which simulators use it. The files it reads and writes are defined in
// docs/webcam-ring.md: the pid file (§8), the default camera (§8a) and the ring's shm name (§1).
package webcam

import (
	"bufio"
	"errors"
	"fmt"
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
	h := uint64(0xcbf29ce484222325)
	add := func(b byte) { h ^= uint64(b); h *= 0x100000001b3 }
	for i := 0; i < len(home); i++ {
		add(home[i])
	}
	add(0)
	for i := 0; i < len(uniqueID); i++ {
		add(uniqueID[i])
	}
	return fmt.Sprintf("%08x", uint32(h))
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

// PidFile is a parsed pid file (spec §8).
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

// ParsePid parses a pid file: the first line exactly "stuntcam-webcam 1", then key=value lines. A known key twice, a
// malformed value or a missing required key is an error; unknown keys are ignored.
func ParsePid(b []byte) (PidFile, error) {
	var p PidFile
	s := string(b)
	if !strings.HasSuffix(s, "\n") {
		return p, errors.New("pid file: not newline-terminated")
	}
	lines := strings.Split(strings.TrimSuffix(s, "\n"), "\n")
	if lines[0] != "stuntcam-webcam 1" {
		return p, fmt.Errorf("pid file: first line %q", lines[0])
	}
	seen := map[string]bool{}
	for _, l := range lines[1:] {
		k, v, ok := strings.Cut(l, "=")
		if !ok {
			return p, fmt.Errorf("pid file: line %q", l)
		}
		known := true
		switch k {
		case "pid":
			n, err := strconv.Atoi(v)
			if err != nil || n < 1 || n > 2147483647 || strconv.Itoa(n) != v {
				return p, fmt.Errorf("pid file: pid=%q", v)
			}
			p.Pid = n
		case "sha":
			if !shaRe.MatchString(v) {
				return p, fmt.Errorf("pid file: sha=%q", v)
			}
			p.Sha = v
		case "rings":
			parts := strings.Split(v, ",")
			if len(parts) > 8 {
				return p, fmt.Errorf("pid file: rings=%q", v)
			}
			for _, r := range parts {
				n, err := strconv.Atoi(r)
				if err != nil || n < 1 || n > 65535 || strconv.Itoa(n) != r {
					return p, fmt.Errorf("pid file: rings=%q", v)
				}
				p.Rings = append(p.Rings, n)
			}
		case "auth":
			if !authOK[v] {
				return p, fmt.Errorf("pid file: auth=%q", v)
			}
			p.Auth = v
		case "state":
			if !stateOK[v] {
				return p, fmt.Errorf("pid file: state=%q", v)
			}
			p.State = v
		case "camera":
			if v == "" || len(v) >= 128 || !printable(v) {
				return p, fmt.Errorf("pid file: camera=%q", v)
			}
			p.Camera = v
		case "started":
			n, err := strconv.ParseInt(v, 10, 64)
			if err != nil || n < 0 {
				return p, fmt.Errorf("pid file: started=%q", v)
			}
			p.Started = n
		default:
			known = false
		}
		if known {
			if seen[k] {
				return p, fmt.Errorf("pid file: %s twice", k)
			}
			seen[k] = true
		}
	}
	for _, k := range []string{"pid", "sha", "rings", "auth", "state"} {
		if !seen[k] {
			return p, fmt.Errorf("pid file: no %s", k)
		}
	}
	return p, nil
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

// ParseList parses `--list` output: "stuntcam-cameras 1", then uniqueID, default, continuity, type, name per line, tab-separated.
func ParseList(b []byte) ([]Camera, error) {
	sc := bufio.NewScanner(strings.NewReader(string(b)))
	if !sc.Scan() || sc.Text() != "stuntcam-cameras 1" {
		return nil, errors.New("camera list: bad first line")
	}
	var cs []Camera
	for sc.Scan() {
		f := strings.SplitN(sc.Text(), "\t", 5)
		if len(f) != 5 || f[0] == "" || (f[1] != "0" && f[1] != "1") || (f[2] != "0" && f[2] != "1") {
			return nil, fmt.Errorf("camera list: line %q", sc.Text())
		}
		cs = append(cs, Camera{ID: f[0], Default: f[1] == "1", Continuity: f[2] == "1", Type: f[3], Name: f[4]})
	}
	return cs, nil
}

// Resolve picks the camera NAME means: "" is the helper's default; otherwise an exact uniqueID, else a localized name
// (case-insensitive), else the one camera whose name contains NAME. "synthetic" is the test pattern.
func Resolve(cs []Camera, name string) (Camera, error) {
	if name == SyntheticID {
		return Camera{ID: SyntheticID, Name: "Synthetic"}, nil
	}
	if name == "" {
		for _, c := range cs {
			if c.Default {
				return c, nil
			}
		}
		return Camera{}, fmt.Errorf("no default camera (no built-in or external camera found)%s", listText(cs))
	}
	for _, c := range cs {
		if c.ID == name {
			return c, nil
		}
	}
	var exact, part []Camera
	for _, c := range cs {
		if strings.EqualFold(c.Name, name) {
			exact = append(exact, c)
		} else if strings.Contains(strings.ToLower(c.Name), strings.ToLower(name)) {
			part = append(part, c)
		}
	}
	for _, m := range [][]Camera{exact, part} {
		switch len(m) {
		case 0:
			continue
		case 1:
			return m[0], nil
		default:
			return Camera{}, fmt.Errorf("%q matches %d cameras: name one by uniqueID%s", name, len(m), listText(m))
		}
	}
	return Camera{}, fmt.Errorf("no camera matches %q%s", name, listText(cs))
}

func listText(cs []Camera) string {
	if len(cs) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString("; cameras:")
	for _, c := range cs {
		fmt.Fprintf(&b, "\n  %s  %s", c.ID, c.Name)
		if c.Default {
			b.WriteString(" (default)")
		}
		if c.Continuity {
			b.WriteString(" (Continuity Camera: used only when named)")
		}
	}
	return b.String()
}

// ReadSession lists the cameras a simulator's webcam session names (the helpers it relies on).
func ReadSession(home, udid string) []string {
	b, err := os.ReadFile(SessionPath(home, udid))
	if err != nil {
		return nil
	}
	var cams []string
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, "camera="); ok && v != "" {
			cams = append(cams, v)
		}
	}
	sort.Strings(cams)
	return cams
}

// SessionUsers lists the simulators (other than except) whose session names camera id.
func SessionUsers(home, id, except string) []string {
	es, _ := os.ReadDir(filepath.Join(home, "sims"))
	var out []string
	for _, e := range es {
		if e.Name() == except {
			continue
		}
		for _, c := range ReadSession(home, e.Name()) {
			if c == id {
				out = append(out, e.Name())
				break
			}
		}
	}
	return out
}
