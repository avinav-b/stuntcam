// SPDX-License-Identifier: Apache-2.0
package xcode

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestParseVersion(t *testing.T) {
	v, b, err := ParseVersion("Xcode 27.0\nBuild version 27A266a\n")
	if err != nil || v != "27.0" || b != "27A266a" {
		t.Fatalf("got %q %q %v", v, b, err)
	}
	if _, _, err := ParseVersion("xcode-select: error: tool 'xcodebuild' requires Xcode\n"); err == nil {
		t.Fatal("CLT error accepted")
	}
	if v, b, err := ParseVersion("Xcode 26.6\r\nBuild version 17F113\r\n"); err != nil || v != "26.6" || b != "17F113" {
		t.Fatalf("CRLF: %q %q %v", v, b, err)
	}
}

func TestCompareBuilds(t *testing.T) {
	order := []string{"17F113", "26E5", "27A266a", "27A266b", "27A9275", "27B52", "100A1"}
	for i := range order {
		for j := range order {
			want := cmp(i, j)
			if got := CompareBuilds(order[i], order[j]); got != want {
				t.Errorf("CompareBuilds(%s, %s) = %d, want %d", order[i], order[j], got, want)
			}
		}
	}
}

func TestRules(t *testing.T) {
	d := t.TempDir()
	if f, v := Rules(d, "27A266a"); f != "" || v {
		t.Fatalf("empty dir: %q %v", f, v)
	}
	for _, n := range []string{"27A266a.json", "26E5.json", "README.md", "27A9275.json"} {
		os.WriteFile(filepath.Join(d, n), []byte(`{"proto": 1, "rules": []}`), 0o644)
	}
	if f, v := Rules(d, "26E5"); filepath.Base(f) != "26E5.json" || !v {
		t.Fatalf("known: %q %v", f, v)
	}
	if f, v := Rules(d, "27Z1"); filepath.Base(f) != "27A9275.json" || v {
		t.Fatalf("unknown -> newest: %q %v", f, v)
	}
	if f, v := Rules(d, ""); filepath.Base(f) != "27A9275.json" || v {
		t.Fatalf("not recorded -> newest: %q %v", f, v)
	}
	// the loader's notion of a usable file (w4 e30): no "rules", or not JSON, is neither verified nor a best guess
	os.WriteFile(filepath.Join(d, "26E5.json"), []byte(`{"proto": 1}`), 0o644)
	if f, v := Rules(d, "26E5"); filepath.Base(f) != "27A9275.json" || v {
		t.Fatalf("no rules key: %q %v", f, v)
	}
	os.WriteFile(filepath.Join(d, "27A9275.json"), []byte(`{"rules": [`), 0o644)
	if f, v := Rules(d, "27Z1"); f != "" || v {
		t.Fatalf("newest unusable -> built-in: %q %v", f, v)
	}
	big := make([]byte, MaxRulesSize+1)
	copy(big, `{"rules": []}`)
	for i := len(`{"rules": []}`); i < len(big); i++ {
		big[i] = ' '
	}
	os.WriteFile(filepath.Join(d, "27A266a.json"), big, 0o644)
	if Usable(filepath.Join(d, "27A266a.json")) {
		t.Fatal("a file the loader would truncate counts as usable")
	}
}

func TestNormalizeDeveloperDir(t *testing.T) {
	for in, want := range map[string]string{
		"/Applications/Xcode.app":                     "/Applications/Xcode.app/Contents/Developer",
		"/Applications/Xcode.app/":                    "/Applications/Xcode.app/Contents/Developer",
		"/Applications/Xcode.app/Contents/Developer/": "/Applications/Xcode.app/Contents/Developer",
		"/Library/Developer/CommandLineTools":         "/Library/Developer/CommandLineTools",
		"":                                            "",
	} {
		if got := NormalizeDeveloperDir(in); got != want {
			t.Errorf("%q -> %q, want %q", in, got, want)
		}
	}
}

func TestRunTimeout(t *testing.T) {
	old := Timeout
	defer func() { Timeout = old }()
	Timeout = 200 * time.Millisecond
	start := time.Now()
	_, err := run(nil, "sleep", "5")
	if err == nil || !strings.Contains(err.Error(), "timed out") || time.Since(start) > 3*time.Second {
		t.Fatalf("%v after %s", err, time.Since(start))
	}
}

// a grandchild holding stdout open must not outlast the timeout (review r2 L-a)
func TestRunTimeoutGrandchild(t *testing.T) {
	old := Timeout
	defer func() { Timeout = old }()
	Timeout = 200 * time.Millisecond
	start := time.Now()
	_, err := run(nil, "sh", "-c", "sleep 6; echo x")
	if err == nil || time.Since(start) > 4*time.Second {
		t.Fatalf("%v after %s", err, time.Since(start))
	}
}

func TestParseMdfind(t *testing.T) {
	out := "/Applications/Xcode.app\n/Users/x/stuntcam-work/xcodes/Xcode-27.1.app\n/Users/x/.Trash/Xcode.app\n" +
		"/Applications/Xcode.app/Contents/Applications/Instruments.app\n\n/Volumes/X/Xcode-beta.app\n"
	want := []string{"/Applications/Xcode.app", "/Users/x/stuntcam-work/xcodes/Xcode-27.1.app", "/Volumes/X/Xcode-beta.app"}
	if got := ParseMdfind(out); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %q", got)
	}
}

func TestActiveFollowsDeveloperDir(t *testing.T) {
	old := run
	defer func() { run = old }()
	run = func(env []string, name string, args ...string) (string, error) {
		switch name {
		case "xcodebuild":
			return "Xcode 27.1\nBuild version 27A9275\n", nil
		case "xcode-select":
			for _, kv := range env {
				if len(kv) > 14 && kv[:14] == "DEVELOPER_DIR=" {
					t.Error("xcode-select got DEVELOPER_DIR")
				}
			}
			return "/Applications/Xcode.app/Contents/Developer\n", nil
		}
		return "", nil
	}
	t.Setenv("DEVELOPER_DIR", "/x/Xcode-27.1.app/Contents/Developer/")
	i, err := Active()
	if err != nil || i.Build != "27A9275" || i.Version != "27.1" || i.DeveloperDir != "/x/Xcode-27.1.app/Contents/Developer" {
		t.Fatalf("%+v %v", i, err)
	}
	t.Setenv("DEVELOPER_DIR", "/x/Xcode-27.1.app/") // the .app itself (xcrun accepts it): same Xcode
	if i, _ := Active(); i.DeveloperDir != "/x/Xcode-27.1.app/Contents/Developer" {
		t.Fatalf(".app DEVELOPER_DIR: %+v", i)
	}
	if s, _ := Selected(); s != "/Applications/Xcode.app/Contents/Developer" {
		t.Fatalf("selected %q", s)
	}
	os.Unsetenv("DEVELOPER_DIR")
	i, _ = Active()
	if i.DeveloperDir != "/Applications/Xcode.app/Contents/Developer" {
		t.Fatalf("without DEVELOPER_DIR: %+v", i)
	}
	if App(i.DeveloperDir) != "/Applications/Xcode.app" || App("/Library/Developer/CommandLineTools") != "" {
		t.Fatal("App")
	}
}

func TestUnverifiedLine(t *testing.T) {
	if got := UnverifiedLine("27Z1"); got != "restore rules unverified for Xcode 27Z1; if console/debugger misbehave use --mode launchd" {
		t.Fatal(got)
	}
}
