// SPDX-License-Identifier: Apache-2.0
package scheme

// What an `xcodebuild test` runs its test runners with: per test configuration, the sanitizers, whether the Main Thread
// Checker is off, and what the scheme or test plan inserts into the runner itself (Guard Malloc, a
// DYLD_INSERT_LIBRARIES variable). `stuntcam exec`'s runner hook replaces the runner's insert list
// (TEST_RUNNER_DYLD_INSERT_LIBRARIES), so it has to put a sanitizer runtime back in front, and can only do that when
// every runner of the run wants the same one.

import (
	"bytes"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
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
	walkFiles(root, maxDepth, func(p string) error {
		if filepath.Base(p) == name+".xcscheme" {
			found = p
			return filepath.SkipAll
		}
		return nil
	})
	return found
}

// schemeSearchDepth is how many directory levels Locate's bounded walk looks down for a scheme.
const schemeSearchDepth = 8

// ReadTestAction reads the test action of the scheme file at schemePath.
//   - With test plans, it reads the plan that plan names: the -testPlan argument. "" means the scheme's default plan,
//     or else its first. It returns one TestConfig per plan configuration that only and skip select
//     (-only-test-configuration and -skip-test-configuration).
//   - Without test plans, it returns one TestConfig from the scheme's own attributes.
//
// An error means the scheme has test plans that can't be read, or doesn't have the one named.
func ReadTestAction(schemePath, plan string, only, skip []string) (*TestAction, error) {
	data, err := os.ReadFile(schemePath)
	if err != nil {
		return nil, err
	}

	var parsed testActionXML
	if err := xml.Unmarshal(data, &parsed); err != nil {
		return nil, fmt.Errorf("%s: %w", schemePath, err)
	}

	action := &TestAction{Scheme: schemePath}
	test := parsed.Test
	if len(test.Plans) == 0 {
		if plan != "" {
			return nil, fmt.Errorf("%s has no test plans (-testPlan %s)", filepath.Base(schemePath), plan)
		}

		config := TestConfig{NoMTC: test.NoMTC == "YES"}
		if test.ASan == "YES" {
			config.Sanitizers = append(config.Sanitizers, "address")
		}

		if test.TSan == "YES" {
			config.Sanitizers = append(config.Sanitizers, "thread")
		}

		env := test.Env
		if test.UseLaunchEnv == "YES" { // "Use the Run action's arguments and environment variables"
			env = parsed.Launch.Env
		}

		// test.Opts are the AdditionalOptions: Guard Malloc and friends
		for _, v := range append(append([]envVar{}, env...), test.Opts...) {
			if v.Key == "DYLD_INSERT_LIBRARIES" && v.Enabled != "NO" {
				config.addInserts(v.Value)
			}
		}

		action.Configs = []TestConfig{config}
		return action, nil
	}

	var ref *planRef
	for i := range test.Plans {
		candidate := &test.Plans[i]
		switch {
		case plan != "":
			if planName(candidate.Ref) == plan {
				ref = candidate
			}
		case candidate.Default == "YES":
			ref = candidate
		}

		if ref != nil {
			break
		}
	}

	if ref == nil && plan != "" {
		return nil, fmt.Errorf("%s has no test plan %q", filepath.Base(schemePath), plan)
	}

	if ref == nil {
		ref = &test.Plans[0]
	}

	planPath := resolveRef(ref.Ref, containerDir(schemePath))
	action.Plan = planPath
	configs, err := readPlan(planPath, only, skip)
	if err != nil {
		return nil, err
	}

	action.Configs = configs
	return action, nil
}

func planName(ref string) string {
	return strings.TrimSuffix(filepath.Base(ref[strings.Index(ref, ":")+1:]), ".xctestplan")
}

// containerDir is the directory "container:" references in the scheme at schemePath resolve against: the one holding
// the .xcodeproj or .xcworkspace the scheme belongs to.
func containerDir(schemePath string) string {
	for dir := filepath.Dir(schemePath); dir != filepath.Dir(dir); dir = filepath.Dir(dir) {
		if strings.HasSuffix(dir, ".xcodeproj") || strings.HasSuffix(dir, ".xcworkspace") {
			return filepath.Dir(dir)
		}
	}
	return filepath.Dir(schemePath)
}

func resolveRef(ref, container string) string {
	kind, path, ok := strings.Cut(ref, ":")
	if !ok {
		path = ref
		kind = ""
	}
	if kind == "absolute" || filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(container, path)
}

// Locate finds the .xcscheme that xcodebuild would use for -scheme name, or returns "" if there is none.
//   - With -project, it looks in that project's shared schemes, then its user schemes.
//   - With -workspace, it looks in the workspace's own schemes, then in those of the projects that its
//     contents.xcworkspacedata lists.
//   - With neither, it looks in the .xcworkspace or .xcodeproj in wd, picked as xcodebuild picks it, and then in a
//     bounded walk under wd.
func Locate(wd, workspace, project, name string) string {
	inProj := func(proj string) string {
		for _, pat := range []string{"xcshareddata/xcschemes/" + name + ".xcscheme",
			"xcuserdata/*/xcschemes/" + name + ".xcscheme"} {
			if matches, _ := filepath.Glob(filepath.Join(proj, pat)); len(matches) > 0 {
				return matches[0]
			}
		}

		return ""
	}

	inWorkspace := func(workspacePath string) string {
		if found := inProj(workspacePath); found != "" {
			return found
		}

		data, err := os.ReadFile(filepath.Join(workspacePath, "contents.xcworkspacedata"))
		if err != nil {
			return ""
		}

		for _, match := range regexp.MustCompile(`location\s*=\s*"([^"]+)"`).FindAllStringSubmatch(string(data), -1) {
			kind, loc, ok := strings.Cut(match[1], ":")
			if !ok || !strings.HasSuffix(loc, ".xcodeproj") {
				continue
			}

			projPath := loc
			switch kind {
			case "absolute":
			case "group", "container", "self":
				projPath = filepath.Join(filepath.Dir(workspacePath), loc)
			default:
				continue
			}

			if found := inProj(projPath); found != "" {
				return found
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

		return FindScheme(filepath.Dir(workspace), name, schemeSearchDepth)
	}

	if matches, _ := filepath.Glob(filepath.Join(wd, "*.xcworkspace")); len(matches) == 1 {
		if p := inWorkspace(matches[0]); p != "" {
			return p
		}
	}

	if matches, _ := filepath.Glob(filepath.Join(wd, "*.xcodeproj")); len(matches) == 1 {
		if found := inProj(matches[0]); found != "" {
			return found
		}
	}

	return FindScheme(wd, name, schemeSearchDepth)
}

// readPlan returns one TestConfig per configuration of an .xctestplan (its options over defaultOptions, key by key).
func readPlan(planPath string, only, skip []string) ([]TestConfig, error) {
	data, err := os.ReadFile(planPath)
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

	if err := json.Unmarshal(data, &plan); err != nil {
		return nil, fmt.Errorf("%s: %w", planPath, err)
	}

	if len(plan.Configurations) == 0 {
		plan.Configurations = append(plan.Configurations, struct {
			Name    string                     `json:"name"`
			Options map[string]json.RawMessage `json:"options"`
		}{})
	}

	var out []TestConfig
	for _, planConfig := range plan.Configurations {
		if (len(only) > 0 && !slices.Contains(only, planConfig.Name)) || slices.Contains(skip, planConfig.Name) {
			continue
		}

		options := map[string]json.RawMessage{}
		for k, v := range plan.Defaults {
			options[k] = v
		}

		for k, v := range planConfig.Options {
			options[k] = mergeObject(options[k], v) // a configuration's {"addressSanitizer":{…}} overrides key by key
		}

		config := TestConfig{Name: planConfig.Name}
		if enabled(options["addressSanitizer"]) || isTrue(options["addressSanitizerEnabled"]) {
			config.Sanitizers = append(config.Sanitizers, "address")
		}

		if enabled(options["threadSanitizer"]) || isTrue(options["threadSanitizerEnabled"]) {
			config.Sanitizers = append(config.Sanitizers, "thread")
		}

		config.NoMTC = isFalse(options["mainThreadCheckerEnabled"])
		config.GuardMalloc = isTrue(options["guardMallocEnabled"])
		var envs []struct {
			Key     string `json:"key"`
			Value   string `json:"value"`
			Enabled *bool  `json:"enabled"`
		}

		if raw, ok := options["environmentVariableEntries"]; ok {
			if err := json.Unmarshal(raw, &envs); err != nil {
				return nil, fmt.Errorf("%s: environmentVariableEntries: %w", planPath, err)
			}
		}

		for _, e := range envs {
			if e.Key == "DYLD_INSERT_LIBRARIES" && (e.Enabled == nil || *e.Enabled) {
				config.addInserts(e.Value)
			}
		}

		out = append(out, config)
	}

	return out, nil
}

// mergeObject returns base with over's keys set over it when both are JSON objects, and over otherwise.
func mergeObject(base, over json.RawMessage) json.RawMessage {
	var merged, overrides map[string]json.RawMessage
	if base == nil || json.Unmarshal(base, &merged) != nil || json.Unmarshal(over, &overrides) != nil {
		return over
	}

	for k, v := range overrides {
		merged[k] = v
	}

	encoded, err := json.Marshal(merged)
	if err != nil {
		return over
	}

	return encoded
}

func isTrue(raw json.RawMessage) bool {
	var b bool
	return json.Unmarshal(raw, &b) == nil && b
}

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
	for _, entry := range strings.Split(list, ":") {
		entry = strings.TrimSpace(entry)
		name := filepath.Base(entry)
		switch {
		case entry == "":
		case strings.HasPrefix(name, "libclang_rt.asan_"):
			c.addSanitizer("address")
		case strings.HasPrefix(name, "libclang_rt.tsan_"):
			c.addSanitizer("thread")
		case name == "libgmalloc.dylib":
			c.GuardMalloc = true
		case !slices.Contains(c.Inserts, entry):
			c.Inserts = append(c.Inserts, entry)
		}
	}
}

func (c *TestConfig) addSanitizer(s string) {
	if !slices.Contains(c.Sanitizers, s) {
		c.Sanitizers = append(c.Sanitizers, s)
		sort.Strings(c.Sanitizers)
	}
}

// XCTestRun reads an .xctestrun file, in format 1 or 2, as an XML or binary plist.
// It returns one TestConfig per test target, for the test configurations that only and skip select (format 2) and the
// targets that onlyTargets names (all when it is empty).
// Each TestConfig comes from the runner's own insert list: the target's DYLD_INSERT_LIBRARIES in
// TestingEnvironmentVariables or EnvironmentVariables.
func XCTestRun(path string, only, skip, onlyTargets []string) ([]TestConfig, error) {
	root, err := ReadPlist(path)
	if err != nil {
		return nil, err
	}

	top, ok := root.(map[string]any)
	if !ok {
		return nil, fmt.Errorf("%s: not a dictionary", path)
	}

	type runTarget struct {
		config, name string
		settings     map[string]any
	}

	var runTargets []runTarget
	if configs, ok := top["TestConfigurations"].([]any); ok { // format 2
		for _, entry := range configs {
			configDict, _ := entry.(map[string]any)
			configName, _ := configDict["Name"].(string)
			if (len(only) > 0 && !slices.Contains(only, configName)) || slices.Contains(skip, configName) {
				continue
			}

			targets, _ := configDict["TestTargets"].([]any)
			for _, target := range targets {
				targetDict, _ := target.(map[string]any)
				targetName, _ := targetDict["BlueprintName"].(string)
				runTargets = append(runTargets, runTarget{configName, targetName, targetDict})
			}
		}
	} else { // format 1: a dictionary per target
		var names []string
		for key := range top {
			if !strings.HasPrefix(key, "__") {
				names = append(names, key)
			}
		}

		sort.Strings(names)
		for _, key := range names {
			if settings, ok := top[key].(map[string]any); ok {
				runTargets = append(runTargets, runTarget{"", key, settings})
			}
		}
	}

	var out []TestConfig
	for _, target := range runTargets {
		if len(onlyTargets) > 0 && !slices.Contains(onlyTargets, target.name) {
			continue
		}

		// an .xctestrun doesn't say whether the Main Thread Checker is on: xcodebuild adds it
		testConfig := TestConfig{Name: strings.TrimPrefix(target.config+"/"+target.name, "/")}
		for _, key := range []string{"TestingEnvironmentVariables", "EnvironmentVariables"} {
			env, _ := target.settings[key].(map[string]any)
			if inserts, _ := env["DYLD_INSERT_LIBRARIES"].(string); inserts != "" {
				testConfig.addInserts(inserts)
			}
		}

		// what Xcode itself inserts into a runner (not "other" libraries the user's scheme added)
		var other []string
		for _, insert := range testConfig.Inserts {
			switch filepath.Base(insert) {
			case "libXCTestBundleInject.dylib", "libMainThreadChecker.dylib", "libRPAC.dylib",
				"libXCTestSwiftSupport.dylib", "libViewDebuggerSupport.dylib", "libLogRedirect.dylib",
				"libBacktraceRecording.dylib":
			default:
				other = append(other, insert)
			}
		}

		testConfig.Inserts = other
		out = append(out, testConfig)
	}

	return out, nil
}

// ReadPlist decodes an XML plist (a binary one through plutil, macOS) into map[string]any, []any, string, bool, int64,
// float64, []byte.
func ReadPlist(path string) (any, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	if bytes.HasPrefix(data, []byte("bplist")) {
		if data, err = exec.Command("plutil", "-convert", "xml1", "-o", "-", path).Output(); err != nil {
			return nil, fmt.Errorf("%s: plutil: %w", path, err)
		}
	}

	decoder := xml.NewDecoder(bytes.NewReader(data))
	decoder.Strict = false
	for {
		tok, err := decoder.Token()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}

		if start, ok := tok.(xml.StartElement); ok {
			if start.Name.Local == "plist" {
				continue
			}

			return plistValue(decoder, start)
		}
	}
}

func plistValue(decoder *xml.Decoder, start xml.StartElement) (any, error) {
	switch start.Name.Local {
	case "dict":
		dict := map[string]any{}
		key := ""
		for {
			tok, err := decoder.Token()
			if err != nil {
				return nil, err
			}

			switch elem := tok.(type) {
			case xml.EndElement:
				return dict, nil
			case xml.StartElement:
				if elem.Name.Local == "key" {
					var name string
					if err := decoder.DecodeElement(&name, &elem); err != nil {
						return nil, err
					}

					key = name
					continue
				}

				value, err := plistValue(decoder, elem)
				if err != nil {
					return nil, err
				}

				dict[key] = value
			}
		}
	case "array":
		var array []any
		for {
			tok, err := decoder.Token()
			if err != nil {
				return nil, err
			}

			switch elem := tok.(type) {
			case xml.EndElement:
				return array, nil
			case xml.StartElement:
				value, err := plistValue(decoder, elem)
				if err != nil {
					return nil, err
				}

				array = append(array, value)
			}
		}
	case "true", "false":
		if err := decoder.Skip(); err != nil {
			return nil, err
		}

		return start.Name.Local == "true", nil
	default: // string, integer, real, date, data: their text
		var text string
		if err := decoder.DecodeElement(&text, &start); err != nil {
			return nil, err
		}

		var integer int64
		var float float64
		switch start.Name.Local {
		case "integer":
			if _, err := fmt.Sscan(text, &integer); err == nil {
				return integer, nil
			}
		case "real":
			if _, err := fmt.Sscan(text, &float); err == nil {
				return float, nil
			}
		}

		return text, nil
	}
}
