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

// cachePath is the verdict file for key: proto 1 JSON, written atomically, holding only a pass or a fail(...) verdict.
func cachePath(key string) string { return filepath.Join(home.Verdicts(), key+".json") }

// Cached returns a cached pass or fail for key; anything else (no file, unreadable, another verdict, a retried pass
// older than RetriedPassTTL) is a miss.
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
	data, err := os.ReadFile(cachePath(key))
	if err != nil {
		return cacheEntry{}
	}

	var cachedResult Result
	if json.Unmarshal(data, &cachedResult) != nil {
		return cacheEntry{why: "the cached verdict for this build is unreadable: running the canary again",
			present: true}
	}

	if cachedResult.State = StateOf(cachedResult.Verdict); cachedResult.State != Pass && cachedResult.State != Fail {
		why := fmt.Sprintf("the cached verdict for this build is %q, not a pass or fail: running the canary again",
			cachedResult.Verdict)
		return cacheEntry{why: why, present: true}
	}

	if cachedResult.Retried == "" { // a file scripts/verify.sh or an older CLI wrote: the RESULT line still says
		cachedResult.Retried = Retried(cachedResult.CanaryResult)
	}

	if cachedResult.RetriedPass() && now.Sub(time.Unix(cachedResult.At, 0)) > RetriedPassTTL {
		what := "needed a canary retry"
		switch {
		case cachedResult.Retried == "" && cachedResult.Relaunched != "":
			what = "needed a canary retry (a relaunch)"
		case cachedResult.Retried == "" && cachedResult.FailedFirst != "":
			what = "needed a canary retry (its first launch failed)"
		}

		why := fmt.Sprintf("the cached pass from %s %s and is over %s old: running the canary again",
			time.Unix(cachedResult.At, 0).Format("15:04"), what, RetriedPassTTLText())
		return cacheEntry{why: why, present: true}
	}

	cachedResult.Cached = true
	return cacheEntry{result: cachedResult, usable: true, present: true}
}

// lookup is the verdict without a canary run: this Mac's cache, else a shipped verdict from shippedDir. Any entry this
// Mac has for key (also an expired retried or relaunched pass, or an unreadable one) means this Mac measured this build
// itself: the shipped verdict is not used then, and the canary runs again.
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

// Shipped returns the package's verdict for key from dir, and only a plain pass.
// dir is the installed layout's share/stuntcam/verdicts. scripts/package.sh --verdicts writes it from the release
// build's own canary runs (scripts/ci-verdicts.sh).
// This Mac's own cache is read first, and any entry there for key wins.
// A shipped fail, a retried or relaunched pass (short-lived by design) or an unreadable file is a miss, and then the
// canary runs.
// `verify --fresh` and exported SIMCTL_CHILD_* variables bypass it, as they bypass the cache.
func Shipped(dir, key string) (Result, bool) {
	if dir == "" || key == "" || strings.ContainsAny(key, "/\\") {
		return Result{}, false
	}

	var result Result
	data, err := os.ReadFile(filepath.Join(dir, key+".json"))
	if err != nil || json.Unmarshal(data, &result) != nil || result.Key != key {
		return Result{}, false
	}

	result.State = StateOf(result.Verdict)
	if result.State != Pass || result.Retried != "" || result.Relaunched != "" || result.FailedFirst != "" ||
		Retried(result.CanaryResult) != "" {
		return Result{}, false
	}

	result.Cached = true
	result.Shipped = true
	return result, true
}

// ShippedSource describes a shipped verdict for messages: when the release build measured it and with which Xcode the
// package was built (recorded by scripts/package.sh).
func (r Result) ShippedSource() string {
	xcode := r.PackagerXcode
	if xcode == "" {
		xcode = "unknown"
	}
	return fmt.Sprintf("measured %s, packaged with Xcode %s", time.Unix(r.At, 0).Format("2006-01-02"), xcode)
}

func store(result Result) error {
	if result.State != Pass && result.State != Fail {
		return nil
	}
	if err := os.MkdirAll(home.Verdicts(), 0o755); err != nil {
		return err
	}
	result.Proto = home.Proto
	data, _ := json.MarshalIndent(result, "", " ") // can't fail: plain fields
	return home.WriteAtomic(cachePath(result.Key), data)
}

// noCacheReason names exported settings that change the canary's result but aren't in the key (SIMCTL_CHILD_* reach
// the canary's boot): with any of them the cache is neither read nor written.
func noCacheReason() string {
	var names []string
	for _, kv := range os.Environ() {
		// SIMCAM_VERIFY_SETTLE_S overrides the post-boot settle floor
		name, _, _ := strings.Cut(kv, "=")
		if strings.HasPrefix(name, "SIMCTL_CHILD_") || name == "SIMCAM_VERIFY_SETTLE_S" {
			names = append(names, name)
		}
	}
	return strings.Join(names, " ")
}
