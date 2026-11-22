// SPDX-License-Identifier: Apache-2.0

// Package simctl wraps `xcrun simctl` for the calls stuntcam needs. Every call can target a private device set (Set),
// which Xcode and Simulator never list.
package simctl

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
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

// State is a device's state as `simctl list devices` reports it.
type State string

// The device states stuntcam acts on.
const (
	Booted       State = "Booted"
	Shutdown     State = "Shutdown"
	Booting      State = "Booting"
	ShuttingDown State = "Shutting Down"
	Creating     State = "Creating"
)

type Device struct {
	UDID, Name, RuntimeID string
	State                 State
	Available             bool
	LastBooted            time.Time // zero if simctl does not report it
}

type Runtime struct {
	Identifier string       `json:"identifier"`
	Version    string       `json:"version"`
	Build      string       `json:"buildversion"`
	Platform   string       `json:"platform"`
	Available  bool         `json:"isAvailable"`
	Name       string       `json:"name"`
	Supported  []DeviceType `json:"supportedDeviceTypes"` // what this runtime can create (newer phones are refused)
}

type DeviceType struct {
	Identifier string `json:"identifier"`
	Name       string `json:"name"`
}

// Ctl runs simctl against the default device set, or Set if non-empty.
type Ctl struct {
	Set     string
	Timeout time.Duration // per call; 0 = DefaultTimeout
}

// DefaultTimeout bounds each call of a Ctl whose Timeout is 0.
const DefaultTimeout = 120 * time.Second

// ErrTimedOut is wrapped by the error of a call that ran past its Timeout.
var ErrTimedOut = errors.New("timed out")

func (c Ctl) args(argv ...string) []string {
	out := []string{"simctl"}
	if c.Set != "" {
		out = append(out, "--set", c.Set)
	}
	return append(out, argv...)
}

// calls in flight, so a signal handler can stop them (Interrupt) before it cleans up
var live struct {
	sync.Mutex
	calls    map[*exec.Cmd]chan struct{} // closed when the call has exited
	consoles map[*exec.Cmd]bool          // LaunchConsole commands (their caller waits on them) until Forget
	stopping bool
}

// Interrupt sends SIGINT to every simctl call in flight, and waits up to wait for them to exit. That is what a
// terminal's Ctrl-C does to the whole process group, and an interrupted `simctl boot` stops booting.
// After it, only `shutdown` and `list` calls run. Any other call fails at once, so nothing new, such as a boot or a
// launch, starts behind a cleanup.
// It is needed because a SIGTERM sent to stuntcam alone reaches no child. Without it, a `simctl boot` that had just
// started went on to boot the device after the handler had already shut it down.
func Interrupt(wait time.Duration) {
	live.Lock()
	live.stopping = true
	var done []chan struct{}
	for cmd, exited := range live.calls {
		if cmd.Process != nil {
			_ = cmd.Process.Signal(syscall.SIGINT) // it may have exited already
		}

		done = append(done, exited)
	}

	for cmd := range live.consoles { // not waited for: the caller's cleanup ends them, or the device shutdown does
		_ = cmd.Process.Signal(syscall.SIGINT) // it may have exited already
	}

	live.Unlock()
	deadline := time.After(wait)
	for _, exited := range done {
		select {
		case <-exited:
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
func (c Ctl) Run(env []string, argv ...string) (string, error) {
	timeout := c.Timeout
	if timeout == 0 {
		timeout = DefaultTimeout
	}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	cmd := exec.CommandContext(ctx, "xcrun", c.args(argv...)...)
	cmd.Env = append(os.Environ(), env...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	live.Lock()
	if live.stopping && (len(argv) == 0 || (argv[0] != "shutdown" && argv[0] != "list")) {
		live.Unlock()
		return "", fmt.Errorf("simctl %s: interrupted", strings.Join(argv, " "))
	}

	if err := cmd.Start(); err != nil {
		live.Unlock()
		return "", fmt.Errorf("simctl %s: %w", strings.Join(argv, " "), err)
	}

	exited := make(chan struct{})
	if live.calls == nil {
		live.calls = map[*exec.Cmd]chan struct{}{}
	}

	live.calls[cmd] = exited
	live.Unlock()
	err := cmd.Wait()
	live.Lock()
	delete(live.calls, cmd)
	close(exited)
	live.Unlock()
	if ctx.Err() == context.DeadlineExceeded {
		return stdout.String(), fmt.Errorf("simctl %s: %w after %s", strings.Join(argv, " "), ErrTimedOut, timeout)
	}

	if err != nil {
		return stdout.String(), fmt.Errorf("simctl %s: %w: %s", strings.Join(argv, " "), err,
			strings.TrimSpace(stderr.String()))
	}

	return stdout.String(), nil
}

func (c Ctl) Devices() ([]Device, error) {
	out, err := c.Run(nil, "list", "devices", "-j")
	if err != nil {
		return nil, err
	}
	return ParseDevices([]byte(out))
}

func ParseDevices(data []byte) ([]Device, error) {
	var parsed struct {
		Devices map[string][]struct {
			UDID      string `json:"udid"`
			Name      string `json:"name"`
			State     State  `json:"state"`
			Available bool   `json:"isAvailable"`
			LastBoot  string `json:"lastBootedAt"`
			LastUsed  string `json:"lastUsedAt"` // Xcode 27 reports only this; it is set when the device boots
		} `json:"devices"`
	}

	if err := json.Unmarshal(data, &parsed); err != nil {
		return nil, err
	}

	var out []Device
	for runtimeID, devices := range parsed.Devices {
		for _, device := range devices {
			lastBooted, _ := time.Parse(time.RFC3339, device.LastBoot) // missing or unparsable: zero, unknown
			if lastBooted.IsZero() {
				lastBooted, _ = time.Parse(time.RFC3339, device.LastUsed)
			}

			out = append(out, Device{UDID: device.UDID, Name: device.Name, State: device.State, RuntimeID: runtimeID,
				Available: device.Available, LastBooted: lastBooted})
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

func ParseRuntimes(data []byte) ([]Runtime, error) {
	var parsed struct {
		Runtimes []Runtime `json:"runtimes"`
	}
	if err := json.Unmarshal(data, &parsed); err != nil {
		return nil, err
	}
	return parsed.Runtimes, nil
}

func (c Ctl) DeviceTypes() ([]DeviceType, error) {
	out, err := c.Run(nil, "list", "devicetypes", "-j")
	if err != nil {
		return nil, err
	}

	var parsed struct {
		DeviceTypes []DeviceType `json:"devicetypes"`
	}

	// not in the return statement: its operands are evaluated before the call
	err = json.Unmarshal([]byte(out), &parsed)
	return parsed.DeviceTypes, err
}

// NewestIOS picks the newest available iOS runtime, or the one matching want (a version like "26.3", or an identifier).
func NewestIOS(runtimes []Runtime, want string) (Runtime, bool) {
	var matches []Runtime
	for _, runtime := range runtimes {
		if !runtime.Available || runtime.Platform != "iOS" {
			continue
		}

		if want == "" || runtime.Identifier == want || runtime.Version == want ||
			strings.HasPrefix(runtime.Version, want+".") {
			matches = append(matches, runtime)
		}
	}

	if len(matches) == 0 {
		return Runtime{}, false
	}

	sort.Slice(matches, func(i, j int) bool { return versionLess(matches[i].Version, matches[j].Version) })
	return matches[len(matches)-1], true
}

func versionLess(left, right string) bool {
	leftParts, rightParts := strings.Split(left, "."), strings.Split(right, ".")
	for i := 0; i < len(leftParts) || i < len(rightParts); i++ {
		var leftNum, rightNum int
		if i < len(leftParts) {
			leftNum, _ = strconv.Atoi(leftParts[i]) // a part that is not a number counts as 0
		}

		if i < len(rightParts) {
			rightNum, _ = strconv.Atoi(rightParts[i]) // a part that is not a number counts as 0
		}

		if leftNum != rightNum {
			return leftNum < rightNum
		}
	}

	return false
}

// Boot boots with a boot environment: each "K=V" is passed as SIMCTL_CHILD_K=V, which every process of that boot
// inherits.
func (c Ctl) Boot(udid string, bootEnv []string) error {
	env := make([]string, 0, len(bootEnv))
	for _, kv := range bootEnv {
		env = append(env, "SIMCTL_CHILD_"+kv)
	}
	_, err := c.Run(env, "boot", udid)
	return err
}

// Shutdown shuts the device down.
func (c Ctl) Shutdown(udid string) error {
	_, err := c.Run(nil, "shutdown", udid)
	return err
}

// WaitBooted runs `bootstatus -b` with a bound (macOS has no timeout(1)).
func (c Ctl) WaitBooted(udid string, bound time.Duration) error {
	c.Timeout = bound
	_, err := c.Run(nil, "bootstatus", udid, "-b")
	return err
}

// Getenv reads a variable of the booted device's launchd environment.
func (c Ctl) Getenv(udid, key string) (string, error) {
	out, err := c.Run(nil, "spawn", udid, "launchctl", "getenv", key)
	return strings.TrimRight(out, "\n"), err
}

// Setenv sets a variable in the booted device's launchd environment.
func (c Ctl) Setenv(udid, key, value string) error {
	_, err := c.Run(nil, "spawn", udid, "launchctl", "setenv", key, value)
	return err
}

// Unsetenv removes a variable from the booted device's launchd environment.
func (c Ctl) Unsetenv(udid, key string) error {
	_, err := c.Run(nil, "spawn", udid, "launchctl", "unsetenv", key)
	return err
}

// Install installs an app bundle on the booted device.
func (c Ctl) Install(udid, app string) error {
	_, err := c.Run(nil, "install", udid, app)
	return err
}

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
func (c Ctl) Spawn(udid string, argv ...string) (string, error) {
	return c.Run(nil, append([]string{"spawn", udid}, argv...)...)
}

// AppContainer returns an installed app's container path (kind: app, data, groups or an app group id). simctl answers
// only for a booted device.
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
	// only simctl's own "no such app" error; any other ENOENT text is not proof the app is gone
	return strings.Contains(s, "domain=NSPOSIXErrorDomain, code=2")
}

// NotBooted says whether a simctl error means the device isn't booted ("Unable to lookup in current state: Shutdown").
func NotBooted(err error) bool {
	return err != nil && strings.Contains(err.Error(), "current state: Shutdown")
}

// TimedOut says whether a call timed out: it ran past its Timeout, or simctl itself reported a timeout. The substring
// test alone would catch both today (ErrTimedOut's text is "timed out"); errors.Is keeps the first case if that text
// ever changes.
func TimedOut(err error) bool {
	return errors.Is(err, ErrTimedOut) || (err != nil && strings.Contains(err.Error(), "timed out"))
}

// Launch starts an installed app on the booted device.
func (c Ctl) Launch(udid, bundle string) error {
	_, err := c.Run(nil, "launch", udid, bundle)
	return err
}

// Terminate stops a running app on the booted device.
func (c Ctl) Terminate(udid, bundle string) error {
	_, err := c.Run(nil, "terminate", udid, bundle)
	return err
}

// LaunchConsole starts an app with its stdout and stderr attached (simctl launch --console-pty). The caller reads
// Stdout and must Wait or Kill the returned command.
// The command is `xcrun simctl …` itself, so a signal sent to it reaches simctl.
// Like Run's calls, it is refused after Interrupt, and Interrupt signals it. Call Forget once it has exited.
func (c Ctl) LaunchConsole(udid, bundle string, env []string) (*exec.Cmd, *bytes.Buffer, error) {
	cmd := exec.Command("xcrun", c.args("launch", "--console-pty", udid, bundle)...)
	cmd.Env = append(os.Environ(), env...)
	var output bytes.Buffer
	cmd.Stdout, cmd.Stderr = &lockedWriter{b: &output}, &lockedWriter{b: &output}
	live.Lock()
	defer live.Unlock()

	if live.stopping {
		return cmd, &output, fmt.Errorf("simctl launch: interrupted")
	}

	if err := cmd.Start(); err != nil {
		return cmd, &output, err
	}

	if live.consoles == nil {
		live.consoles = map[*exec.Cmd]bool{}
	}

	live.consoles[cmd] = true
	return cmd, &output, nil
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
func Snapshot(b *bytes.Buffer) string {
	bufMu.Lock()
	defer bufMu.Unlock()
	return b.String()
}
