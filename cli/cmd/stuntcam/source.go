// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// stageSource validates a source spec and copies an image or video where simulator processes can read it (not a
// privacy-protected folder), returning the spec to use: for qr:TEXT, the text encoded for the engine (qr.go).
func stageSource(spec string) (string, error) {
	if spec == "scene" {
		return spec, nil
	}

	if text, ok := strings.CutPrefix(spec, qrPrefix); ok {
		return qrSpec(text)
	}

	kind, path, ok := strings.Cut(spec, ":")
	if !ok || (kind != "image" && kind != "video") || path == "" {
		return "", fmt.Errorf("source must be scene, image:PATH, video:PATH or qr:TEXT (got %q)", spec)
	}

	abs, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}

	if info, err := os.Stat(abs); err != nil || info.IsDir() {
		return "", fmt.Errorf("%s: no such file", abs)
	}

	staged, err := artifacts.StageData(abs)
	if err != nil {
		return "", fmt.Errorf("staging %s: %w", abs, err)
	}

	return kind + ":" + staged, nil
}

const sourceUsage = "usage: stuntcam source [--udid U] [--front NAME] [--back NAME] " +
	"scene|image:PATH|video:PATH|qr:TEXT|webcam[:NAME]"

func cmdSource(args []string) error {
	flags := flag.NewFlagSet("source", flag.ContinueOnError)
	udid := flags.String("udid", "", "simulator UDID")
	front := flags.String("front", "", "webcam: the camera for the front position (localized name or uniqueID)")
	back := flags.String("back", "", "webcam: the camera for the back position (localized name or uniqueID)")
	if err := parse(flags, args, 1); err != nil {
		return err
	}

	if flags.NArg() != 1 {
		return usagef(sourceUsage)
	}

	source := flags.Arg(0)
	webcamName, useWebcam := webcamSpec(source)
	kind, _, hasKind := strings.Cut(source, ":")
	if !hasKind {
		kind = ""
	}

	if source != "scene" && kind != "image" && kind != "video" && kind != "qr" && !useWebcam {
		return usagef(sourceUsage)
	}

	if !useWebcam && (*front != "" || *back != "") {
		return usagef("--front and --back go with the webcam source")
	}

	if kind == "qr" { // a bad text is a usage error before any simulator is looked at
		if _, err := stageSource(source); err != nil {
			return usageError{err}
		}
	}

	device, _, err := pickSim(*udid)
	if err != nil {
		return err
	}

	if err := controllable(device, "source"); err != nil { // before copying the file into data/ or starting the helper
		return err
	}

	if useWebcam {
		return switchToWebcam(device, webcamName, *front, *back)
	}

	spec, err := stageSource(source)
	if err != nil {
		return usageError{err}
	}

	if kind == "qr" {
		noteOtherBuild(device)
	}

	if err := writeControl(device, "source", spec); err != nil {
		return err
	}

	fmt.Printf("%s: source %s (running apps switch within a second; apps launched later start with it)\n", device.Name,
		sourceText(typedSource(spec)))

	// this simulator's webcam session ends; the helper stops unless another simulator uses it
	return endWebcam(device.UDID)
}

// switchToWebcam points the sim's apps at the Mac camera (name, or --front and --back), starting the camera helper
// first, and records the sim's webcam session.
func switchToWebcam(device simctl.Device, name, front, back string) error {
	set, err := artifacts.Find("")
	if err != nil {
		return err
	}

	webcamEnv := newWebcamEnv()
	webcamID, err := startWebcam(webcamEnv, set, name, front, back)
	if err != nil {
		return err
	}

	spec := "webcam:" + webcamID
	if err := writeControl(device, "source", spec); err != nil {
		return err
	}

	if err := webcamEnv.Switch(device.UDID, []string{webcamID}, webcamInUse()); err != nil {
		fmt.Fprintf(os.Stderr, "warning: %v\n", err)
	}

	fmt.Printf("%s: source %s (running apps switch within a second; apps launched later start with it; the camera "+
		"is on only while an app has a capture session running)\n", device.Name, spec)
	return nil
}
