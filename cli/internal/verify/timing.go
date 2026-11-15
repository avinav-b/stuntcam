// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"os"
	"time"
)

// SIMCAM_VERIFY_TIMING=1 prints one "TIMING <epoch s> <event>" line to stderr at each verify phase boundary, for
// splitting a verify's wall time into phases.
var timingOn = os.Getenv("SIMCAM_VERIFY_TIMING") == "1"

func mark(format string, a ...any) {
	if !timingOn {
		return
	}
	fmt.Fprintf(os.Stderr, "TIMING %.3f %s\n", float64(time.Now().UnixNano())/1e9, fmt.Sprintf(format, a...))
}

// timedSim marks the boot steps arm.ArmBoot takes (shutdown, boot, bootstatus) without changing them, and records when
// bootstatus returned (booted; the post-boot settle counts from it).
type timedSim struct {
	simSteps
	booted *time.Time
}

type simSteps interface {
	Boot(udid string, bootEnv []string) error
	Shutdown(udid string) error
	WaitBooted(udid string, bound time.Duration) error
	Getenv(udid, k string) (string, error)
	Setenv(udid, k, v string) error
	Unsetenv(udid, k string) error
}

func (t timedSim) Shutdown(u string) error {
	err := t.simSteps.Shutdown(u)
	mark("shutdown-done")
	return err
}
func (t timedSim) Boot(u string, env []string) error {
	err := t.simSteps.Boot(u, env)
	mark("boot-returned")
	return err
}
func (t timedSim) WaitBooted(u string, d time.Duration) error {
	err := t.simSteps.WaitBooted(u, d)
	if t.booted != nil && err == nil {
		*t.booted = time.Now()
	}
	mark("bootstatus-done")
	return err
}
