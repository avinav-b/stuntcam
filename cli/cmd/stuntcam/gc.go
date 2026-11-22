// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/arm"
	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/service"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

func cmdGC(args []string) error {
	flags := flag.NewFlagSet("gc", flag.ContinueOnError)
	dry := flags.Bool("dry-run", false, "list what would be removed")
	keep := flags.Int("keep-builds", defaultKeptBuilds, "newest unreferenced builds to keep")
	if err := parse(flags, args, 0); err != nil {
		return err
	}

	remove := func(what, path string) {
		if *dry {
			fmt.Println("would remove", what, path)
			return
		}

		if err := os.RemoveAll(path); err == nil {
			fmt.Println("removed", what, path)
		}
	}

	lists := listForGC()
	if lists.ok {
		gcPrivateDevices(lists, *dry)
	}

	refs := gcSimDirs(lists, remove)
	currentCanary, currentHelper := refsOfCurrentBuild(refs)
	refsOfService(refs)
	gcBuilds(refs, *keep, remove)
	gcCanaries(home.Canary(), currentCanary, keptCanaries, time.Now(), *dry, remove)
	gcHelpers(currentHelper, *dry, remove)
	gcStale(home.Rules(), time.Now(), remove) // StageRules' temp dirs
	return nil
}

// removeFunc removes path, or under --dry-run only prints what it would remove; what names the kind of thing.
type removeFunc func(what, path string)

// gcLists is what simctl lists for gc. Per-simulator state and private devices are removed only when ok: every listing
// succeeded and isn't empty, since an empty list from a timed-out or out-of-date simctl would make every simulator
// look deleted. Builds and old exec runs don't depend on those lists and are cleaned anyway.
type gcLists struct {
	runtimes        []simctl.Runtime
	public, private []simctl.Device // the default device set's and the canary's private set's
	ok              bool
}

// listForGC lists the runtimes and both device sets, and says when it keeps every simulator's state because of them.
func listForGC() gcLists {
	privateSet := simctl.Ctl{Set: home.Devices()}
	var lists gcLists
	listedRuntimes, runtimesErr := ctl.Runtimes()
	publicDevices, publicErr := ctl.Devices()
	privateDevices, privateErr := privateSet.Devices()
	if privateErr != nil {
		if _, statErr := os.Stat(home.Devices()); statErr != nil {
			privateDevices = nil
			privateErr = nil // no private set yet
		}
	}

	lists.runtimes = listedRuntimes
	lists.public = publicDevices
	lists.private = privateDevices
	lists.ok = runtimesErr == nil && publicErr == nil && privateErr == nil && len(listedRuntimes) > 0 &&
		len(publicDevices) > 0
	if !lists.ok {
		reason := "no simulators or runtimes listed"
		for _, listErr := range []error{runtimesErr, publicErr, privateErr} {
			if listErr != nil {
				reason = listErr.Error()
			}
		}

		fmt.Printf("keeping every simulator's state and the private devices (%s)\n", reason)
	}

	return lists
}

// gcPrivateDevices deletes the private canary devices of runtimes no longer available.
func gcPrivateDevices(lists gcLists, dry bool) {
	privateSet := simctl.Ctl{Set: home.Devices()}
	available := map[string]bool{}
	for _, runtime := range lists.runtimes {
		if runtime.Available {
			available[runtime.Identifier] = true
		}
	}

	for _, device := range lists.private {
		if !available[device.RuntimeID] {
			if dry {
				fmt.Println("would delete private canary device", device.Name, device.UDID)
			} else if _, err := privateSet.Run(nil, "delete", device.UDID); err == nil {
				fmt.Println("deleted private canary device", device.Name, device.UDID)
			}
		}
	}
}

// gcSimDirs removes the state of simulators that no longer exist, in the default or the private set. It does that
// only when the lists are ok. It also removes exec run directories older than execRunAge.
// It returns the builds that the remaining ones reference, through their current and arm record.
func gcSimDirs(lists gcLists, remove removeFunc) map[string]bool {
	exists := map[string]bool{}
	for _, device := range append(append([]simctl.Device{}, lists.public...), lists.private...) {
		exists[device.UDID] = true
	}

	sims, _ := os.ReadDir(filepath.Join(home.Root(), "sims")) // no dir: no sims
	refs := map[string]bool{}
	for _, simDir := range sims {
		if strings.HasPrefix(simDir.Name(), "exec-") {
			// an exec run's directory: simulators it booted read its current until they shut down
			if fi, err := simDir.Info(); err == nil && time.Since(fi.ModTime()) > execRunAge {
				remove("finished exec run", filepath.Join(home.Root(), "sims", simDir.Name()))
				continue
			}
		} else if lists.ok && !exists[simDir.Name()] {
			remove("state of deleted simulator", filepath.Join(home.Root(), "sims", simDir.Name()))
			continue
		}

		if _, loaderPath, err := home.ReadCurrent(simDir.Name()); err == nil {
			refs[filepath.Base(filepath.Dir(loaderPath))] = true
		}

		if record, err := arm.ReadRecord(simDir.Name()); err == nil {
			refs[record.Build] = true
		}
	}

	return refs
}

// refsOfCurrentBuild adds the build stuntcam uses now (its loader, stub and hook dirs) to refs and returns its
// unpacked canary's id (if it is one under the canary dir) and its camera helper's sha.
func refsOfCurrentBuild(refs map[string]bool) (currentCanary, currentHelper string) {
	set, err := artifacts.Find("")
	if err != nil {
		return "", ""
	}

	currentHelper = set.HelperSHA()
	refs[filepath.Base(filepath.Dir(set.Loader))], refs[filepath.Base(filepath.Dir(set.Stub))] = true, true
	if set.Hook != "" {
		refs[filepath.Base(filepath.Dir(set.Hook))] = true
	}

	if id := filepath.Dir(set.Canary); set.Canary != "" && filepath.Dir(id) == home.Canary() {
		currentCanary = filepath.Base(id)
	}

	return currentCanary, currentHelper
}

// refsOfService adds the login service's build to refs: deleting it would leave the agent unable to start.
func refsOfService(refs map[string]bool) {
	env := service.InstalledEnv()
	if env == nil {
		return
	}

	if build := env["STUNTCAM_BUILD"]; build != "" {
		refs[filepath.Base(build)] = true
	}

	if stub := env["STUNTCAM_STUB"]; stub != "" {
		refs[filepath.Base(filepath.Dir(stub))] = true
	}
}

// gcBuilds removes the staged builds that are neither referenced nor always kept (alwaysKeptBuild), except the newest
// keep of them.
func gcBuilds(refs map[string]bool, keep int, remove removeFunc) {
	builds, _ := os.ReadDir(home.Builds()) // no dir: nothing to collect
	mtime := func(entry os.DirEntry) time.Time {
		if fi, err := entry.Info(); err == nil {
			return fi.ModTime()
		}

		return time.Time{}
	}

	sort.Slice(builds, func(i, j int) bool { return mtime(builds[i]).After(mtime(builds[j])) })
	kept := 0
	for _, build := range builds {
		path := filepath.Join(home.Builds(), build.Name())
		if refs[build.Name()] {
			continue
		}

		if alwaysKeptBuild(path) {
			continue
		}

		if kept < keep {
			kept++
			continue
		}

		remove("unused build", path)
	}
}

const (
	// staleAge is the age from which a .stage-*/.old-* dir counts as left over from a stuntcam that died mid-unpack
	// (an unpack takes well under a second).
	staleAge = time.Hour
	// execRunAge is the age from which gc removes an exec run's directory.
	execRunAge = 7 * 24 * time.Hour
	// canaryIdleAge is how long an unpacked canary must be untouched before gc removes it, so that it is never one a
	// concurrent verify just picked.
	canaryIdleAge = 24 * time.Hour
	// keptCanaries is how many unpacked canaries gc keeps besides the current one, newest first.
	keptCanaries = 2
	// defaultKeptBuilds is how many unreferenced builds gc keeps, newest first, without --keep-builds.
	defaultKeptBuilds = 3
)

// gcCanaries removes the canaries unpacked under dir (<id>/m10canary.app, from installed packages) but the current one
// and the `keep` newest of the others (mtime: unpackTree touches the dir at each use), and stale temp dirs. It holds
// the unpack lock, so it never races an unpack. A removed canary comes back from its package's payload at that
// package's next use.
func gcCanaries(dir, current string, keep int, now time.Time, dry bool, remove removeFunc) {
	if _, err := os.Stat(dir); err != nil {
		return
	}

	if !dry {
		unlock, err := artifacts.LockDir(dir)
		if err != nil {
			fmt.Println("keeping the unpacked canaries:", err)
			return
		}
		defer unlock()
	}

	gcStale(dir, now, remove)
	entries, _ := os.ReadDir(dir) // no dir: nothing to collect
	type ent struct {
		name string
		mt   time.Time
	}

	var ids []ent
	for _, entry := range entries {
		if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") || entry.Name() == current {
			continue
		}

		if fi, err := entry.Info(); err == nil {
			ids = append(ids, ent{entry.Name(), fi.ModTime()})
		}
	}

	sort.Slice(ids, func(i, j int) bool { return ids[i].mt.After(ids[j].mt) })
	for i, canary := range ids {
		if i >= keep && now.Sub(canary.mt) > canaryIdleAge {
			remove("old unpacked canary", filepath.Join(dir, canary.name))
		}
	}
}

// gcStale removes .stage-* and .old-* entries in dir older than staleAge (younger ones may be an unpack in progress).
func gcStale(dir string, now time.Time, remove removeFunc) {
	entries, _ := os.ReadDir(dir) // no dir: nothing to collect
	for _, entry := range entries {
		if !strings.HasPrefix(entry.Name(), ".stage-") && !strings.HasPrefix(entry.Name(), ".old-") {
			continue
		}
		if fi, err := entry.Info(); err == nil && now.Sub(fi.ModTime()) > staleAge {
			remove("stale temp dir", filepath.Join(dir, entry.Name()))
		}
	}
}

// alwaysKeptBuild reports whether dir is a build dir `gc` never removes: a stub's (tiny, and a boot environment may
// name any of them until that sim shuts down) or the runner hook's (tiny, and a running exec's test runners may name
// it).
func alwaysKeptBuild(dir string) bool {
	for _, name := range []string{artifacts.StubName, artifacts.HookName} {
		if _, err := os.Stat(filepath.Join(dir, name)); err == nil {
			return true
		}
	}
	return false
}
