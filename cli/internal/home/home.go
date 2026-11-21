// SPDX-License-Identifier: Apache-2.0

// Package home lays out stuntcam's host directory and the small text formats the simulator side reads.
//
// Everything a simulator process reads or writes lives here, never in a project checkout: simulator processes cannot see
// macOS privacy-protected folders (~/Documents, ~/Desktop, ~/Downloads, iCloud Drive) and get "no such file" without any
// log line.
package home

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Proto is the version of the host files the stub, loader and engine read (design §4.1 item 4).
const Proto = 1

// Root returns $SIMCAM_HOME, else ~/Library/Application Support/stuntcam.
func Root() string {
	if h := os.Getenv("SIMCAM_HOME"); h != "" {
		return h
	}
	d, err := os.UserHomeDir()
	if err != nil {
		d = "/tmp"
	}
	return filepath.Join(d, "Library", "Application Support", "stuntcam")
}

func Builds() string   { return filepath.Join(Root(), "builds") }
func Devices() string  { return filepath.Join(Root(), "devices") } // the private device set (simctl --set) for the canary
func Verdicts() string { return filepath.Join(Root(), "verdicts") }

// Canary holds the canary apps unpacked from an installed package's payload, at <id>/m10canary.app (id: a hash of its files).
func Canary() string { return filepath.Join(Root(), "canary") }

func Logs() string { return filepath.Join(Root(), "logs") }

// Dev holds profiles/ and media/ as rsync'd by the last checkout that sourced scripts/env.sh (the scripts use dev/<hash>/).
func Dev() string { return filepath.Join(Root(), "dev") }

// Data holds the profiles and media the CLI stages, at content-hash paths (data/<sha1-12>/<name>): an armed sim's SIMCAM_PROFILE
// and SIMCAM_FIXTURE_IMAGE never change under it when another `up`, `verify` or script stages a different file of the same name.
func Data() string { return filepath.Join(Root(), "data") }

// Sim is one simulator's directory: current (the loader to use), chain.txt, state/ (sentinel events per bundle id).
func Sim(udid string) string { return filepath.Join(Root(), "sims", udid) }

func CurrentFile(udid string) string { return filepath.Join(Sim(udid), "current") }

// ConfigFile is the arm-time record the loader reads: the Xcode build whose restore rules apply (arm.Config).
func ConfigFile(udid string) string { return filepath.Join(Sim(udid), "config.json") }

// Rules holds the staged restore-rules/<xcode build>.json files the loader reads (a checkout's restore-rules/ is under
// ~/Documents for many people, which simulator processes can't read).
func Rules() string                { return filepath.Join(Root(), "restore-rules") }
func ChainFile(udid string) string { return filepath.Join(Sim(udid), "chain.txt") }
func StateDir(udid string) string  { return filepath.Join(Sim(udid), "state") }

// ControlFile is the file the engine polls every 250 ms in each app (SIMCAM_CONTROL): source=, orientation=, event= lines
// (engine/src/core/control.inc). source and orientation also apply to apps launched later.
func ControlFile(udid string) string { return filepath.Join(Sim(udid), "control") }

// WriteCurrent points a sim at a loader, atomically ("<proto> <absolute path>\n"), so a launch never reads half a line.
func WriteCurrent(udid, loader string) error {
	if !filepath.IsAbs(loader) {
		return fmt.Errorf("loader path must be absolute: %s", loader)
	}
	if err := os.MkdirAll(Sim(udid), 0o755); err != nil {
		return err
	}
	return WriteAtomic(CurrentFile(udid), []byte(fmt.Sprintf("%d %s\n", Proto, loader)))
}

// ReadCurrent parses a current file the way the stub does: proto >= 1 and an absolute path, else an error.
func ReadCurrent(udid string) (proto int, loader string, err error) {
	b, err := os.ReadFile(CurrentFile(udid))
	if err != nil {
		return 0, "", err
	}
	return ParseCurrent(string(b))
}

func ParseCurrent(s string) (proto int, loader string, err error) {
	line, _, _ := strings.Cut(s, "\n")
	p, path, ok := strings.Cut(line, " ")
	if !ok {
		return 0, "", fmt.Errorf("current is not '<proto> <path>': %q", line)
	}
	proto, err = strconv.Atoi(p)
	if err != nil || proto < 1 {
		return 0, "", fmt.Errorf("current has no valid proto: %q", line)
	}
	if proto > Proto { // the stub refuses a newer proto ("needs a newer stub")
		return 0, "", fmt.Errorf("current has proto %d; this stuntcam (and its stub) understand %d", proto, Proto)
	}
	if !filepath.IsAbs(path) {
		return 0, "", fmt.Errorf("current names a relative path: %q", path)
	}
	return proto, path, nil
}

// WriteAtomic writes via a temp file in the same directory and renames it into place (mode 0644).
func WriteAtomic(path string, data []byte) error { return WriteAtomicPerm(path, data, 0o644) }

// WriteAtomicPerm is WriteAtomic with the file's mode.
func WriteAtomicPerm(path string, data []byte, perm os.FileMode) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}
	if err := tmp.Chmod(perm); err != nil {
		tmp.Close()
		os.Remove(tmp.Name())
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		os.Remove(tmp.Name())
		return err
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	if err := os.Rename(tmp.Name(), path); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	return nil
}

// Protected reports whether a path is inside a folder simulator processes can't reach.
func Protected(path string) bool {
	d, err := os.UserHomeDir()
	if err != nil {
		return false
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return false
	}
	for _, p := range []string{"Documents", "Desktop", "Downloads", "Library/Mobile Documents", "Library/CloudStorage"} {
		base := filepath.Join(d, p)
		if abs == base || strings.HasPrefix(abs, base+string(filepath.Separator)) {
			return true
		}
	}
	return false
}
