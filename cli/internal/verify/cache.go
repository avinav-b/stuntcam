// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

func cachePath(key string) string { return filepath.Join(home.Verdicts(), key+".json") }

// Cached returns a cached pass or fail for key; anything else (no file, unreadable, another verdict, a retried pass older
// than RetriedPassTTL) is a miss.
func Cached(key string) (Result, bool) {
	c := cached(key, time.Now())
	return c.result, c.usable
}

// cacheEntry is what this Mac's verdict cache has for a key.
type cacheEntry struct {
	result  Result // the verdict, if usable
	usable  bool   // a pass or fail to use as it is
	why     string // why an existing entry was not used (a retried pass that expired, an unreadable file), else ""
	present bool   // the cache has an entry for the key at all, used or not
}

// cached is Cached, with why a present entry was not used.
func cached(key string, now time.Time) cacheEntry {
	b, err := os.ReadFile(cachePath(key))
	if err != nil {
		return cacheEntry{}
	}
	var r Result
	if json.Unmarshal(b, &r) != nil {
		return cacheEntry{why: "the cached verdict for this build is unreadable: running the canary again", present: true}
	}
	if r.State = StateOf(r.Verdict); r.State != Pass && r.State != Fail {
		why := fmt.Sprintf("the cached verdict for this build is %q, not a pass or fail: running the canary again", r.Verdict)
		return cacheEntry{why: why, present: true}
	}
	if r.Retried == "" { // a file scripts/verify.sh or an older CLI wrote: the RESULT line still says
		r.Retried = Retried(r.CanaryResult)
	}
	if r.RetriedPass() && now.Sub(time.Unix(r.At, 0)) > RetriedPassTTL {
		what := "needed a canary retry"
		switch {
		case r.Retried == "" && r.Relaunched != "":
			what = "needed a canary retry (a relaunch)"
		case r.Retried == "" && r.FailedFirst != "":
			what = "needed a canary retry (its first launch failed)"
		}
		why := fmt.Sprintf("the cached pass from %s %s and is over %s old: running the canary again",
			time.Unix(r.At, 0).Format("15:04"), what, RetriedPassTTLText())
		return cacheEntry{why: why, present: true}
	}
	r.Cached = true
	return cacheEntry{result: r, usable: true, present: true}
}

// lookup is the verdict without a canary run: this Mac's cache, else a shipped verdict from shippedDir. Any entry this Mac
// has for key (also an expired retried or relaunched pass, or an unreadable one) means this Mac measured this build itself:
// the shipped verdict is not used then, and the canary runs again.
func lookup(key, shippedDir string, now time.Time) (Result, bool, string) {
	own := cached(key, now)
	switch {
	case own.usable:
		return own.result, true, ""
	case own.present:
		why := own.why
		if _, shipped := Shipped(shippedDir, key); shipped {
			why += " (this Mac's own verdict for this build takes precedence over the one shipped with the package)"
		}
		return Result{}, false, why
	}
	if c, ok := Shipped(shippedDir, key); ok {
		return c, true, ""
	}
	return Result{}, false, ""
}

// Shipped returns the package's verdict for key from dir (the installed layout's share/stuntcam/verdicts, written by
// scripts/package.sh --verdicts from release-gate runs): a plain pass only. This Mac's own cache is read first, and any
// entry there for key wins; a shipped fail, a retried or relaunched pass (short-lived by design) or an unreadable file is
// a miss, and the canary runs. `verify --fresh` and exported SIMCTL_CHILD_* bypass it like the cache.
func Shipped(dir, key string) (Result, bool) {
	if dir == "" || key == "" || strings.ContainsAny(key, "/\\") {
		return Result{}, false
	}
	var r Result
	b, err := os.ReadFile(filepath.Join(dir, key+".json"))
	if err != nil || json.Unmarshal(b, &r) != nil || r.Key != key {
		return Result{}, false
	}
	r.State = StateOf(r.Verdict)
	if r.State != Pass || r.Retried != "" || r.Relaunched != "" || r.FailedFirst != "" || Retried(r.CanaryResult) != "" {
		return Result{}, false
	}
	r.Cached, r.Shipped = true, true
	return r, true
}

// ShippedSource describes a shipped verdict for messages: when the release gate measured it and with which Xcode the
// package was built (recorded by scripts/package.sh).
func (r Result) ShippedSource() string {
	x := r.PackagerXcode
	if x == "" {
		x = "unknown"
	}
	return fmt.Sprintf("measured %s, packaged with Xcode %s", time.Unix(r.At, 0).Format("2006-01-02"), x)
}

func store(r Result) error {
	if r.State != Pass && r.State != Fail {
		return nil
	}
	if err := os.MkdirAll(home.Verdicts(), 0o755); err != nil {
		return err
	}
	r.Proto = home.Proto
	b, _ := json.MarshalIndent(r, "", " ")
	return home.WriteAtomic(cachePath(r.Key), b)
}

// noCacheReason names exported settings that change the canary's result but aren't in the key.
func noCacheReason() string {
	var n []string
	for _, kv := range os.Environ() {
		// SIMCAM_VERIFY_SETTLE_S overrides the post-boot settle floor
		if k, _, _ := strings.Cut(kv, "="); strings.HasPrefix(k, "SIMCTL_CHILD_") || k == "SIMCAM_VERIFY_SETTLE_S" {
			n = append(n, k)
		}
	}
	return strings.Join(n, " ")
}
