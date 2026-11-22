// SPDX-License-Identifier: Apache-2.0
package arm

import (
	"encoding/json"
	"fmt"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// TestLaunchdArmRollbackMatrix runs a failed launchd arm from every combination of what a sim can hold before it, and
// checks everything the rollback leaves against launchdBefore.rollback's documented contract: the down marker, `current`
// byte for byte, the simulator's launchd environment, arm.json (byte for byte, absent, or the launchd record it must
// get) and the error text. Any change to what ArmLaunchd reads before it writes, or to what the rollback puts back,
// fails a case here.
func TestLaunchdArmRollbackMatrix(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir()) // every case has its own udid, so the cases can run at the same time

	type currentState struct {
		name  string
		data  []byte // nil: no `current`
		build string // the build ReadCurrent finds, "" if it fails
	}
	currents := []currentState{
		{name: "no current"},
		{name: "current b0", data: []byte("1 /h/builds/b0/libsimcamloader.dylib\n"), build: "b0"},
		{name: "unreadable current", data: []byte("garbage\n")},
	}

	type recordState struct {
		name     string
		data     func(t *testing.T) []byte // nil: no arm.json
		launchd  []string                  // the names a launchd record lists (its variables are set in the sim)
		ownBoots bool                      // this process's BootUnarmed record, which the rollback doesn't put back
	}
	marshal := func(r Record) func(*testing.T) []byte {
		return func(t *testing.T) []byte {
			r.Proto = home.Proto
			data, err := json.MarshalIndent(r, "", " ")
			if err != nil {
				t.Fatal(err)
			}
			return data
		}
	}
	record110 := func(t *testing.T) []byte {
		data, err := os.ReadFile(filepath.Join("testdata", "arm-1.1.0-launchd.json"))
		if err != nil {
			t.Fatal(err)
		}
		return data
	}
	var names110Record struct{ Vars []string }
	if err := json.Unmarshal(record110(t), &names110Record); err != nil {
		t.Fatal(err)
	}
	records := []recordState{
		{name: "no record"},
		{name: "1.1.0 launchd record", data: record110, launchd: names110Record.Vars},
		{name: "launchd record", launchd: []string{"DYLD_INSERT_LIBRARIES", "SIMCAM_SIM_DIR", "SIMCAM_SOURCE"},
			data: marshal(Record{Mode: Launchd, Build: "b1", At: 42,
				Vars: []string{"DYLD_INSERT_LIBRARIES", "SIMCAM_SIM_DIR", "SIMCAM_SOURCE"}})},
		{name: "boot record", data: marshal(Record{Mode: Boot, Build: "b0", At: 42, Vars: []string{"DYLD_INSERT_LIBRARIES"}})},
		{name: "env record", data: marshal(Record{Mode: Env, Build: "b0", At: 42})},
		{name: "own boot-in-progress record", ownBoots: true,
			data: marshal(Record{Mode: Launchd, Build: "b2", At: time.Now().Unix(), Booting: true, Pid: os.Getpid()})},
		// a mistyped field: the rest still parses, so its launchd names count, and its bytes go back as they were
		{name: "record with a mistyped field", launchd: []string{"DYLD_INSERT_LIBRARIES", "SIMCAM_SOURCE"},
			data: func(*testing.T) []byte {
				return []byte(`{"mode":"launchd","build":"b1","at":"soon","vars":["DYLD_INSERT_LIBRARIES","SIMCAM_SOURCE"]}`)
			}},
	}

	const (
		otherTool = "/t3/serve-sim.dylib"
		oldStub   = "/h/builds/s0/libsimcamstub.dylib"
	)
	failed := map[string]string{"a setting": "SIMCAM_CHAIN", "the stub": "DYLD_INSERT_LIBRARIES"}
	b2 := artifacts.Set{Stub: "/h/builds/s2/libsimcamstub.dylib", Loader: "/h/builds/b2/libsimcamloader.dylib",
		Engine: "/h/builds/b2/libsimcam.dylib"}

	i := 0
	for _, current := range currents {
		for _, what := range slices.Sorted(maps.Keys(failed)) {
			for _, unsetFails := range []bool{false, true} {
				// records is the innermost loop and has an odd length, so these two alternate against every other
				// dimension without multiplying the cases by four
				for _, record := range records {
					down, withOtherTool := i%2 == 1, i%4 >= 2
					udid := fmt.Sprintf("U%d", i)
					i++
					name := fmt.Sprintf("%s, %s, %s fails, unset fails %v, down %v, other tool %v", current.name,
						record.name, what, unsetFails, down, withOtherTool)
					t.Run(name, func(t *testing.T) {
						t.Parallel()
						failKey := failed[what]

						// before: the sim dir, the down marker, `current`, arm.json and the launchd environment
						if err := os.MkdirAll(home.Sim(udid), 0o755); err != nil {
							t.Fatal(err)
						}
						if down {
							writeFile(t, DownMarker(udid), []byte("1\n"))
						}
						if current.data != nil {
							writeFile(t, home.CurrentFile(udid), current.data)
						}
						var recordBefore []byte
						if record.data != nil {
							recordBefore = record.data(t)
							writeFile(t, recordPath(udid), recordBefore)
						}
						insert := ""
						if withOtherTool {
							insert = otherTool + ":" + oldStub
						}
						f := &fakeSim{env: map[string]string{}}
						if insert != "" {
							f.env["DYLD_INSERT_LIBRARIES"] = insert
						}
						for _, name := range record.launchd {
							if name != "DYLD_INSERT_LIBRARIES" {
								f.env[name] = "old-" + name
							}
						}
						f.failKey = failKey
						if unsetFails {
							f.unsetErrKey = "SIMCAM_STATE_DIR"
						}

						reads := f.env[failKey] // the fake never lets failKey stick, so it reads as before
						vars := Vars(udid, b2, Options{})
						err := ArmLaunchd(f, udid, b2, vars)

						// the error: the failed set, then the rollback's note
						wantErr := fmt.Sprintf("launchctl setenv %s did not stick (reads %q); rolled back (other tools' "+
							"DYLD_INSERT_LIBRARIES entries kept, stuntcam's removed)", failKey, reads)
						if err == nil || err.Error() != wantErr {
							t.Fatalf("err %v, want %s", err, wantErr)
						}

						// the down marker is back if it was there, and only then
						if _, err := os.Stat(DownMarker(udid)); (err == nil) != down {
							t.Errorf("down marker exists %v, want %v", err == nil, down)
						}

						// `current` is back byte for byte, or absent
						gotCurrent, err := os.ReadFile(home.CurrentFile(udid))
						if (err == nil) != (current.data != nil) || string(gotCurrent) != string(current.data) {
							t.Errorf("current %q (%v), want %q", gotCurrent, err, current.data)
						}

						// the launchd environment: other tools' entries without stuntcam's, and none of stuntcam's
						// variables except one whose unset failed
						rest := ""
						if withOtherTool {
							rest = otherTool
						}
						restored := rest == "" || failKey != "DYLD_INSERT_LIBRARIES" // the rollback's own set sticks
						wantEnv := map[string]string{}
						switch {
						case rest == "":
						case restored:
							wantEnv["DYLD_INSERT_LIBRARIES"] = rest
						default:
							wantEnv["DYLD_INSERT_LIBRARIES"] = insert
						}
						if unsetFails {
							wantEnv["SIMCAM_STATE_DIR"] = home.StateDir(udid)
						}
						if !maps.Equal(f.env, wantEnv) {
							t.Errorf("launchd environment %v, want %v", f.env, wantEnv)
						}

						// arm.json: put back when the rollback was clean and there was an earlier arm's record; absent
						// when it was clean and there was neither such a record nor a `current`; otherwise a finished
						// launchd record for the build `current` is back on, naming every variable this arm and an
						// earlier launchd arm set
						clean := restored && !unsetFails
						found := record.data != nil && !record.ownBoots
						gotRecord, readErr := os.ReadFile(recordPath(udid))
						switch {
						case found && clean:
							if string(gotRecord) != string(recordBefore) {
								t.Errorf("arm.json %s (%v), want it put back:\n%s", gotRecord, readErr, recordBefore)
							}
						case !found && clean && current.data == nil:
							if readErr == nil {
								t.Errorf("arm.json left: %s", gotRecord)
							}
						default:
							wantBuild := current.build
							if wantBuild == "" {
								wantBuild = b2.BuildID()
							}
							wantVars := names(vars)
							if record.launchd != nil {
								wantVars = slices.Clone(record.launchd)
								for _, name := range names(vars) {
									if !slices.Contains(wantVars, name) {
										wantVars = append(wantVars, name)
									}
								}
							}
							var got Record
							if err := json.Unmarshal(gotRecord, &got); err != nil {
								t.Fatalf("arm.json %s (%v, %v), want a launchd record", gotRecord, readErr, err)
							}
							want := Record{Proto: home.Proto, Mode: Launchd, Build: wantBuild, At: got.At, Vars: wantVars}
							if !recordsEqual(got, want) || time.Since(time.Unix(got.At, 0)) > time.Minute {
								t.Errorf("arm.json %+v, want %+v (at now)", got, want)
							}
						}
					})
				}
			}
		}
	}
}

func writeFile(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func recordsEqual(a, b Record) bool {
	return a.Proto == b.Proto && a.Mode == b.Mode && a.Build == b.Build && a.At == b.At && slices.Equal(a.Vars, b.Vars) &&
		a.Booting == b.Booting && a.Pid == b.Pid
}
