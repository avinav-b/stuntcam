// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// running is the state of a verify on its device that the cleanup and the signal handler need.
type running struct {
	mu   sync.Mutex
	ctl  simctl.Ctl
	udid string
	keep bool
	lock string
	cmd  *exec.Cmd
}

func (r *running) cleanup() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.cleanupLocked()
}

func (r *running) cleanupLocked() {
	if r.cmd != nil && r.cmd.Process != nil {
		_ = r.cmd.Process.Signal(syscall.SIGTERM) // best effort cleanup: it may have exited already
	}

	if r.udid != "" && !r.keep {
		_ = r.ctl.Shutdown(r.udid) // best effort cleanup: fine if already shut down
	}

	if r.lock != "" {
		release(r.lock)
		r.lock = ""
	}
}

// settledDownFor is how long settleShutdown wants the device to stay shut down.
const settledDownFor = 2 * time.Second

// settleShutdown shuts the device down again if a boot that was past cancelling brings it up after all. It watches for
// up to limit and returns once no simctl call is still in flight (a boot that ignored Interrupt's SIGINT) and the
// device has stayed shut down for settledDownFor.
func settleShutdown(ctl simctl.Ctl, udid string, limit time.Duration) {
	var downSince time.Time
	for end := time.Now().Add(limit); time.Now().Before(end); time.Sleep(devicePoll) {
		devices, err := ctl.Devices()
		if err != nil {
			continue
		}

		down := true
		for _, listed := range devices {
			if listed.UDID == udid && listed.State != simctl.Shutdown {
				down = false
				_ = ctl.Shutdown(udid) // the loop checks the state again
			}
		}

		switch {
		case !down || simctl.Busy():
			downSince = time.Time{}
		case downSince.IsZero():
			downSince = time.Now()
		case time.Since(downSince) >= settledDownFor:
			return
		}
	}
}

// What an interrupted verify waits for.
const (
	interruptWait       = 10 * time.Second // for the simctl calls in flight to stop after their SIGINT
	interruptSettleWait = 30 * time.Second // for the device to stay shut down (settleShutdown)
	exitInterrupted     = 130              // 128 + SIGINT, as a shell reports a Ctrl-C
)

// onSignal makes SIGINT, SIGTERM and SIGHUP end the verify (interrupted) until the returned stop is called.
func (r *running) onSignal() (stop func()) {
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	stopped := make(chan struct{})
	go func() {
		select {
		case <-sigs:
			r.interrupted()
		case <-stopped:
		}
	}()

	return func() {
		signal.Stop(sigs)
		close(stopped)
	}
}

// interrupted ends an interrupted verify: it stops the simctl calls in flight, shuts the device down (unless keep),
// releases the lock once the device has settled, prints the inconclusive verdict and exits.
func (r *running) interrupted() {
	r.mu.Lock()                     // held until exit: Run can't go on (e.g. `up` arming the user's sim) after an interrupt
	simctl.Interrupt(interruptWait) // stop a boot in flight first, or it boots the device after the shutdown below
	lock := r.lock                  // kept until the device has settled, so the next verify can't boot it meanwhile
	r.lock = ""
	r.cleanupLocked()
	if r.udid != "" && !r.keep {
		settleShutdown(r.ctl, r.udid, interruptSettleWait)
		if simctl.Busy() {
			fmt.Fprintf(os.Stderr, "warning: a simctl call ignored the interrupt; the private canary device may "+
				"still boot: xcrun simctl --set %s shutdown %s\n", shellArg(home.Devices()), r.udid)
		}
	}

	if lock != "" {
		release(lock)
	}

	fmt.Println("verdict inconclusive(interrupted)")
	os.Exit(exitInterrupted)
}
