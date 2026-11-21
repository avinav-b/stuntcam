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
	fs := flag.NewFlagSet("gc", flag.ContinueOnError)
	dry := fs.Bool("dry-run", false, "list what would be removed")
	keep := fs.Int("keep-builds", defaultKeptBuilds, "newest unreferenced builds to keep")
	if err := parse(fs, args, 0); err != nil {
		return err
	}
	rm := func(what, p string) {
		if *dry {
			fmt.Println("would remove", what, p)
			return
		}
		if err := os.RemoveAll(p); err == nil {
			fmt.Println("removed", what, p)
		}
	}
	lists := listForGC()
	if lists.ok {
		gcPrivateDevices(lists, *dry)
	}
	refs := gcSimDirs(lists, rm)
	currentCanary, currentHelper := refsOfCurrentBuild(refs)
	refsOfService(refs)
	gcBuilds(refs, *keep, rm)
	gcCanaries(home.Canary(), currentCanary, keptCanaries, time.Now(), *dry, rm)
	gcHelpers(currentHelper, *dry, rm)
	gcStale(home.Rules(), time.Now(), rm) // StageRules' temp dirs
	return nil
}

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
	priv := simctl.Ctl{Set: home.Devices()}
	var l gcLists
	rs, rerr := ctl.Runtimes()
	pubDevs, derr := ctl.Devices()
	privDevs, privErr := priv.Devices()
	if privErr != nil {
		if _, statErr := os.Stat(home.Devices()); statErr != nil {
			privDevs, privErr = nil, nil // no private set yet
		}
	}
	l.runtimes, l.public, l.private = rs, pubDevs, privDevs
	l.ok = rerr == nil && derr == nil && privErr == nil && len(rs) > 0 && len(pubDevs) > 0
	if !l.ok {
		why := "no simulators or runtimes listed"
		for _, e := range []error{rerr, derr, privErr} {
			if e != nil {
				why = e.Error()
			}
		}
		fmt.Printf("keeping every simulator's state and the private devices (%s)\n", why)
	}
	return l
}

// gcPrivateDevices deletes the private canary devices of runtimes no longer available.
func gcPrivateDevices(l gcLists, dry bool) {
	priv := simctl.Ctl{Set: home.Devices()}
	avail := map[string]bool{}
	for _, r := range l.runtimes {
		if r.Available {
			avail[r.Identifier] = true
		}
	}
	for _, d := range l.private {
		if !avail[d.RuntimeID] {
			if dry {
				fmt.Println("would delete private canary device", d.Name, d.UDID)
			} else if _, err := priv.Run(nil, "delete", d.UDID); err == nil {
				fmt.Println("deleted private canary device", d.Name, d.UDID)
			}
		}
	}
}

// gcSimDirs removes the state of simulators that no longer exist (in the default or the private set; only when the
// lists are ok) and exec run directories older than execRunAge, and returns the builds the remaining ones reference
// (their current and arm record).
func gcSimDirs(l gcLists, rm func(what, p string)) map[string]bool {
	exists := map[string]bool{}
	for _, d := range append(append([]simctl.Device{}, l.public...), l.private...) {
		exists[d.UDID] = true
	}
	sims, _ := os.ReadDir(filepath.Join(home.Root(), "sims"))
	refs := map[string]bool{}
	for _, s := range sims {
		if strings.HasPrefix(s.Name(), "exec-") {
			// an exec run's directory: simulators it booted read its current until they shut down
			if fi, err := s.Info(); err == nil && time.Since(fi.ModTime()) > execRunAge {
				rm("finished exec run", filepath.Join(home.Root(), "sims", s.Name()))
				continue
			}
		} else if l.ok && !exists[s.Name()] {
			rm("state of deleted simulator", filepath.Join(home.Root(), "sims", s.Name()))
			continue
		}
		if _, ldr, err := home.ReadCurrent(s.Name()); err == nil {
			refs[filepath.Base(filepath.Dir(ldr))] = true
		}
		if r, err := arm.ReadRecord(s.Name()); err == nil {
			refs[r.Build] = true
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
	if b := env["STUNTCAM_BUILD"]; b != "" {
		refs[filepath.Base(b)] = true
	}
	if st := env["STUNTCAM_STUB"]; st != "" {
		refs[filepath.Base(filepath.Dir(st))] = true
	}
}

// gcBuilds removes the staged builds that are neither referenced nor always kept (alwaysKeptBuild), but the keep
// newest.
func gcBuilds(refs map[string]bool, keep int, rm func(what, p string)) {
	bs, _ := os.ReadDir(home.Builds())
	mtime := func(e os.DirEntry) time.Time {
		if fi, err := e.Info(); err == nil {
			return fi.ModTime()
		}
		return time.Time{}
	}
	sort.Slice(bs, func(i, j int) bool { return mtime(bs[i]).After(mtime(bs[j])) })
	kept := 0
	for _, b := range bs {
		p := filepath.Join(home.Builds(), b.Name())
		if refs[b.Name()] {
			continue
		}
		if alwaysKeptBuild(p) {
			continue
		}
		if kept < keep {
			kept++
			continue
		}
		rm("unused build", p)
	}
}

const (
	// staleAge: a .stage-*/.old-* dir older than this is left over from a stuntcam that died mid-unpack (an unpack takes
	// well under a second).
	staleAge = time.Hour
	// execRunAge: gc removes an exec run's directory older than this.
	execRunAge = 7 * 24 * time.Hour
	// canaryIdleAge: an unpacked canary untouched this long is never one a concurrent verify just picked.
	canaryIdleAge = 24 * time.Hour
	// keptCanaries: unpacked canaries kept besides the current one, newest first.
	keptCanaries = 2
	// defaultKeptBuilds: unreferenced builds kept, newest first, without --keep-builds.
	defaultKeptBuilds = 3
)

// gcCanaries removes the canaries unpacked under dir (<id>/m10canary.app, from installed packages) but the current one
// and the `keep` newest of the others (mtime: unpackTree touches the dir at each use), and stale temp dirs. It holds the
// unpack lock, so it never races an unpack. A removed canary comes back from its package's payload at that package's next
// use.
func gcCanaries(dir, current string, keep int, now time.Time, dry bool, rm func(what, p string)) {
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
	gcStale(dir, now, rm)
	es, _ := os.ReadDir(dir)
	type ent struct {
		name string
		mt   time.Time
	}
	var ids []ent
	for _, e := range es {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") || e.Name() == current {
			continue
		}
		if fi, err := e.Info(); err == nil {
			ids = append(ids, ent{e.Name(), fi.ModTime()})
		}
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i].mt.After(ids[j].mt) })
	for i, e := range ids {
		if i >= keep && now.Sub(e.mt) > canaryIdleAge {
			rm("old unpacked canary", filepath.Join(dir, e.name))
		}
	}
}

// gcStale removes .stage-* and .old-* entries in dir older than staleAge (younger ones may be an unpack in progress).
func gcStale(dir string, now time.Time, rm func(what, p string)) {
	es, _ := os.ReadDir(dir)
	for _, e := range es {
		if !strings.HasPrefix(e.Name(), ".stage-") && !strings.HasPrefix(e.Name(), ".old-") {
			continue
		}
		if fi, err := e.Info(); err == nil && now.Sub(fi.ModTime()) > staleAge {
			rm("stale temp dir", filepath.Join(dir, e.Name()))
		}
	}
}

// alwaysKeptBuild: a build dir `gc` never removes: a stub's (tiny, and a boot env may name any of them until that sim
// shuts down) or the runner hook's (tiny, and a running exec's test runners may name it).
func alwaysKeptBuild(dir string) bool {
	for _, n := range []string{artifacts.StubName, artifacts.HookName} {
		if _, err := os.Stat(filepath.Join(dir, n)); err == nil {
			return true
		}
	}
	return false
}
