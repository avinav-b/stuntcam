// SPDX-License-Identifier: Apache-2.0
package scheme

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

const withInsert = `<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="2700" version="1.7">
   <TestAction buildConfiguration="Debug"><EnvironmentVariables>
      <EnvironmentVariable key="DYLD_INSERT_LIBRARIES" value="/opt/reveal/libReveal.dylib" isEnabled="YES"/>
   </EnvironmentVariables></TestAction>
   <LaunchAction buildConfiguration="Debug"><EnvironmentVariables>
      <EnvironmentVariable key="DYLD_INSERT_LIBRARIES" value="/opt/reveal/libReveal.dylib:$(BUILT_PRODUCTS_DIR)/libDebugTools.dylib" isEnabled="YES"/>
      <EnvironmentVariable key="DYLD_INSERT_LIBRARIES" value="/off.dylib" isEnabled="NO"/>
      <EnvironmentVariable key="OTHER" value="x" isEnabled="YES"/>
   </EnvironmentVariables></LaunchAction>
</Scheme>`

func TestScan(t *testing.T) {
	d := t.TempDir()
	w := func(p, s string) { os.MkdirAll(filepath.Dir(p), 0o755); os.WriteFile(p, []byte(s), 0o644) }
	w(filepath.Join(d, "App.xcodeproj", "xcshareddata", "xcschemes", "App.xcscheme"), withInsert)
	w(filepath.Join(d, "App.xcodeproj", "xcshareddata", "xcschemes", "Plain.xcscheme"), `<Scheme><LaunchAction/></Scheme>`)
	w(filepath.Join(d, "node_modules", "x", "X.xcodeproj", "xcshareddata", "xcschemes", "X.xcscheme"), withInsert) // skipped
	got := Scan(d, 6)
	if len(got) != 1 || len(got[0].Libs) != 2 || got[0].Libs[0] != "/opt/reveal/libReveal.dylib" {
		t.Fatalf("%+v", got)
	}
}

func TestReadTestAction(t *testing.T) {
	d := t.TempDir()
	// a workspace's scheme: container: resolves against the workspace's dir
	dir := filepath.Join(d, "App.xcworkspace", "xcshareddata", "xcschemes")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "P.xcscheme"), []byte(`<?xml version="1.0"?><Scheme><TestAction><TestPlans><TestPlanReference reference = "container:Tests/P.xctestplan" default = "YES"></TestPlanReference></TestPlans></TestAction></Scheme>`), 0o644)
	os.MkdirAll(filepath.Join(d, "Tests"), 0o755)
	os.WriteFile(filepath.Join(d, "Tests", "P.xctestplan"), []byte(`{"configurations":[{"name":"A","options":{"threadSanitizer":{"enabled":true}}},{"name":"B","options":{"addressSanitizer":{"enabled":false}}}],
		"defaultOptions":{"addressSanitizer":{"enabled":true},"guardMallocEnabled":true,"mainThreadCheckerEnabled":false,
		"environmentVariableEntries":[{"key":"DYLD_INSERT_LIBRARIES","value":"/a/libx.dylib","enabled":false},{"key":"DYLD_INSERT_LIBRARIES","value":"/b/liby.dylib"}]}}`), 0o644)
	p := FindScheme(d, "P", 4)
	if p == "" {
		t.Fatal("not found")
	}
	ta, err := ReadTestAction(p, "", nil, nil)
	if err != nil || ta.Plan != filepath.Join(d, "Tests", "P.xctestplan") || len(ta.Configs) != 2 {
		t.Fatalf("%+v %v", ta, err)
	}
	a, b := ta.Configs[0], ta.Configs[1]
	if strings.Join(a.Sanitizers, ",") != "address,thread" || b.Sanitizers != nil || !a.GuardMalloc || !a.NoMTC || strings.Join(a.Inserts, ",") != "/b/liby.dylib" {
		t.Fatalf("%+v %+v", a, b)
	}
	if ta, err := ReadTestAction(p, "", []string{"B"}, nil); err != nil || len(ta.Configs) != 1 || ta.Configs[0].Name != "B" {
		t.Fatalf("%+v %v", ta, err)
	}
	if _, err := ReadTestAction(p, "Other", nil, nil); err == nil {
		t.Fatal("unknown plan accepted")
	}
	if FindScheme(d, "Missing", 4) != "" {
		t.Fatal("found a missing scheme")
	}
}

func TestReadPlistBinary(t *testing.T) {
	if _, err := exec.LookPath("plutil"); err != nil {
		t.Skip("no plutil")
	}
	d := t.TempDir()
	x := filepath.Join(d, "a.plist")
	os.WriteFile(x, []byte(`<?xml version="1.0"?><plist version="1.0"><dict><key>A</key><array><string>s</string><integer>3</integer><true/></array><key>B</key><dict/></dict></plist>`), 0o644)
	if err := exec.Command("plutil", "-convert", "binary1", x).Run(); err != nil {
		t.Skip(err)
	}
	v, err := ReadPlist(x)
	m, _ := v.(map[string]any)
	if err != nil || fmt.Sprint(m["A"]) != "[s 3 true]" || m["B"] == nil {
		t.Fatalf("%#v %v", v, err)
	}
}

// where xcodebuild looks for -scheme
func TestLocate(t *testing.T) {
	d := t.TempDir()
	mk := func(p string) string {
		p = filepath.Join(d, p)
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("<Scheme/>"), 0o644)
		return p
	}
	deep := mk("ios/Modules/Feature/Foo.xcodeproj/xcshareddata/xcschemes/Foo.xcscheme")
	os.MkdirAll(filepath.Join(d, "ios/App.xcworkspace"), 0o755)
	os.WriteFile(filepath.Join(d, "ios/App.xcworkspace/contents.xcworkspacedata"),
		[]byte(`<Workspace version="1.0"><FileRef location="group:Modules/Feature/Foo.xcodeproj"></FileRef></Workspace>`), 0o644)
	if got := Locate(d, filepath.Join(d, "ios/App.xcworkspace"), "", "Foo"); got != deep {
		t.Fatalf("workspace-listed project: %q", got)
	}
	// a library repo: the project in the current folder wins over Example/
	ex := mk("Example/Lib.xcodeproj/xcshareddata/xcschemes/Lib.xcscheme")
	top := mk("Lib.xcodeproj/xcshareddata/xcschemes/Lib.xcscheme")
	if got := Locate(d, "", "", "Lib"); got != top || got == ex {
		t.Fatalf("library repo: %q", got)
	}
	user := mk("P.xcodeproj/xcuserdata/me.xcuserdatad/xcschemes/U.xcscheme")
	if got := Locate(d, "", filepath.Join(d, "P.xcodeproj"), "U"); got != user {
		t.Fatalf("user scheme: %q", got)
	}
	if got := Locate(d, "", filepath.Join(d, "P.xcodeproj"), "Lib"); got != "" {
		t.Fatalf("-project limits the search: %q", got)
	}
}

// a configuration's nested sanitizer options merge over the defaults'
func TestPlanNestedOverride(t *testing.T) {
	pf := filepath.Join(t.TempDir(), "p.xctestplan")
	os.WriteFile(pf, []byte(`{"configurations":[{"name":"C","options":{"addressSanitizer":{"detectStackUseAfterReturn":true}}}],
		"defaultOptions":{"addressSanitizer":{"enabled":true}}}`), 0o644)
	cs, err := readPlan(pf, nil, nil)
	if err != nil || len(cs) != 1 || len(cs[0].Sanitizers) != 1 || cs[0].Sanitizers[0] != "address" {
		t.Fatalf("%+v %v", cs, err)
	}
}
