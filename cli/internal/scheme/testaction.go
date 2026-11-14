// SPDX-License-Identifier: Apache-2.0
package scheme

// What an `xcodebuild test` runs its test runners with (w4 e43, review r1 B1/M2/L3): per test configuration, the
// sanitizers, whether the Main Thread Checker is off, and what the scheme or test plan inserts into the runner itself
// (Guard Malloc, a DYLD_INSERT_LIBRARIES variable). `stuntcam exec`'s runner hook replaces the runner's insert list
// (TEST_RUNNER_DYLD_INSERT_LIBRARIES), so it has to put a sanitizer runtime back in front, and can only do that when every
// runner of the run wants the same one.

import (
	"bytes"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// TestConfig is one test configuration's runner settings: a test plan configuration, an .xctestrun test target, or the
// scheme's test action when it has no test plans.
type TestConfig struct {
	Name        string   // configuration name ("" for a scheme without test plans); for an .xctestrun "<configuration>/<target>"
	Sanitizers  []string // "address", "thread" (sorted)
	NoMTC       bool     // the Main Thread Checker is off for it
	GuardMalloc bool
	Inserts     []string // other libraries the scheme, plan or .xctestrun inserts into the runner (as written)
}

// TestAction is what the test action of one scheme runs.
type TestAction struct {
	Scheme  string // the .xcscheme
	Plan    string // the .xctestplan used ("" if the scheme has none)
	Configs []TestConfig
}

type planRef struct {
	Ref     string `xml:"reference,attr"`
	Default string `xml:"default,attr"`
}

type testActionXML struct {
	Launch struct {
		Env []envVar `xml:"EnvironmentVariables>EnvironmentVariable"`
	} `xml:"LaunchAction"`
	Test struct {
		ASan         string    `xml:"enableAddressSanitizer,attr"`
		TSan         string    `xml:"enableThreadSanitizer,attr"`
		NoMTC        string    `xml:"disableMainThreadChecker,attr"`
		UseLaunchEnv string    `xml:"shouldUseLaunchSchemeArgsEnv,attr"`
		Env          []envVar  `xml:"EnvironmentVariables>EnvironmentVariable"`
		Opts         []envVar  `xml:"AdditionalOptions>AdditionalOption"`
		Plans        []planRef `xml:"TestPlans>TestPlanReference"`
	} `xml:"TestAction"`
}

// FindScheme is the first <name>.xcscheme under root (bounded depth, same skips as Scan), or "".
func FindScheme(root, name string, maxDepth int) string {
	found := ""
	base := strings.Count(filepath.Clean(root), string(filepath.Separator))
	filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil || found != "" {
			return nil
		}
		if d.IsDir() {
			switch d.Name() {
			case "node_modules", "Pods", "build", "DerivedData", ".git", ".dart_tool", ".build":
				return filepath.SkipDir
			}
			if strings.Count(p, string(filepath.Separator))-base > maxDepth {
				return filepath.SkipDir
			}
			return nil
		}
		if filepath.Base(p) == name+".xcscheme" {
			found = p
			return filepath.SkipAll
		}
		return nil
	})
	return found
}

// ReadTestAction reads the test action of scheme file p. With test plans it reads the one named plan (the -testPlan
// argument; "" = the scheme's default plan, else its first) and returns one TestConfig per plan configuration that
// only/skip (-only-test-configuration / -skip-test-configuration) select. Without test plans it returns one TestConfig from
// the scheme's own attributes. An error means the scheme has test plans that can't be read (or not the one named).
func ReadTestAction(p, plan string, only, skip []string) (*TestAction, error) {
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var s testActionXML
	if err := xml.Unmarshal(b, &s); err != nil {
		return nil, fmt.Errorf("%s: %v", p, err)
	}
	ta := &TestAction{Scheme: p}
	t := s.Test
	if len(t.Plans) == 0 {
		if plan != "" {
			return nil, fmt.Errorf("%s has no test plans (-testPlan %s)", filepath.Base(p), plan)
		}
		c := TestConfig{NoMTC: t.NoMTC == "YES"}
		if t.ASan == "YES" {
			c.Sanitizers = append(c.Sanitizers, "address")
		}
		if t.TSan == "YES" {
			c.Sanitizers = append(c.Sanitizers, "thread")
		}
		env := t.Env
		if t.UseLaunchEnv == "YES" { // "Use the Run action's arguments and environment variables"
			env = s.Launch.Env
		}
		for _, v := range append(append([]envVar{}, env...), t.Opts...) { // AdditionalOptions: Guard Malloc and friends
			if v.Key == "DYLD_INSERT_LIBRARIES" && v.Enabled != "NO" {
				c.addInserts(v.Value)
			}
		}
		ta.Configs = []TestConfig{c}
		return ta, nil
	}
	var ref *planRef
	for i := range t.Plans {
		r := &t.Plans[i]
		switch {
		case plan != "":
			if planName(r.Ref) == plan {
				ref = r
			}
		case r.Default == "YES":
			ref = r
		}
		if ref != nil {
			break
		}
	}
	if ref == nil && plan != "" {
		return nil, fmt.Errorf("%s has no test plan %q", filepath.Base(p), plan)
	}
	if ref == nil {
		ref = &t.Plans[0]
	}
	pf := resolveRef(ref.Ref, containerDir(p))
	ta.Plan = pf
	cs, err := readPlan(pf, only, skip)
	if err != nil {
		return nil, err
	}
	ta.Configs = cs
	return ta, nil
}

func planName(ref string) string {
	return strings.TrimSuffix(filepath.Base(ref[strings.Index(ref, ":")+1:]), ".xctestplan")
}

// containerDir is the directory "container:" references in scheme p resolve against: the one holding the .xcodeproj or
// .xcworkspace the scheme belongs to.
func containerDir(p string) string {
	for d := filepath.Dir(p); d != filepath.Dir(d); d = filepath.Dir(d) {
		if strings.HasSuffix(d, ".xcodeproj") || strings.HasSuffix(d, ".xcworkspace") {
			return filepath.Dir(d)
		}
	}
	return filepath.Dir(p)
}

func resolveRef(ref, container string) string {
	kind, path, ok := strings.Cut(ref, ":")
	if !ok {
		path, kind = ref, ""
	}
	if kind == "absolute" || filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(container, path)
}

// Locate finds the .xcscheme xcodebuild would use for -scheme name (r2 M-1): with -project, that project's shared then user
// schemes; with -workspace, the workspace's own schemes, then those of the projects its contents.xcworkspacedata lists; with
// neither, the .xcworkspace or .xcodeproj in wd (as xcodebuild picks it), then a bounded walk under wd. "" if not found.
func Locate(wd, workspace, project, name string) string {
	inProj := func(proj string) string {
		for _, pat := range []string{"xcshareddata/xcschemes/" + name + ".xcscheme", "xcuserdata/*/xcschemes/" + name + ".xcscheme"} {
			if m, _ := filepath.Glob(filepath.Join(proj, pat)); len(m) > 0 {
				return m[0]
			}
		}
		return ""
	}
	inWorkspace := func(ws string) string {
		if p := inProj(ws); p != "" {
			return p
		}
		b, err := os.ReadFile(filepath.Join(ws, "contents.xcworkspacedata"))
		if err != nil {
			return ""
		}
		for _, m := range regexp.MustCompile(`location\s*=\s*"([^"]+)"`).FindAllStringSubmatch(string(b), -1) {
			kind, loc, ok := strings.Cut(m[1], ":")
			if !ok || !strings.HasSuffix(loc, ".xcodeproj") {
				continue
			}
			pp := loc
			switch kind {
			case "absolute":
			case "group", "container", "self":
				pp = filepath.Join(filepath.Dir(ws), loc)
			default:
				continue
			}
			if p := inProj(pp); p != "" {
				return p
			}
		}
		return ""
	}
	switch {
	case project != "":
		return inProj(project)
	case workspace != "":
		if p := inWorkspace(workspace); p != "" {
			return p
		}
		return FindScheme(filepath.Dir(workspace), name, 8)
	}
	if m, _ := filepath.Glob(filepath.Join(wd, "*.xcworkspace")); len(m) == 1 {
		if p := inWorkspace(m[0]); p != "" {
			return p
		}
	}
	if m, _ := filepath.Glob(filepath.Join(wd, "*.xcodeproj")); len(m) == 1 {
		if p := inProj(m[0]); p != "" {
			return p
		}
	}
	return FindScheme(wd, name, 8)
}

// readPlan: one TestConfig per configuration of an .xctestplan (its options over defaultOptions, key by key).
func readPlan(pf string, only, skip []string) ([]TestConfig, error) {
	b, err := os.ReadFile(pf)
	if err != nil {
		return nil, err
	}
	var plan struct {
		Configurations []struct {
			Name    string                     `json:"name"`
			Options map[string]json.RawMessage `json:"options"`
		} `json:"configurations"`
		Defaults map[string]json.RawMessage `json:"defaultOptions"`
	}
	if err := json.Unmarshal(b, &plan); err != nil {
		return nil, fmt.Errorf("%s: %v", pf, err)
	}
	if len(plan.Configurations) == 0 {
		plan.Configurations = append(plan.Configurations, struct {
			Name    string                     `json:"name"`
			Options map[string]json.RawMessage `json:"options"`
		}{})
	}
	var out []TestConfig
	for _, c := range plan.Configurations {
		if (len(only) > 0 && !contains(only, c.Name)) || contains(skip, c.Name) {
			continue
		}
		o := map[string]json.RawMessage{}
		for k, v := range plan.Defaults {
			o[k] = v
		}
		for k, v := range c.Options {
			o[k] = mergeObject(o[k], v) // a configuration's {"addressSanitizer":{…}} overrides key by key (r2 L-3)
		}
		tc := TestConfig{Name: c.Name}
		if enabled(o["addressSanitizer"]) || isTrue(o["addressSanitizerEnabled"]) {
			tc.Sanitizers = append(tc.Sanitizers, "address")
		}
		if enabled(o["threadSanitizer"]) || isTrue(o["threadSanitizerEnabled"]) {
			tc.Sanitizers = append(tc.Sanitizers, "thread")
		}
		tc.NoMTC = isFalse(o["mainThreadCheckerEnabled"])
		tc.GuardMalloc = isTrue(o["guardMallocEnabled"])
		var envs []struct {
			Key     string `json:"key"`
			Value   string `json:"value"`
			Enabled *bool  `json:"enabled"`
		}
		if raw, ok := o["environmentVariableEntries"]; ok {
			if err := json.Unmarshal(raw, &envs); err != nil {
				return nil, fmt.Errorf("%s: environmentVariableEntries: %v", pf, err)
			}
		}
		for _, e := range envs {
			if e.Key == "DYLD_INSERT_LIBRARIES" && (e.Enabled == nil || *e.Enabled) {
				tc.addInserts(e.Value)
			}
		}
		out = append(out, tc)
	}
	return out, nil
}

// mergeObject: over's keys over base's when both are JSON objects; otherwise over.
func mergeObject(base, over json.RawMessage) json.RawMessage {
	var b, o map[string]json.RawMessage
	if base == nil || json.Unmarshal(base, &b) != nil || json.Unmarshal(over, &o) != nil {
		return over
	}
	for k, v := range o {
		b[k] = v
	}
	m, err := json.Marshal(b)
	if err != nil {
		return over
	}
	return m
}

func isTrue(raw json.RawMessage) bool { var b bool; return json.Unmarshal(raw, &b) == nil && b }
func isFalse(raw json.RawMessage) bool {
	var b bool
	return raw != nil && json.Unmarshal(raw, &b) == nil && !b
}
func enabled(raw json.RawMessage) bool {
	var o struct {
		Enabled bool `json:"enabled"`
	}
	return raw != nil && json.Unmarshal(raw, &o) == nil && o.Enabled
}

// addInserts files the entries of an insert list: sanitizer runtimes, Guard Malloc, others.
func (c *TestConfig) addInserts(list string) {
	for _, l := range strings.Split(list, ":") {
		l = strings.TrimSpace(l)
		b := filepath.Base(l)
		switch {
		case l == "":
		case strings.HasPrefix(b, "libclang_rt.asan_"):
			c.addSanitizer("address")
		case strings.HasPrefix(b, "libclang_rt.tsan_"):
			c.addSanitizer("thread")
		case b == "libgmalloc.dylib":
			c.GuardMalloc = true
		case !contains(c.Inserts, l):
			c.Inserts = append(c.Inserts, l)
		}
	}
}

func (c *TestConfig) addSanitizer(s string) {
	if !contains(c.Sanitizers, s) {
		c.Sanitizers = append(c.Sanitizers, s)
		sort.Strings(c.Sanitizers)
	}
}

// XCTestRun reads an .xctestrun (format 1 or 2; XML or binary plist): one TestConfig per test target of the test
// configurations only/skip select (format 2), for the targets onlyTargets names (all if empty), from the runner's own
// insert list (each target's TestingEnvironmentVariables / EnvironmentVariables DYLD_INSERT_LIBRARIES).
func XCTestRun(path string, only, skip, onlyTargets []string) ([]TestConfig, error) {
	root, err := ReadPlist(path)
	if err != nil {
		return nil, err
	}
	top, ok := root.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("%s: not a dictionary", path)
	}
	type tgt struct {
		config, name string
		d            map[string]any
	}
	var tgts []tgt
	if cs, ok := top["TestConfigurations"].([]any); ok { // format 2
		for _, c := range cs {
			cd, _ := c.(map[string]any)
			cn, _ := cd["Name"].(string)
			if (len(only) > 0 && !contains(only, cn)) || contains(skip, cn) {
				continue
			}
			ts, _ := cd["TestTargets"].([]any)
			for _, t := range ts {
				td, _ := t.(map[string]any)
				n, _ := td["BlueprintName"].(string)
				tgts = append(tgts, tgt{cn, n, td})
			}
		}
	} else { // format 1: a dictionary per target
		var names []string
		for k := range top {
			if !strings.HasPrefix(k, "__") {
				names = append(names, k)
			}
		}
		sort.Strings(names)
		for _, k := range names {
			if td, ok := top[k].(map[string]any); ok {
				tgts = append(tgts, tgt{"", k, td})
			}
		}
	}
	var out []TestConfig
	for _, t := range tgts {
		if len(onlyTargets) > 0 && !contains(onlyTargets, t.name) {
			continue
		}
		tc := TestConfig{Name: strings.TrimPrefix(t.config+"/"+t.name, "/")} // (an .xctestrun doesn't say whether the Main Thread Checker is on: xcodebuild adds it)
		for _, k := range []string{"TestingEnvironmentVariables", "EnvironmentVariables"} {
			env, _ := t.d[k].(map[string]any)
			if l, _ := env["DYLD_INSERT_LIBRARIES"].(string); l != "" {
				tc.addInserts(l)
			}
		}
		// what Xcode itself inserts into a runner (not "other" libraries the user's scheme added)
		var other []string
		for _, l := range tc.Inserts {
			switch filepath.Base(l) {
			case "libXCTestBundleInject.dylib", "libMainThreadChecker.dylib", "libRPAC.dylib", "libXCTestSwiftSupport.dylib", "libViewDebuggerSupport.dylib", "libLogRedirect.dylib", "libBacktraceRecording.dylib":
			default:
				other = append(other, l)
			}
		}
		tc.Inserts = other
		out = append(out, tc)
	}
	return out, nil
}

// ReadPlist decodes an XML plist (a binary one through plutil, macOS) into map[string]any, []any, string, bool, int64,
// float64, []byte.
func ReadPlist(path string) (any, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if bytes.HasPrefix(b, []byte("bplist")) {
		if b, err = exec.Command("plutil", "-convert", "xml1", "-o", "-", path).Output(); err != nil {
			return nil, fmt.Errorf("%s: plutil: %v", path, err)
		}
	}
	d := xml.NewDecoder(bytes.NewReader(b))
	d.Strict = false
	for {
		tok, err := d.Token()
		if err != nil {
			return nil, fmt.Errorf("%s: %v", path, err)
		}
		if se, ok := tok.(xml.StartElement); ok {
			if se.Name.Local == "plist" {
				continue
			}
			return plistValue(d, se)
		}
	}
}

func plistValue(d *xml.Decoder, se xml.StartElement) (any, error) {
	switch se.Name.Local {
	case "dict":
		m := map[string]any{}
		key := ""
		for {
			tok, err := d.Token()
			if err != nil {
				return nil, err
			}
			switch t := tok.(type) {
			case xml.EndElement:
				return m, nil
			case xml.StartElement:
				if t.Name.Local == "key" {
					var k string
					if err := d.DecodeElement(&k, &t); err != nil {
						return nil, err
					}
					key = k
					continue
				}
				v, err := plistValue(d, t)
				if err != nil {
					return nil, err
				}
				m[key] = v
			}
		}
	case "array":
		var a []any
		for {
			tok, err := d.Token()
			if err != nil {
				return nil, err
			}
			switch t := tok.(type) {
			case xml.EndElement:
				return a, nil
			case xml.StartElement:
				v, err := plistValue(d, t)
				if err != nil {
					return nil, err
				}
				a = append(a, v)
			}
		}
	case "true", "false":
		if err := d.Skip(); err != nil {
			return nil, err
		}
		return se.Name.Local == "true", nil
	default: // string, integer, real, date, data: their text
		var s string
		if err := d.DecodeElement(&s, &se); err != nil {
			return nil, err
		}
		var n int64
		var f float64
		switch se.Name.Local {
		case "integer":
			if _, err := fmt.Sscan(s, &n); err == nil {
				return n, nil
			}
		case "real":
			if _, err := fmt.Sscan(s, &f); err == nil {
				return f, nil
			}
		}
		return s, nil
	}
}
