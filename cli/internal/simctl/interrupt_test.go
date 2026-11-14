// SPDX-License-Identifier: Apache-2.0

package simctl

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A fake xcrun whose `boot` runs until it gets SIGINT: Interrupt must stop it, and refuse later calls except shutdown/list.
func TestInterruptStopsCallsInFlight(t *testing.T) {
	dir := t.TempDir()
	log := filepath.Join(dir, "calls.log")
	ready := filepath.Join(dir, "ready")
	script := "#!/bin/sh\necho \"$*\" >> " + log + "\ncase \"$*\" in *boot*) trap 'echo interrupted >> " + log + "; exit 130' INT; : > " + ready + "; while :; do sleep 0.05; done;; esac\nexit 0\n"
	if err := os.WriteFile(filepath.Join(dir, "xcrun"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	defer func() { live.Lock(); live.stopping = false; live.Unlock() }()

	c := Ctl{}
	errc := make(chan error, 1)
	go func() { errc <- c.Boot("U1", nil) }()
	for i := 0; ; i++ { // wait until the boot call is in flight and its SIGINT trap is set (a new script's first run is slow on macOS)
		if _, err := os.Stat(ready); err == nil {
			break
		}
		if i > 1000 {
			t.Fatal("boot never started")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !Busy() {
		t.Fatal("the boot call in flight is not tracked")
	}
	t0 := time.Now()
	Interrupt(5 * time.Second)
	if time.Since(t0) > 3*time.Second {
		t.Fatalf("Interrupt waited %s", time.Since(t0))
	}
	select {
	case err := <-errc:
		if err == nil {
			t.Fatal("interrupted boot reported success")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Boot did not return after Interrupt")
	}
	if err := c.Boot("U1", nil); err == nil || !strings.Contains(err.Error(), "interrupted") {
		t.Fatalf("a boot after Interrupt ran: %v", err)
	}
	if _, _, err := c.LaunchConsole("U1", "b", nil); err == nil {
		t.Fatal("a canary launch after Interrupt started")
	}
	if Busy() {
		t.Fatal("Busy after the interrupted boot exited")
	}
	if err := c.Shutdown("U1"); err != nil {
		t.Fatalf("shutdown after Interrupt: %v", err)
	}
	b, _ := os.ReadFile(log)
	if got := string(b); got != "simctl boot U1\ninterrupted\nsimctl shutdown U1\n" {
		t.Fatalf("calls:\n%s", got)
	}
}
