// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func writeApp(t *testing.T, dir, exe string) string {
	t.Helper()
	app := filepath.Join(dir, "m10canary.app")
	os.MkdirAll(filepath.Join(app, "_CodeSignature"), 0o755)
	os.WriteFile(filepath.Join(app, "m10canary"), []byte(exe), 0o755)
	os.WriteFile(filepath.Join(app, "Info.plist"), []byte("<dict><key>CFBundleIdentifier</key><string>"+CanaryBundle+"</string></dict>"), 0o644)
	os.WriteFile(filepath.Join(app, "_CodeSignature", "CodeResources"), []byte("sig"), 0o644)
	return app
}

func TestBundleHash(t *testing.T) {
	a := writeApp(t, t.TempDir(), "v1")
	b := writeApp(t, t.TempDir(), "v1")
	ha, _ := BundleHash(a)
	hb, _ := BundleHash(b)
	if ha == "" || ha != hb {
		t.Fatalf("same content, different hash: %s %s", ha, hb)
	}
	os.WriteFile(filepath.Join(b, "m10canary"), []byte("v2"), 0o755)
	if h, _ := BundleHash(b); h == ha {
		t.Fatal("changed executable, same hash")
	}
	c := writeApp(t, t.TempDir(), "v1")
	os.WriteFile(filepath.Join(c, "extra.txt"), nil, 0o644)
	if h, _ := BundleHash(c); h == ha {
		t.Fatal("added file, same hash")
	}
	d := writeApp(t, t.TempDir(), "v1")
	os.Rename(filepath.Join(d, "Info.plist"), filepath.Join(d, "Info2.plist"))
	if h, _ := BundleHash(d); h == ha {
		t.Fatal("renamed file, same hash")
	}
	if _, err := BundleHash(filepath.Join(t.TempDir(), "missing.app")); err == nil {
		t.Fatal("missing bundle hashed")
	}
}

// a fake private device dir with the canary installed in a container
func fakeDevice(t *testing.T, exe string) (devDir, inst string) {
	devDir = t.TempDir()
	c := filepath.Join(devDir, "data", "Containers", "Bundle", "Application", "5EED0006-0000-4000-8000-00000000B006")
	os.MkdirAll(c, 0o755)
	return devDir, writeApp(t, c, exe)
}

func TestPlanInstall(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	const udid = "5EED0001-0000-4000-8000-00000000B001"
	src := writeApp(t, t.TempDir(), "build-1")
	want, _ := BundleHash(src)

	// nothing installed
	if p := planInstall(t.TempDir(), udid, src); !p.Install || !p.Grant || !strings.Contains(p.Why, "not installed") {
		t.Fatalf("empty device: %+v", p)
	}
	dev, inst := fakeDevice(t, "build-1")
	// installed (e.g. by scripts/verify.sh) but no record of ours: install
	if p := planInstall(dev, udid, src); !p.Install || !strings.Contains(p.Why, "no install record") {
		t.Fatalf("no record: %+v", p)
	}
	// recorded and identical: no install; the grant is skipped only if the record and TCC both say granted
	if err := recordInstall(dev, udid, want, true); err != nil {
		t.Fatal(err)
	}
	p := planInstall(dev, udid, src)
	if p.Install || !p.Grant { // this fake device has no TCC.db: grant
		t.Fatalf("installed, no TCC: %+v", p)
	}
	if sqlite(t, dev, "kTCCServiceCamera", "kTCCServiceMicrophone") {
		if p := planInstall(dev, udid, src); p.Install || p.Grant || !strings.Contains(p.Why, "already installed and granted") {
			t.Fatalf("installed + granted: %+v", p)
		}
		recordInstall(dev, udid, want, false) // the last grant failed: grant again
		if p := planInstall(dev, udid, src); p.Install || !p.Grant {
			t.Fatalf("record not granted: %+v", p)
		}
		recordInstall(dev, udid, want, true)
	}
	// a new canary build: reinstall
	src2 := writeApp(t, t.TempDir(), "build-2")
	if p := planInstall(dev, udid, src2); !p.Install || !p.Grant || !strings.Contains(p.Why, "differs from this build") {
		t.Fatalf("new build: %+v", p)
	}
	// the installed copy changed behind the record's back (same build on the host): reinstall
	os.WriteFile(filepath.Join(inst, "m10canary"), []byte("tampered"), 0o755)
	if p := planInstall(dev, udid, src); !p.Install {
		t.Fatalf("tampered install: %+v", p)
	}
	// a record for another container (a reinstall moved it): reinstall
	os.WriteFile(filepath.Join(inst, "m10canary"), []byte("build-1"), 0o755)
	recordInstall(dev, udid, want, true)
	moved := filepath.Join(filepath.Dir(filepath.Dir(inst)), "11111111-2222-3333-4444-555555555555")
	os.Rename(filepath.Dir(inst), moved)
	if p := planInstall(dev, udid, src); !p.Install || !strings.Contains(p.Why, "record differs") {
		t.Fatalf("moved container: %+v", p)
	}
	// two canaries installed: ambiguous, install
	c2 := filepath.Join(dev, "data", "Containers", "Bundle", "Application", "AAAAAAAA-2222-3333-4444-555555555555")
	os.MkdirAll(c2, 0o755)
	writeApp(t, c2, "build-1")
	if installedCanary(dev) != "" {
		t.Fatal("two installed canaries not treated as none")
	}
	// recordInstall refuses a copy that isn't the build it was told about
	dev3, _ := fakeDevice(t, "other")
	recordInstall(dev3, "U3", want, true)
	if _, err := os.Stat(CanaryRecordFile("U3")); err == nil {
		t.Fatal("recorded an install that doesn't match")
	}
}

// sqlite makes a TCC.db granting services to the canary; false if sqlite3 isn't there
func sqlite(t *testing.T, dev string, services ...string) bool {
	if _, err := os.Stat("/usr/bin/sqlite3"); err != nil {
		t.Log("no /usr/bin/sqlite3: TCC part skipped")
		return false
	}
	d := filepath.Join(dev, "data", "Library", "TCC")
	os.MkdirAll(d, 0o755)
	sql := "CREATE TABLE access (service TEXT, client TEXT, client_type INTEGER, auth_value INTEGER);"
	for _, s := range services {
		sql += "INSERT INTO access VALUES ('" + s + "','" + CanaryBundle + "',0,2);"
	}
	sql += "INSERT INTO access VALUES ('kTCCServiceCamera','other.app',0,2);"
	if out, err := exec.Command("/usr/bin/sqlite3", filepath.Join(d, "TCC.db"), sql).CombinedOutput(); err != nil {
		t.Fatalf("%v %s", err, out)
	}
	if !tccGranted(dev) {
		t.Fatal("granted TCC.db read as not granted")
	}
	return true
}

func TestTCCGranted(t *testing.T) {
	dev := t.TempDir()
	if tccGranted(dev) {
		t.Fatal("no TCC.db read as granted")
	}
	if _, err := os.Stat("/usr/bin/sqlite3"); err != nil {
		t.Skip("no sqlite3")
	}
	d := filepath.Join(dev, "data", "Library", "TCC")
	os.MkdirAll(d, 0o755)
	exec.Command("/usr/bin/sqlite3", filepath.Join(d, "TCC.db"), "CREATE TABLE access (service TEXT, client TEXT, auth_value INTEGER); INSERT INTO access VALUES ('kTCCServiceCamera','"+CanaryBundle+"',2); INSERT INTO access VALUES ('kTCCServiceMicrophone','"+CanaryBundle+"',0);").Run()
	if tccGranted(dev) {
		t.Fatal("microphone denied (0) read as granted")
	}
}
