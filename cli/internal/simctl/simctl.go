// SPDX-License-Identifier: Apache-2.0

// Package simctl wraps `xcrun simctl` for the calls stuntcam needs. Every call can target a private device set (Set), which
// Xcode and Simulator never list (design §5.3).
package simctl

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type Device struct {
	UDID, Name, State, RuntimeID string
	Available                    bool
	LastBooted                   time.Time // zero if simctl does not report it
}

type Runtime struct {
	Identifier string       `json:"identifier"`
	Version    string       `json:"version"`
	Build      string       `json:"buildversion"`
	Platform   string       `json:"platform"`
	Available  bool         `json:"isAvailable"`
	Name       string       `json:"name"`
	Supported  []DeviceType `json:"supportedDeviceTypes"` // what this runtime can create (newer phones are refused, w2 e9)
}

type DeviceType struct {
	Identifier string `json:"identifier"`
	Name       string `json:"name"`
}

// Ctl runs simctl against the default device set, or Set if non-empty.
type Ctl struct {
	Set     string
	Timeout time.Duration // per call; 0 = 120 s
}

func (c Ctl) args(a ...string) []string {
	out := []string{"simctl"}
	if c.Set != "" {
		out = append(out, "--set", c.Set)
	}
	return append(out, a...)
}

// calls in flight, so a signal handler can stop them (Interrupt) before it cleans up
var live struct {
	sync.Mutex
	calls    map[*exec.Cmd]chan struct{} // closed when the call has exited
	consoles map[*exec.Cmd]bool          // LaunchConsole commands (their caller waits on them) until Forget
	stopping bool
}

// Interrupt sends SIGINT to every simctl call in flight (what a terminal Ctrl-C does to the whole process group: an
// interrupted `simctl boot` stops booting) and waits up to d for them to exit. After it, only `shutdown` and `list` calls
// run; any other call fails at once, so nothing new (a boot, a launch) starts behind a cleanup. A SIGTERM sent to stuntcam alone
// reaches no child: without this, a `simctl boot` that had just started went on to boot the device after the handler had
// already shut it down (w3 e17).
func Interrupt(d time.Duration) {
	live.Lock()
	live.stopping = true
	var done []chan struct{}
	for cmd, ch := range live.calls {
		if cmd.Process != nil {
			_ = cmd.Process.Signal(syscall.SIGINT)
		}
		done = append(done, ch)
	}
	for cmd := range live.consoles { // not waited for: the caller's cleanup ends them, or the device shutdown does
		_ = cmd.Process.Signal(syscall.SIGINT)
	}
	live.Unlock()
	deadline := time.After(d)
	for _, ch := range done {
		select {
		case <-ch:
		case <-deadline:
			return
		}
	}
}

// Busy says whether any Run call is still in flight (after Interrupt: one that SIGINT didn't stop within its wait).
func Busy() bool {
	live.Lock()
	defer live.Unlock()
	return len(live.calls) > 0
}

// Run returns stdout; stderr is folded into the error.
func (c Ctl) Run(env []string, a ...string) (string, error) {
	to := c.Timeout
	if to == 0 {
		to = 120 * time.Second
	}
	ctx, cancel := context.WithTimeout(context.Background(), to)
	defer cancel()
	cmd := exec.CommandContext(ctx, "xcrun", c.args(a...)...)
	cmd.Env = append(os.Environ(), env...)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	live.Lock()
	if live.stopping && (len(a) == 0 || (a[0] != "shutdown" && a[0] != "list")) {
		live.Unlock()
		return "", fmt.Errorf("simctl %s: interrupted", strings.Join(a, " "))
	}
	if err := cmd.Start(); err != nil {
		live.Unlock()
		return "", fmt.Errorf("simctl %s: %v", strings.Join(a, " "), err)
	}
	ch := make(chan struct{})
	if live.calls == nil {
		live.calls = map[*exec.Cmd]chan struct{}{}
	}
	live.calls[cmd] = ch
	live.Unlock()
	err := cmd.Wait()
	live.Lock()
	delete(live.calls, cmd)
	close(ch)
	live.Unlock()
	if ctx.Err() == context.DeadlineExceeded {
		return out.String(), fmt.Errorf("simctl %s: timed out after %s", strings.Join(a, " "), to)
	}
	if err != nil {
		return out.String(), fmt.Errorf("simctl %s: %v: %s", strings.Join(a, " "), err, strings.TrimSpace(errb.String()))
	}
	return out.String(), nil
}

func (c Ctl) Devices() ([]Device, error) {
	out, err := c.Run(nil, "list", "devices", "-j")
	if err != nil {
		return nil, err
	}
	return ParseDevices([]byte(out))
}

func ParseDevices(b []byte) ([]Device, error) {
	var v struct {
		Devices map[string][]struct {
			UDID      string `json:"udid"`
			Name      string `json:"name"`
			State     string `json:"state"`
			Available bool   `json:"isAvailable"`
			LastBoot  string `json:"lastBootedAt"`
			LastUsed  string `json:"lastUsedAt"` // Xcode 27 reports only this; it is set when the device boots (w3 e12)
		} `json:"devices"`
	}
	if err := json.Unmarshal(b, &v); err != nil {
		return nil, err
	}
	var out []Device
	for rt, ds := range v.Devices {
		for _, d := range ds {
			lb, _ := time.Parse(time.RFC3339, d.LastBoot)
			if lb.IsZero() {
				lb, _ = time.Parse(time.RFC3339, d.LastUsed)
			}
			out = append(out, Device{UDID: d.UDID, Name: d.Name, State: d.State, RuntimeID: rt, Available: d.Available, LastBooted: lb})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

func (c Ctl) Runtimes() ([]Runtime, error) {
	out, err := c.Run(nil, "list", "runtimes", "-j")
	if err != nil {
		return nil, err
	}
	return ParseRuntimes([]byte(out))
}

func ParseRuntimes(b []byte) ([]Runtime, error) {
	var v struct {
		Runtimes []Runtime `json:"runtimes"`
	}
	if err := json.Unmarshal(b, &v); err != nil {
		return nil, err
	}
	return v.Runtimes, nil
}

func (c Ctl) DeviceTypes() ([]DeviceType, error) {
	out, err := c.Run(nil, "list", "devicetypes", "-j")
	if err != nil {
		return nil, err
	}
	var v struct {
		DeviceTypes []DeviceType `json:"devicetypes"`
	}
	err = json.Unmarshal([]byte(out), &v) // not in the return statement: its operands are evaluated before the call
	return v.DeviceTypes, err
}

// NewestIOS picks the newest available iOS runtime, or the one matching want (a version like "26.3", or an identifier).
func NewestIOS(rs []Runtime, want string) (Runtime, bool) {
	var c []Runtime
	for _, r := range rs {
		if !r.Available || r.Platform != "iOS" {
			continue
		}
		if want == "" || r.Identifier == want || r.Version == want || strings.HasPrefix(r.Version, want+".") {
			c = append(c, r)
		}
	}
	if len(c) == 0 {
		return Runtime{}, false
	}
	sort.Slice(c, func(i, j int) bool { return versionLess(c[i].Version, c[j].Version) })
	return c[len(c)-1], true
}

func versionLess(a, b string) bool {
	pa, pb := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(pa) || i < len(pb); i++ {
		var x, y int
		if i < len(pa) {
			x, _ = strconv.Atoi(pa[i])
		}
		if i < len(pb) {
			y, _ = strconv.Atoi(pb[i])
		}
		if x != y {
			return x < y
		}
	}
	return false
}

// Boot boots with a boot environment: each "K=V" is passed as SIMCTL_CHILD_K=V, which every process of that boot inherits.
func (c Ctl) Boot(udid string, bootEnv []string) error {
	env := make([]string, 0, len(bootEnv))
	for _, kv := range bootEnv {
		env = append(env, "SIMCTL_CHILD_"+kv)
	}
	_, err := c.Run(env, "boot", udid)
	return err
}

func (c Ctl) Shutdown(udid string) error { _, err := c.Run(nil, "shutdown", udid); return err }

// WaitBooted runs `bootstatus -b` with a bound (macOS has no timeout(1)).
func (c Ctl) WaitBooted(udid string, bound time.Duration) error {
	c.Timeout = bound
	_, err := c.Run(nil, "bootstatus", udid, "-b")
	return err
}

func (c Ctl) Getenv(udid, k string) (string, error) {
	out, err := c.Run(nil, "spawn", udid, "launchctl", "getenv", k)
	return strings.TrimRight(out, "\n"), err
}
func (c Ctl) Setenv(udid, k, v string) error {
	_, err := c.Run(nil, "spawn", udid, "launchctl", "setenv", k, v)
	return err
}
func (c Ctl) Unsetenv(udid, k string) error {
	_, err := c.Run(nil, "spawn", udid, "launchctl", "unsetenv", k)
	return err
}
func (c Ctl) Install(udid, app string) error { _, err := c.Run(nil, "install", udid, app); return err }

// Grant grants a privacy service (camera, microphone, …) to a bundle. It terminates the app if it is running.
func (c Ctl) Grant(udid, service, bundle string) error {
	_, err := c.Run(nil, "privacy", udid, "grant", service, bundle)
	return err
}

// Create makes a device and returns its UDID.
func (c Ctl) Create(name, deviceType, runtime string) (string, error) {
	out, err := c.Run(nil, "create", name, deviceType, runtime)
	return strings.TrimSpace(out), err
}

// Spawn runs a command inside the booted sim and returns its stdout.
func (c Ctl) Spawn(udid string, a ...string) (string, error) {
	return c.Run(nil, append([]string{"spawn", udid}, a...)...)
}

// AppContainer returns an installed app's container path (kind: app, data, groups or an app group id). simctl answers only
// for a booted device.
func (c Ctl) AppContainer(udid, bundle, kind string) (string, error) {
	out, err := c.Run(nil, "get_app_container", udid, bundle, kind)
	return strings.TrimSpace(out), err
}

// NotInstalled says whether a get_app_container error means the app isn't installed on the (booted) device.
func NotInstalled(err error) bool {
	if err == nil {
		return false
	}
	s := err.Error()
	// only simctl's own "no such app" error (w4 e33); any other ENOENT text is not proof the app is gone (review 7)
	return strings.Contains(s, "domain=NSPOSIXErrorDomain, code=2")
}

// NotBooted says whether a simctl error means the device isn't booted ("Unable to lookup in current state: Shutdown").
func NotBooted(err error) bool {
	return err != nil && strings.Contains(err.Error(), "current state: Shutdown")
}

func (c Ctl) Launch(udid, bundle string) error {
	_, err := c.Run(nil, "launch", udid, bundle)
	return err
}
func (c Ctl) Terminate(udid, bundle string) error {
	_, err := c.Run(nil, "terminate", udid, bundle)
	return err
}

// LaunchConsole starts an app with its stdout/stderr attached (simctl launch --console-pty); the caller reads Stdout and must
// Wait or Kill the returned command. The command is `xcrun simctl …` itself, so signalling it reaches simctl.
// Like Run's calls it is refused after Interrupt, and Interrupt signals it; call Forget once it has exited.
func (c Ctl) LaunchConsole(udid, bundle string, env []string) (*exec.Cmd, *bytes.Buffer, error) {
	cmd := exec.Command("xcrun", c.args("launch", "--console-pty", udid, bundle)...)
	cmd.Env = append(os.Environ(), env...)
	var buf bytes.Buffer
	cmd.Stdout, cmd.Stderr = &lockedWriter{b: &buf}, &lockedWriter{b: &buf}
	live.Lock()
	defer live.Unlock()
	if live.stopping {
		return cmd, &buf, fmt.Errorf("simctl launch: interrupted")
	}
	if err := cmd.Start(); err != nil {
		return cmd, &buf, err
	}
	if live.consoles == nil {
		live.consoles = map[*exec.Cmd]bool{}
	}
	live.consoles[cmd] = true
	return cmd, &buf, nil
}

// Forget stops tracking a LaunchConsole command.
func Forget(cmd *exec.Cmd) {
	live.Lock()
	delete(live.consoles, cmd)
	live.Unlock()
}

// lockedWriter lets stdout and stderr share one buffer that the caller polls while the process runs.
type lockedWriter struct{ b *bytes.Buffer }

var bufMu sync.Mutex

func (w *lockedWriter) Write(p []byte) (int, error) {
	bufMu.Lock()
	defer bufMu.Unlock()
	return w.b.Write(p)
}

// Snapshot returns a copy of a buffer filled by LaunchConsole.
func Snapshot(b *bytes.Buffer) string { bufMu.Lock(); defer bufMu.Unlock(); return b.String() }
