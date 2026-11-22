// SPDX-License-Identifier: Apache-2.0

package verify

import "time"

// Test seams: the hooks tests replace to stand in for the host, the device or the clock. Production code only reads
// them; each defaults to the real implementation or value.
var (
	hostProcs        = realHostProcs        // ps, for the SpringBoard guard (springboard.go)
	springboardSpawn = realSpringboardSpawn // simctl spawn, the guard's slow fallback poll (springboard.go)
	spawnShutdown    = realSpawnShutdown    // starts the detached shutdown child (shutdown.go)
	writeLockPid     = realWriteLockPid     // names a new lock owner; tests make it fail (shutdown.go)
	installGrant     = realInstallGrant     // installs and grants the canary (device.go)
)

// canaryResultWait is how long runCanary waits for the RESULT line: the canary's own budget is 45 s. The no-verdict
// text is built from it ("no verdict in 60 s"); scripts/verify.sh spells the same text out, and the two share the
// verdict cache, so change both together.
var canaryResultWait = 60 * time.Second

// handoffGrace is how long the shutdown child waits for its parent to name it the lock's owner before it decides whose
// lock it is.
var handoffGrace = 5 * time.Second
