// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// The private device keeps the canary between boots, so a verify reinstalls and re-grants it only when needed
// (install + grant were 5–6.6 s of a warm 27.0 verify under load). The decision is made before the boot, from
// the host side, while the device is shut down:
//   - install unless the device's installed m10canary.app has the same content hash (every file's path and bytes) as the
//     build's canary and the install record (CanaryRecordFile) names that hash and that container. The installed copy itself
//     is hashed every time, so the verdict always tests the build's canary, never a stale or foreign one;
//   - grant camera + microphone after every install, and otherwise unless the device's TCC database already allows both.
//
// simctl install copies the bundle byte for byte (checked on iOS 26.3, 27.0 and 17.5), so the hashes compare directly.

// CanaryRecordFile is the install record for a private device: what this CLI last installed there.
func CanaryRecordFile(udid string) string {
	return filepath.Join(home.Sim(udid), "canary-install.json")
}

type canaryRecord struct {
	Hash      string `json:"hash"`      // BundleHash of the installed app
	Container string `json:"container"` // its host path, under the device's data dir
	Granted   bool   `json:"granted"`   // camera + microphone granted after that install
	At        int64  `json:"at"`
}

// BundleHash is the SHA-256 of an app bundle's files: each regular file's path (relative, sorted) and contents; symlinks
// by their target. Directories alone don't count.
func BundleHash(app string) (string, error) {
	type ent struct{ rel, link string }
	var es []ent
	err := filepath.WalkDir(app, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(app, p)
		switch {
		case d.Type()&fs.ModeSymlink != 0:
			t, err := os.Readlink(p)
			if err != nil {
				return err
			}
			es = append(es, ent{rel, t})
		case d.Type().IsRegular():
			es = append(es, ent{rel: rel})
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	sort.Slice(es, func(i, j int) bool { return es[i].rel < es[j].rel })
	h := sha256.New()
	for _, e := range es {
		h.Write([]byte(e.rel + "\x00"))
		if e.link != "" {
			h.Write([]byte("->" + e.link + "\x00"))
			continue
		}
		b, err := os.ReadFile(filepath.Join(app, e.rel))
		if err != nil {
			return "", err
		}
		s := sha256.Sum256(b)
		h.Write(s[:])
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// installedCanary finds the canary's installed bundle under a device's data dir (host side, booted or not): the one
// Containers/Bundle/Application/<uuid>/m10canary.app whose Info.plist names CanaryBundle. "" if none, or more than one.
func installedCanary(deviceDir string) string {
	ms, _ := filepath.Glob(filepath.Join(deviceDir, "data", "Containers", "Bundle", "Application", "*", "m10canary.app"))
	var found []string
	for _, m := range ms {
		b, err := os.ReadFile(filepath.Join(m, "Info.plist"))
		if err == nil && strings.Contains(string(b), "<string>"+CanaryBundle+"</string>") {
			found = append(found, m)
		}
	}
	if len(found) != 1 {
		return ""
	}
	return found[0]
}

// tccGranted says whether a device's TCC database allows camera and microphone for the canary (auth_value 2). It reads
// the database read-only with the system sqlite3; any error is "no", so the grant is then made.
func tccGranted(deviceDir string) bool {
	db := filepath.Join(deviceDir, "data", "Library", "TCC", "TCC.db")
	if _, err := os.Stat(db); err != nil {
		return false
	}
	q := "SELECT service FROM access WHERE client='" + CanaryBundle + "' AND auth_value=2 AND service IN ('kTCCServiceCamera','kTCCServiceMicrophone')"
	cmd := exec.Command("/usr/bin/sqlite3", "-readonly", "file:"+db+"?mode=ro", q)
	cmd.Env = append(os.Environ(), "LC_ALL=C")
	out, err := runBounded(cmd, 5*time.Second)
	if err != nil {
		return false
	}
	have := map[string]bool{}
	for _, l := range strings.Split(out, "\n") {
		have[strings.TrimSpace(l)] = true
	}
	return have["kTCCServiceCamera"] && have["kTCCServiceMicrophone"]
}

func runBounded(cmd *exec.Cmd, d time.Duration) (string, error) {
	var out strings.Builder
	cmd.Stdout = &out
	if err := cmd.Start(); err != nil {
		return "", err
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		return out.String(), err
	case <-time.After(d):
		_ = cmd.Process.Kill()
		go func() { <-done }() // reaped later: a child stuck in the kernel must not hold the guard past d
		return "", os.ErrDeadlineExceeded
	}
}

// installPlan is what bootInstall has to do for the canary on a device.
type installPlan struct {
	Install, Grant bool
	Why            string // for the notes: why it (re)installs, or that it doesn't
	hash           string // the build's canary
	canary         string // its path
}

func planInstall(deviceDir, udid, canary string) installPlan {
	want, err := BundleHash(canary)
	if err != nil {
		return installPlan{Install: true, Grant: true, Why: "canary hash: " + err.Error(), canary: canary}
	}
	p := installPlan{Install: true, Grant: true, hash: want, canary: canary}
	var rec canaryRecord
	b, rerr := os.ReadFile(CanaryRecordFile(udid))
	haveRec := rerr == nil && json.Unmarshal(b, &rec) == nil
	inst := installedCanary(deviceDir)
	switch {
	case inst == "":
		p.Why = "canary not installed on the device: installing"
	case !haveRec:
		p.Why = "no install record: installing the canary"
	default:
		got, err := BundleHash(inst)
		switch {
		case err != nil:
			p.Why = "installed canary unreadable: reinstalling"
		case got != want:
			p.Why = "installed canary differs from this build's (" + short(got) + " != " + short(want) + "): reinstalling"
		case rec.Hash != want || rec.Container != inst:
			p.Why = "install record differs from the installed canary: reinstalling"
		default:
			p.Install = false
			p.Grant = !(rec.Granted && tccGranted(deviceDir))
			p.Why = "canary " + short(want) + " already installed"
			if p.Grant {
				p.Why += "; granting camera + microphone"
			} else {
				p.Why += " and granted"
			}
		}
	}
	return p
}

func short(h string) string {
	if len(h) > 12 {
		return h[:12]
	}
	return h
}

// recordInstall writes the install record after an install (and grants) succeeded, from the installed copy itself.
func recordInstall(deviceDir, udid, want string, granted bool) error {
	inst := installedCanary(deviceDir)
	if inst == "" || want == "" {
		_ = os.Remove(CanaryRecordFile(udid))
		return nil
	}
	if got, err := BundleHash(inst); err != nil || got != want {
		_ = os.Remove(CanaryRecordFile(udid)) // not what we installed: the next verify reinstalls
		return nil
	}
	if err := os.MkdirAll(home.Sim(udid), 0o755); err != nil {
		return err
	}
	b, _ := json.MarshalIndent(canaryRecord{Hash: want, Container: inst, Granted: granted, At: time.Now().Unix()}, "", " ")
	return home.WriteAtomic(CanaryRecordFile(udid), b)
}
