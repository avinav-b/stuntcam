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

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// The private device keeps the canary between boots, so a verify reinstalls and re-grants it only when needed.
// Install and grant took 5–6.6 s of a warm 27.0 verify under load.
// The decision is made before the boot, from the host side, while the device is shut down:
//   - Install, unless the device's installed m10canary.app has the same content hash as the build's canary, and the
//     install record (CanaryRecordFile) names that hash and that container. The content hash covers every file's
//     path and bytes. The installed copy itself is hashed every time, so the verdict always tests the build's canary,
//     never a stale or foreign one.
//   - Grant camera and microphone after every install. Without an install, grant them unless the device's TCC
//     database already allows both.
//
// simctl install copies the bundle byte for byte (checked on iOS 26.3, 27.0 and 17.5), so the hashes compare directly.

// CanaryRecordFile is the install record for a private device: what this CLI last installed there.
func CanaryRecordFile(udid string) string {
	return filepath.Join(home.Sim(udid), "canary-install.json")
}

type canaryRecord struct {
	Hash      string `json:"hash"`      // bundleSHA256 of the installed app
	Container string `json:"container"` // its host path, under the device's data dir
	Granted   bool   `json:"granted"`   // camera + microphone granted after that install
	At        int64  `json:"at"`
}

// bundleSHA256 is the SHA-256 of an app bundle's files: each regular file's path (relative, sorted) and contents;
// symlinks by their target. Directories alone don't count.
func bundleSHA256(app string) (string, error) {
	type ent struct{ rel, link string }
	var entries []ent
	err := filepath.WalkDir(app, func(walked string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}

		rel, _ := filepath.Rel(app, walked) // can't fail: walked is under app
		switch {
		case entry.Type()&fs.ModeSymlink != 0:
			target, err := os.Readlink(walked)
			if err != nil {
				return err
			}

			entries = append(entries, ent{rel, target})
		case entry.Type().IsRegular():
			entries = append(entries, ent{rel: rel})
		}

		return nil
	})

	if err != nil {
		return "", err
	}

	sort.Slice(entries, func(i, j int) bool { return entries[i].rel < entries[j].rel })
	hasher := sha256.New()
	for _, entry := range entries {
		hasher.Write([]byte(entry.rel + "\x00"))
		if entry.link != "" {
			hasher.Write([]byte("->" + entry.link + "\x00"))
			continue
		}

		data, err := os.ReadFile(filepath.Join(app, entry.rel))
		if err != nil {
			return "", err
		}

		sum := sha256.Sum256(data)
		hasher.Write(sum[:])
	}

	return hex.EncodeToString(hasher.Sum(nil)), nil
}

// installedCanary finds the canary's installed bundle under a device's data dir (host side, booted or not): the one
// Containers/Bundle/Application/<uuid>/m10canary.app whose Info.plist names CanaryBundle. "" if none, or more than one.
func installedCanary(deviceDir string) string {
	matches, _ := filepath.Glob(filepath.Join(deviceDir, "data", "Containers", "Bundle", "Application", "*",
		"m10canary.app"))
	var found []string
	for _, app := range matches {
		data, err := os.ReadFile(filepath.Join(app, "Info.plist"))
		if err == nil && strings.Contains(string(data), "<string>"+CanaryBundle+"</string>") {
			found = append(found, app)
		}
	}

	if len(found) != 1 {
		return ""
	}

	return found[0]
}

// tccQueryTimeout bounds tccGranted's sqlite3 query.
const tccQueryTimeout = 5 * time.Second

// tccGranted says whether a device's TCC database allows camera and microphone for the canary (auth_value 2). It reads
// the database read-only with the system sqlite3; any error is "no", so the grant is then made.
func tccGranted(deviceDir string) bool {
	tccDB := filepath.Join(deviceDir, "data", "Library", "TCC", "TCC.db")
	if _, err := os.Stat(tccDB); err != nil {
		return false
	}

	query := "SELECT service FROM access WHERE client='" + CanaryBundle + "' AND auth_value=2 " +
		"AND service IN ('kTCCServiceCamera','kTCCServiceMicrophone')"
	cmd := exec.Command("/usr/bin/sqlite3", "-readonly", "file:"+tccDB+"?mode=ro", query)
	cmd.Env = append(os.Environ(), "LC_ALL=C")
	out, err := runBounded(cmd, tccQueryTimeout)
	if err != nil {
		return false
	}

	have := map[string]bool{}
	for _, line := range strings.Split(out, "\n") {
		have[strings.TrimSpace(line)] = true
	}

	return have["kTCCServiceCamera"] && have["kTCCServiceMicrophone"]
}

func runBounded(cmd *exec.Cmd, limit time.Duration) (string, error) {
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
	case <-time.After(limit):
		_ = cmd.Process.Kill() // it may have exited just now
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
	want, err := bundleSHA256(canary)
	if err != nil {
		return installPlan{Install: true, Grant: true, Why: "canary hash: " + err.Error(), canary: canary}
	}

	plan := installPlan{Install: true, Grant: true, hash: want, canary: canary}

	var record canaryRecord
	data, readErr := os.ReadFile(CanaryRecordFile(udid))
	haveRecord := readErr == nil && json.Unmarshal(data, &record) == nil
	installed := installedCanary(deviceDir)
	switch {
	case installed == "":
		plan.Why = "canary not installed on the device: installing"
	case !haveRecord:
		plan.Why = "no install record: installing the canary"
	default:
		got, err := bundleSHA256(installed)
		switch {
		case err != nil:
			plan.Why = "installed canary unreadable: reinstalling"
		case got != want:
			plan.Why = "installed canary differs from this build's (" +
				artifacts.ShortHash(got) + " != " + artifacts.ShortHash(want) + "): reinstalling"
		case record.Hash != want || record.Container != installed:
			plan.Why = "install record differs from the installed canary: reinstalling"
		default:
			plan.Install = false
			plan.Grant = !(record.Granted && tccGranted(deviceDir))
			plan.Why = "canary " + artifacts.ShortHash(want) + " already installed"
			if plan.Grant {
				plan.Why += "; granting camera + microphone"
			} else {
				plan.Why += " and granted"
			}
		}
	}

	return plan
}

// recordInstall writes the install record after an install (and grants) succeeded, from the installed copy itself.
func recordInstall(deviceDir, udid, want string, granted bool) error {
	installed := installedCanary(deviceDir)
	if installed == "" || want == "" {
		_ = os.Remove(CanaryRecordFile(udid)) // nothing to record: the next verify reinstalls
		return nil
	}

	if got, err := bundleSHA256(installed); err != nil || got != want {
		_ = os.Remove(CanaryRecordFile(udid)) // not what we installed: the next verify reinstalls
		return nil
	}

	if err := os.MkdirAll(home.Sim(udid), 0o755); err != nil {
		return err
	}

	// can't fail: plain fields
	data, _ := json.MarshalIndent(
		canaryRecord{Hash: want, Container: installed, Granted: granted, At: time.Now().Unix()}, "", " ")
	return home.WriteAtomic(CanaryRecordFile(udid), data)
}
