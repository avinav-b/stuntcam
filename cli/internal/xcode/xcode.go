// SPDX-License-Identifier: Apache-2.0

// Package xcode identifies the Xcode that `stuntcam up` arms for (design §4.1 "Which Xcode's libs", §4.5 step 1, B-MF5).
//
// Under ⌘R nothing in the launched app names the Xcode that launched it (w4 e26: only DerivedData paths and IDE_*
// variables), so the loader can't learn it in-process. The CLI records the build at arm time (`xcodebuild -version`, which
// follows DEVELOPER_DIR, else `xcode-select -p`) in sims/<udid>/config.json, and the loader reads the restore rules for that
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
	for _, l := range strings.Split(out, "\n") {
		l = strings.TrimSpace(l)
		if v, ok := strings.CutPrefix(l, "Xcode "); ok && version == "" {
			version = strings.TrimSpace(v)
		}
		if b, ok := strings.CutPrefix(l, "Build version "); ok {
			build = strings.TrimSpace(b)
		}
	}
	if version == "" || build == "" {
		return version, build, errors.New("not xcodebuild -version output: " + strings.TrimSpace(out))
	}
	return version, build, nil
}

// Timeout bounds each tool this package runs (xcodebuild -version can hang on a first launch, a license prompt or a
// broken install; w4 e30, L2).
var Timeout = 20 * time.Second

// run is replaced in tests.
var run = func(env []string, name string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), Timeout)
	defer cancel()
	c := exec.CommandContext(ctx, name, args...)
	c.WaitDelay = 2 * time.Second // the kill reaches only the direct child: don't wait for a grandchild holding stdout (r2 L-a)
	if env != nil {
		c.Env = env
	}
	b, err := c.Output()
	if ctx.Err() == context.DeadlineExceeded {
		err = fmt.Errorf("%s timed out after %s", name, Timeout)
	}
	return string(b), err
}

// NormalizeDeveloperDir turns a DEVELOPER_DIR naming the Xcode.app itself (which xcrun accepts) into its
// Contents/Developer, so the same Xcode always compares equal (w4 e30, L3). Other paths are only cleaned.
func NormalizeDeveloperDir(d string) string {
	if d == "" {
		return ""
	}
	d = filepath.Clean(d)
	if strings.HasSuffix(d, ".app") {
		d = filepath.Join(d, "Contents", "Developer")
	}
	return d
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
		i.DeveloperDir, _ = Selected()
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
	ra, err1 := filepath.EvalSymlinks(a)
	rb, err2 := filepath.EvalSymlinks(b)
	if err1 != nil || err2 != nil {
		return filepath.Clean(a) == filepath.Clean(b)
	}
	return ra == rb
}

// Installed lists the Xcodes on the Mac: Spotlight's com.apple.dt.Xcode bundles plus /Applications/Xcode*.app and
// ~/Applications/Xcode*.app (Spotlight can be off), each with its version.plist's version and build.
func Installed() []Info {
	out, _ := run(nil, "mdfind", "kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'")
	paths := ParseMdfind(out)
	globs := []string{"/Applications/Xcode*.app"}
	if h, err := os.UserHomeDir(); err == nil {
		globs = append(globs, filepath.Join(h, "Applications", "Xcode*.app"))
	}
	for _, g := range globs {
		m, _ := filepath.Glob(g)
		paths = append(paths, m...)
	}
	var list []Info
	seen := map[string]bool{}
	for _, p := range paths {
		r, err := filepath.EvalSymlinks(p)
		if err != nil || seen[r] {
			continue
		}
		seen[r] = true
		i := Info{DeveloperDir: filepath.Join(p, "Contents", "Developer")}
		vp := filepath.Join(p, "Contents", "version.plist")
		i.Version, _ = plistString(vp, "CFBundleShortVersionString")
		i.Build, _ = plistString(vp, "ProductBuildVersion")
		list = append(list, i)
	}
	sort.Slice(list, func(a, b int) bool { return list[a].DeveloperDir < list[b].DeveloperDir })
	return list
}

// ParseMdfind keeps the .app paths of mdfind output (one per line), dropping copies inside other bundles and Trash.
func ParseMdfind(out string) []string {
	var ps []string
	sc := bufio.NewScanner(strings.NewReader(out))
	for sc.Scan() {
		p := strings.TrimSpace(sc.Text())
		if !strings.HasSuffix(p, ".app") || strings.Contains(p, "/.Trash/") || strings.Contains(strings.TrimSuffix(p, ".app"), ".app/") {
			continue
		}
		ps = append(ps, p)
	}
	return ps
}

func plistString(path, key string) (string, error) {
	out, err := run(nil, "/usr/libexec/PlistBuddy", "-c", "Print :"+key, path)
	return strings.TrimSpace(out), err
}

// CompareBuilds orders Apple build numbers: <major><train letter><number><suffix> (27A266a < 27A9275 < 27B52). It matches
// the loader's buildcmp.
func CompareBuilds(a, b string) int {
	ma, ra := leadInt(a)
	mb, rb := leadInt(b)
	if ma != mb {
		return cmp(ma, mb)
	}
	if ra != "" && rb != "" && ra[0] != rb[0] {
		return cmp(int(ra[0]), int(rb[0]))
	}
	if (ra == "") != (rb == "") {
		return cmp(len(ra), len(rb))
	}
	if ra != "" {
		ra, rb = ra[1:], rb[1:]
	}
	na, sa := leadInt(ra)
	nb, sb := leadInt(rb)
	if na != nb {
		return cmp(na, nb)
	}
	return strings.Compare(sa, sb)
}

func leadInt(s string) (int, string) {
	i := 0
	for i < len(s) && s[i] >= '0' && s[i] <= '9' {
		i++
	}
	n, _ := strconv.Atoi(s[:i])
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

// Rules picks the restore-rules file for a build in dir, as the loader does: <build>.json if it is usable (verified), else
// the newest build's file if that one is usable (best guess). "" when neither is: the loader then uses its built-in rules.
func Rules(dir, build string) (file string, verified bool) {
	if build != "" {
		f := filepath.Join(dir, build+".json")
		if Usable(f) {
			return f, true
		}
	}
	m, _ := filepath.Glob(filepath.Join(dir, "[0-9]*.json"))
	best := ""
	for _, f := range m {
		b := strings.TrimSuffix(filepath.Base(f), ".json")
		if best == "" || CompareBuilds(b, best) > 0 {
			best = b
		}
	}
	if best == "" || !Usable(filepath.Join(dir, best+".json")) {
		return "", false
	}
	return filepath.Join(dir, best+".json"), false
}

// MaxRulesSize is the loader's rules buffer less its NUL: a larger file is unreadable to the loader (loader.c rulesbuf).
const MaxRulesSize = 32768 - 2

// Usable reports whether the loader would use a rules file: it parses as JSON, has a "rules" value, and fits the loader's
// buffer (loader.c slurp/rulesok).
func Usable(file string) bool {
	b, err := os.ReadFile(file)
	if err != nil || len(b) > MaxRulesSize {
		return false
	}
	var doc map[string]json.RawMessage
	if json.Unmarshal(b, &doc) != nil {
		return false
	}
	_, ok := doc["rules"]
	return ok
}

// UnverifiedLine is the loader's line for a build without rules (design §4.1), so the CLI says the same at `up`.
func UnverifiedLine(build string) string {
	if build == "" {
		build = "(not recorded)"
	}
	return "restore rules unverified for Xcode " + build + "; if console/debugger misbehave use --mode launchd"
}
