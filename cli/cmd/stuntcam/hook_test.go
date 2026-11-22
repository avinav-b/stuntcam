// SPDX-License-Identifier: Apache-2.0
package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestHookMode(t *testing.T) {
	tests := []struct {
		status statusMode
		want   runnerHookMode
	}{
		{modeBoot, hookBoot},
		{modeLaunchd, hookLaunchd},
		{modeBootedUnarmed, hookUnarmed},
		{modeEnv, hookUnarmed},
		{modeScriptArmed, hookUnarmed},
		{modeBooting, hookNone},
		{modeUnarmed, hookNone},
	}
	for _, tt := range tests {
		t.Run(string(tt.status), func(t *testing.T) {
			if got := hookMode(tt.status); got != tt.want {
				t.Errorf("hookMode(%q) = %q, want %q", tt.status, got, tt.want)
			}
		})
	}
}

func TestHookEnvText(t *testing.T) {
	got := hookEnvText("unarmed", []string{"DYLD_INSERT_LIBRARIES=/a b/libsimcamstub.dylib", "SIMCAM_SIM_DIR=/x", "BAD=a\nb"})
	want := "mode=unarmed\nDYLD_INSERT_LIBRARIES=/a b/libsimcamstub.dylib\nSIMCAM_SIM_DIR=/x\n"
	if got != want {
		t.Fatalf("%q", got)
	}
	d := t.TempDir()
	if err := writeHookEnv(filepath.Join(d, "hook"), "U1", "boot", nil); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(d, "hook", "U1.env")); string(b) != "mode=boot\n" {
		t.Fatalf("%q", b)
	}
}

func TestRunnerHookEnv(t *testing.T) {
	got := runnerHookEnv("/t/libown.dylib:/old/libsimcamrunnerhook.dylib::/t/libown.dylib", "/h/libsimcamrunnerhook.dylib", "/run/hook", "/s/libsimcamstub.dylib", "", nil, nil)
	want := []string{
		"TEST_RUNNER_DYLD_INSERT_LIBRARIES=/t/libown.dylib:/h/libsimcamrunnerhook.dylib",
		"TEST_RUNNER_SIMCAM_HOOK_DIR=/run/hook",
		"TEST_RUNNER_SIMCAM_HOOK_STUB=/s/libsimcamstub.dylib",
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("%q", got)
	}
	if got := runnerHookEnv("", "/h", "/d", "/s", "/r/27A266a.json", nil, nil); len(got) != 4 || got[0] != "TEST_RUNNER_DYLD_INSERT_LIBRARIES=/h" || got[3] != "TEST_RUNNER_SIMCAM_HOOK_RULES=/r/27A266a.json" {
		t.Fatalf("%q", got)
	}
}

func TestStubIn(t *testing.T) {
	if s := stubIn("/a/libx.dylib:/b/libsimcamstub.dylib", "libsimcamstub.dylib"); s != "/b/libsimcamstub.dylib" {
		t.Fatal(s)
	}
	if s := stubIn("", "libsimcamstub.dylib"); s != "" {
		t.Fatal(s)
	}
}

func TestHookReport(t *testing.T) {
	d := t.TempDir()
	os.WriteFile(filepath.Join(d, "U1.log"), []byte(
		"1.0 [hook 1 R] loaded in /x\n"+
			"1.1 [hook 1 R] armed dev.a via _combinedLaunchEnvironment: DYLD_INSERT_LIBRARIES=/s (no other list)\n"+
			"1.2 [hook 1 R] armed dev.a via _combinedLaunchEnvironment: DYLD_INSERT_LIBRARIES=/s (no other list)\n"+
			"1.3 [hook 1 R] failed (launch left as it was): boom\n"), 0o644)
	os.WriteFile(filepath.Join(d, "U2.log"), []byte("1.0 [hook 2 M] loaded in /y\n"), 0o644)
	os.WriteFile(filepath.Join(d, "U2.env"), []byte("mode=boot\n"), 0o644)
	r := hookReport(d)
	if r["U1"].Armed != 2 || len(r["U1"].Failures) != 1 || r["U2"].Armed != 0 || len(r) != 2 {
		t.Fatalf("%+v", r)
	}
	if k := sortedKeys(r); !reflect.DeepEqual(k, []string{"U1", "U2"}) {
		t.Fatal(k)
	}
}

func TestSanitizerRuntime(t *testing.T) {
	d := t.TempDir()
	lib := filepath.Join(d, "Toolchains", "XcodeDefault.xctoolchain", "usr", "lib", "clang", "21", "lib", "darwin")
	os.MkdirAll(lib, 0o755)
	os.WriteFile(filepath.Join(lib, "libclang_rt.asan_iossim_dynamic.dylib"), []byte("x"), 0o644)
	os.Symlink("21", filepath.Join(d, "Toolchains", "XcodeDefault.xctoolchain", "usr", "lib", "clang", "21.0.0"))
	if got := sanitizerRuntime(d, "address"); got == "" || filepath.Base(got) != "libclang_rt.asan_iossim_dynamic.dylib" {
		t.Fatalf("%q", got)
	}
	if got := sanitizerRuntime(d, "thread"); got != "" {
		t.Fatalf("%q", got)
	}
	if got := sanitizerRuntime("", "address"); got != "" {
		t.Fatalf("%q", got)
	}
}
