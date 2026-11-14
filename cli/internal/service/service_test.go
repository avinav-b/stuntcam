// SPDX-License-Identifier: Apache-2.0
package service

import (
	"encoding/xml"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

func TestToArmOncePerBoot(t *testing.T) {
	t0 := time.Unix(1_800_000_000, 0)
	ds := []simctl.Device{
		{UDID: "A", State: "Booted", RuntimeID: "iOS-26-3", LastBooted: t0},
		{UDID: "B", State: "Booted", RuntimeID: "iOS-27-0", LastBooted: t0},
		{UDID: "C", State: "Shutdown", RuntimeID: "iOS-27-0"},
		{UDID: "W", State: "Booted", RuntimeID: "watchOS-12-0", LastBooted: t0},
	}
	armed := func(d simctl.Device) State {
		if d.UDID == "B" {
			return IsArmed
		}
		return Unarmed
	}
	handled := map[string]time.Time{}
	got := ToArm(ds, armed, handled)
	if len(got) != 1 || got[0].UDID != "A" {
		t.Fatalf("%v", got)
	}
	handled["A"] = t0
	if got := ToArm(ds, armed, handled); len(got) != 0 {
		t.Fatalf("same boot armed twice: %v", got)
	}
	ds[0].LastBooted = t0.Add(time.Minute) // rebooted by Xcode
	if got := ToArm(ds, armed, handled); len(got) != 1 {
		t.Fatalf("new boot missed: %v", got)
	}
}

func TestPlist(t *testing.T) {
	if got := ToArm([]simctl.Device{{UDID: "X", State: "Booted", RuntimeID: "iOS-27-0"}}, func(simctl.Device) State { return Unarmed }, map[string]time.Time{}, "ONLY"); len(got) != 0 {
		t.Fatal("--only ignored")
	}
	p := Plist("/opt/stuntcam/bin/stuntcam", map[string]string{"STUNTCAM_BUILD": "/x & y", "SIMCAM_HOME": "/h"}, "U1", []string{"A"})
	for _, want := range []string{"<string>service</string><string>run</string><string>--only</string><string>A</string><string>--preboot</string><string>U1</string>", "/x &amp; y", Label,
		"<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>", "<key>ThrottleInterval</key><integer>10</integer>",
		"<key>SIMCAM_HOME</key><string>/h</string><key>STUNTCAM_BUILD</key>"} { // env keys sorted
		if !strings.Contains(p, want) {
			t.Errorf("plist lacks %s", want)
		}
	}
	// well-formed XML (launchd rejects anything else)
	d := xml.NewDecoder(strings.NewReader(p))
	d.Strict = true
	for {
		if _, err := d.Token(); err == io.EOF {
			break
		} else if err != nil {
			t.Fatalf("plist is not XML: %v", err)
		}
	}
	if !strings.Contains(Plist("/b", nil, "", nil), "<string>--all</string>") {
		t.Error("no --only must mean an explicit --all")
	}
	// InstalledEnv reads back what Plist wrote
	t.Setenv("HOME", t.TempDir())
	os.MkdirAll(filepath.Dir(PlistPath()), 0o755)
	os.WriteFile(PlistPath(), []byte(p), 0o644)
	if env := InstalledEnv(); env["STUNTCAM_BUILD"] != "/x & y" || env["SIMCAM_HOME"] != "/h" {
		t.Errorf("InstalledEnv: %v", env)
	}
	// a pending sim isn't marked handled: it is looked at again
	h := map[string]time.Time{}
	pending := func(simctl.Device) State { return Pending }
	ds2 := []simctl.Device{{UDID: "P", State: "Booted", RuntimeID: "iOS-27-0"}}
	if got := ToArm(ds2, pending, h); len(got) != 0 {
		t.Fatal("pending sim armed")
	}
	if _, ok := h["P"]; ok {
		t.Fatal("pending boot marked handled")
	}
	if got := ToArm(ds2, func(simctl.Device) State { return Unarmed }, h); len(got) != 1 {
		t.Fatal("a sim whose arming never finished isn't armed by the service")
	}
}
