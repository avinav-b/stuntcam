// SPDX-License-Identifier: Apache-2.0

// probe prints what artifacts.Find resolves from where it runs (TestFindFollowsSymlinkedExecutable). With "here" it
// calls artifacts.InstalledHere instead, after waiting for the file named by $PROBE_GO (if set) to exist, and checks the
// unpacked canary right away (TestInstalledConcurrentProcesses).
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
)

func main() {
	if len(os.Args) > 1 && os.Args[1] == "here" {
		if g := os.Getenv("PROBE_GO"); g != "" {
			for deadline := time.Now().Add(30 * time.Second); ; time.Sleep(time.Millisecond) {
				if _, err := os.Stat(g); err == nil || time.Now().After(deadline) {
					break
				}
			}
		}
		set, err := artifacts.InstalledHere()
		if err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
		fi, err := os.Stat(filepath.Join(set.Canary, "m10canary"))
		if err != nil || fi.Mode().Perm() != 0o755 {
			fmt.Println("canary missing or not executable right after InstalledHere:", fi, err)
			os.Exit(1)
		}
		fmt.Println("ok", set.BuildID())
		return
	}
	found, err := artifacts.Find("")
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	where := "elsewhere"
	if found.Installed {
		where = "installed"
	}
	fmt.Println(found.BuildID(), where)
}
