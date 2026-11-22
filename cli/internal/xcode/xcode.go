// SPDX-License-Identifier: Apache-2.0

// Package xcode identifies the Xcode that `stuntcam up` arms for, whose restore rules the loader then applies.
//
// Under ⌘R nothing in the launched app names the Xcode that launched it (only DerivedData paths and IDE_* variables),
// so the loader can't learn it in-process. The CLI records the build at arm time (`xcodebuild -version`, which follows
// DEVELOPER_DIR, else `xcode-select -p`) in sims/<udid>/config.json, and the loader reads the restore rules for that
// build. An Xcode switched after arming is noticed by `doctor`, not by the app.
package xcode

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Info is one Xcode: its version, build and developer directory (…/Xcode.app/Contents/Developer).
type Info struct {
	Version      string `json:"xcode_version"`
	Build        string `json:"xcode_build"`
	DeveloperDir string `json:"developer_dir"`
}

// ParseVersion reads `xcodebuild -version` output ("Xcode 27.0\nBuild version 27A266a").
func ParseVersion(out string) (version, build string, err error) {
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if v, ok := strings.CutPrefix(line, "Xcode "); ok && version == "" {
			version = strings.TrimSpace(v)
		}

		if b, ok := strings.CutPrefix(line, "Build version "); ok {
			build = strings.TrimSpace(b)
		}
	}

	if version == "" || build == "" {
		return version, build, errors.New("not xcodebuild -version output: " + strings.TrimSpace(out))
	}

	return version, build, nil
}

// Timeout bounds each tool this package runs (xcodebuild -version can hang on a first launch, a license prompt or a
// broken install).
var Timeout = 20 * time.Second

// pipeWait bounds how long a killed tool's output pipes may stay open: the kill reaches only the direct child, so a
// grandchild can still hold stdout.
const pipeWait = 2 * time.Second

// run is replaced in tests.
var run = func(env []string, name string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), Timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.WaitDelay = pipeWait
	if env != nil {
		cmd.Env = env
	}
	out, err := cmd.Output()
	if ctx.Err() == context.DeadlineExceeded {
		err = fmt.Errorf("%s timed out after %s", name, Timeout)
	}
	return string(out), err
}

// NormalizeDeveloperDir turns a DEVELOPER_DIR naming the Xcode.app itself (which xcrun accepts) into its
// Contents/Developer, so the same Xcode always compares equal. Other paths are only cleaned.
func NormalizeDeveloperDir(dir string) string {
	if dir == "" {
		return ""
	}
	dir = filepath.Clean(dir)
	if strings.HasSuffix(dir, ".app") {
		dir = filepath.Join(dir, "Contents", "Developer")
	}
	return dir
}

// Active is the Xcode that xcodebuild/xcrun use now: DEVELOPER_DIR when set, else `xcode-select -p`.
func Active() (Info, error) {
	var i Info
	out, err := run(nil, "xcodebuild", "-version")
	if err != nil {
		return i, errors.New("xcodebuild -version failed: " + strings.TrimSpace(out) + errText(err))
	}

	if i.Version, i.Build, err = ParseVersion(out); err != nil {
		return i, err
	}

	i.DeveloperDir = os.Getenv("DEVELOPER_DIR")
	if i.DeveloperDir == "" {
		i.DeveloperDir, _ = Selected() // "" if xcode-select fails: NormalizeDeveloperDir handles it
	}

	i.DeveloperDir = NormalizeDeveloperDir(i.DeveloperDir)
	return i, nil
}

// Selected is `xcode-select -p` without DEVELOPER_DIR (which xcode-select would echo back).
func Selected() (string, error) {
	var env []string
	for _, kv := range os.Environ() {
		if !strings.HasPrefix(kv, "DEVELOPER_DIR=") {
			env = append(env, kv)
		}
	}
	out, err := run(env, "xcode-select", "-p")
	return NormalizeDeveloperDir(strings.TrimSpace(out)), err
}

func errText(err error) string {
	if err == nil {
		return ""
	}
	return " (" + err.Error() + ")"
}

// App is the Xcode.app of a developer directory ("" if it isn't inside one).
func App(developerDir string) string {
	d := filepath.Clean(developerDir)
	if strings.HasSuffix(d, "/Contents/Developer") {
		return strings.TrimSuffix(d, "/Contents/Developer")
	}
	return ""
}

// Same reports whether two developer directories are the same Xcode (symlinks resolved).
func Same(a, b string) bool {
	realA, err1 := filepath.EvalSymlinks(a)
	realB, err2 := filepath.EvalSymlinks(b)
	if err1 != nil || err2 != nil {
		return filepath.Clean(a) == filepath.Clean(b)
	}
	return realA == realB
}

// Installed lists the Xcodes on the Mac: Spotlight's com.apple.dt.Xcode bundles plus /Applications/Xcode*.app and
// ~/Applications/Xcode*.app (Spotlight can be off), each with its version.plist's version and build.
func Installed() []Info {
	// Spotlight may be off: the globs below still find Xcodes
	out, _ := run(nil, "mdfind", "kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'")
	paths := ParseMdfind(out)
	globs := []string{"/Applications/Xcode*.app"}
	if homeDir, err := os.UserHomeDir(); err == nil {
		globs = append(globs, filepath.Join(homeDir, "Applications", "Xcode*.app"))
	}

	for _, glob := range globs {
		matches, _ := filepath.Glob(glob)
		paths = append(paths, matches...)
	}

	var list []Info
	seen := map[string]bool{}
	for _, path := range paths {
		resolved, err := filepath.EvalSymlinks(path)
		if err != nil || seen[resolved] {
			continue
		}

		seen[resolved] = true
		info := Info{DeveloperDir: filepath.Join(path, "Contents", "Developer")}
		versionPlist := filepath.Join(path, "Contents", "version.plist")
		// unreadable: version and build stay ""
		info.Version, _ = plistString(versionPlist, "CFBundleShortVersionString")
		info.Build, _ = plistString(versionPlist, "ProductBuildVersion")
		list = append(list, info)
	}

	sort.Slice(list, func(a, b int) bool { return list[a].DeveloperDir < list[b].DeveloperDir })
	return list
}

// ParseMdfind keeps the .app paths of mdfind output (one per line), dropping copies inside other bundles and Trash.
func ParseMdfind(out string) []string {
	var paths []string
	scanner := bufio.NewScanner(strings.NewReader(out))
	for scanner.Scan() {
		path := strings.TrimSpace(scanner.Text())
		insideApp := strings.Contains(strings.TrimSuffix(path, ".app"), ".app/") // an app inside another app's bundle
		if !strings.HasSuffix(path, ".app") || strings.Contains(path, "/.Trash/") || insideApp {
			continue
		}

		paths = append(paths, path)
	}

	return paths
}

func plistString(path, key string) (string, error) {
	out, err := run(nil, "/usr/libexec/PlistBuddy", "-c", "Print :"+key, path)
	return strings.TrimSpace(out), err
}

// CompareBuilds orders Apple build numbers: <major><train letter><number><suffix> (27A266a < 27A9275 < 27B52). It
// matches the loader's build_compare.
func CompareBuilds(a, b string) int {
	majorA, restA := leadInt(a)
	majorB, restB := leadInt(b)
	if majorA != majorB {
		return cmp(majorA, majorB)
	}

	if restA != "" && restB != "" && restA[0] != restB[0] {
		return cmp(int(restA[0]), int(restB[0]))
	}

	if (restA == "") != (restB == "") {
		return cmp(len(restA), len(restB))
	}

	if restA != "" {
		restA, restB = restA[1:], restB[1:]
	}

	numberA, suffixA := leadInt(restA)
	numberB, suffixB := leadInt(restB)
	if numberA != numberB {
		return cmp(numberA, numberB)
	}

	return strings.Compare(suffixA, suffixB)
}

func leadInt(s string) (int, string) {
	i := 0
	for i < len(s) && s[i] >= '0' && s[i] <= '9' {
		i++
	}
	n, _ := strconv.Atoi(s[:i]) // digits only; "" is 0
	return n, s[i:]
}

func cmp(a, b int) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// Rules picks the restore-rules file for a build in dir, as the loader does.
//   - <build>.json, if it is usable. That is a verified pick.
//   - Otherwise the newest build's file, if that one is usable. That is a best guess.
//
// It returns "" when neither is usable. The loader then uses its built-in rules.
func Rules(dir, build string) (file string, verified bool) {
	if build != "" {
		exact := filepath.Join(dir, build+".json")
		if Usable(exact) {
			return exact, true
		}
	}

	matches, _ := filepath.Glob(filepath.Join(dir, "[0-9]*.json"))
	best := ""
	for _, path := range matches {
		candidate := strings.TrimSuffix(filepath.Base(path), ".json")
		if best == "" || CompareBuilds(candidate, best) > 0 {
			best = candidate
		}
	}

	if best == "" || !Usable(filepath.Join(dir, best+".json")) {
		return "", false
	}

	return filepath.Join(dir, best+".json"), false
}

// MaxRulesSize is the loader's rules buffer less its NUL: a larger file is unreadable to the loader (loader.c
// gRulesJson).
const MaxRulesSize = 32768 - 2

// Usable reports whether the loader would use a rules file: it parses as JSON, has a "rules" value, and fits the
// loader's buffer (loader.c read_whole_file/rules_usable).
func Usable(file string) bool {
	data, err := os.ReadFile(file)
	if err != nil || len(data) > MaxRulesSize {
		return false
	}

	var doc map[string]json.RawMessage
	if json.Unmarshal(data, &doc) != nil {
		return false
	}

	_, ok := doc["rules"]
	return ok
}

// UnverifiedLine is the line the loader logs for an Xcode build without restore rules, so the CLI says the same at
// `up`.
func UnverifiedLine(build string) string {
	if build == "" {
		build = "(not recorded)"
	}
	return "restore rules unverified for Xcode " + build + "; if console/debugger misbehave use --mode launchd"
}
