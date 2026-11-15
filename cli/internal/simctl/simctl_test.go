// SPDX-License-Identifier: Apache-2.0
package simctl

import (
	"os"
	"path/filepath"
	"testing"
)

func TestParseDevicesAndRuntimes(t *testing.T) {
	ds, err := ParseDevices([]byte(`{"devices":{"com.apple.CoreSimulator.SimRuntime.iOS-26-3":[{"udid":"A","name":"iPhone 17 Pro","state":"Booted","isAvailable":true,"lastBootedAt":"2026-11-12T06:00:00Z"}],"com.apple.CoreSimulator.SimRuntime.iOS-27-0":[{"udid":"B","name":"iPhone 17","state":"Shutdown","isAvailable":true}]}}`))
	if err != nil || len(ds) != 2 || ds[0].Name != "iPhone 17" || ds[1].State != "Booted" || ds[1].LastBooted.IsZero() || !ds[0].LastBooted.IsZero() {
		t.Fatalf("%+v %v", ds, err)
	}
	// Xcode 27's simctl has no lastBootedAt, only lastUsedAt (the boot time); status's reboot check needs it
	ds, err = ParseDevices([]byte(`{"devices":{"com.apple.CoreSimulator.SimRuntime.iOS-26-3":[{"udid":"C","name":"x","state":"Booted","isAvailable":true,"lastUsedAt":"2026-11-12T12:51:12Z"}]}}`))
	if err != nil || len(ds) != 1 || ds[0].LastBooted.Unix() != 1794487872 {
		t.Fatalf("lastUsedAt fallback: %+v %v", ds, err)
	}
	rs, err := ParseRuntimes([]byte(`{"runtimes":[
	 {"identifier":"com.apple.CoreSimulator.SimRuntime.iOS-17-5","version":"17.5","buildversion":"21F79","platform":"iOS","isAvailable":true},
	 {"identifier":"com.apple.CoreSimulator.SimRuntime.iOS-26-3","version":"26.3.1","buildversion":"23D8133","platform":"iOS","isAvailable":true},
	 {"identifier":"com.apple.CoreSimulator.SimRuntime.iOS-27-0","version":"27.0","buildversion":"24A434","platform":"iOS","isAvailable":true},
	 {"identifier":"com.apple.CoreSimulator.SimRuntime.watchOS-12-0","version":"12.0","buildversion":"x","platform":"watchOS","isAvailable":true}]}`))
	if err != nil {
		t.Fatal(err)
	}
	if r, ok := NewestIOS(rs, ""); !ok || r.Build != "24A434" {
		t.Errorf("newest: %+v", r)
	}
	if r, ok := NewestIOS(rs, "26.3"); !ok || r.Build != "23D8133" {
		t.Errorf("26.3 prefix: %+v", r)
	}
	if _, ok := NewestIOS(rs, "99"); ok {
		t.Error("99 matched") // verify.sh --runtime 99 must not fall through to 27.0
	}
	if !versionLess("26.3.1", "27.0") || versionLess("27.0", "26.3.1") {
		t.Error("versionLess")
	}
	if !versionLess("26.9", "26.10") {
		t.Error("26.9 < 26.10")
	}
}

// get_app_container through a fake xcrun with the texts simctl 27.0 prints: data path, not installed, not booted.
func TestAppContainer(t *testing.T) {
	dir := t.TempDir()
	script := "#!/bin/sh\ncase \"$*\" in\n" +
		"'simctl get_app_container U dev.x data') echo /d/U/data/Containers/Data/Application/A1;;\n" +
		"'simctl get_app_container U dev.gone data') printf 'An error was encountered processing the command (domain=NSPOSIXErrorDomain, code=2):\\nThe operation couldn\\342\\200\\231t be completed. No such file or directory\\nNo such file or directory\\n' >&2; exit 2;;\n" +
		"'simctl get_app_container S dev.x data') printf 'An error was encountered processing the command (domain=com.apple.CoreSimulator.SimError, code=405):\\nUnable to lookup in current state: Shutdown\\n' >&2; exit 149;;\n" +
		"*) exit 3;;\nesac\n"
	if err := os.WriteFile(filepath.Join(dir, "xcrun"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	if p, err := (Ctl{}).AppContainer("U", "dev.x", "data"); err != nil || p != "/d/U/data/Containers/Data/Application/A1" {
		t.Fatal(p, err)
	}
	_, err := (Ctl{}).AppContainer("U", "dev.gone", "data")
	if !NotInstalled(err) || NotBooted(err) {
		t.Fatalf("not installed: %v", err)
	}
	_, err = (Ctl{}).AppContainer("S", "dev.x", "data")
	if !NotBooted(err) || NotInstalled(err) {
		t.Fatalf("not booted: %v", err)
	}
	if NotInstalled(nil) || NotBooted(nil) {
		t.Fatal("nil error")
	}
}
