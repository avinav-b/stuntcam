// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
)

// stageSource validates a source spec and copies an image or video where simulator processes can read it (not a
// privacy-protected folder), returning the spec to use.
func stageSource(spec string) (string, error) {
	if spec == "scene" {
		return spec, nil
	}
	kind, p, ok := strings.Cut(spec, ":")
	if !ok || (kind != "image" && kind != "video") || p == "" {
		return "", fmt.Errorf("source must be scene, image:PATH or video:PATH (got %q)", spec)
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", err
	}
	if fi, err := os.Stat(abs); err != nil || fi.IsDir() {
		return "", fmt.Errorf("%s: no such file", abs)
	}
	staged, err := artifacts.StageData(abs)
	if err != nil {
		return "", fmt.Errorf("staging %s: %w", abs, err)
	}
	return kind + ":" + staged, nil
}

const sourceUsage = "usage: stuntcam source [--udid U] [--front NAME] [--back NAME] " +
	"scene|image:PATH|video:PATH|webcam[:NAME]"

func cmdSource(args []string) error {
	fs := flag.NewFlagSet("source", flag.ContinueOnError)
	udid := fs.String("udid", "", "simulator UDID")
	front := fs.String("front", "", "webcam: the camera for the front position (localized name or uniqueID)")
	back := fs.String("back", "", "webcam: the camera for the back position (localized name or uniqueID)")
	if err := parse(fs, args, 1); err != nil {
		return err
	}
	if fs.NArg() != 1 {
		return usagef(sourceUsage)
	}
	a := fs.Arg(0)
	wcName, wcOn := webcamSpec(a)
	if a != "scene" && !strings.HasPrefix(a, "image:") && !strings.HasPrefix(a, "video:") && !wcOn {
		return usagef(sourceUsage)
	}
	if !wcOn && (*front != "" || *back != "") {
		return usagef("--front and --back go with the webcam source")
	}
	d, _, err := pickSim(*udid)
	if err != nil {
		return err
	}
	if err := controllable(d, "source"); err != nil { // before copying the file into data/ or starting the helper
		return err
	}
	if wcOn {
		set, err := artifacts.Find("")
		if err != nil {
			return err
		}
		e := newWebcamEnv()
		id, err := startWebcam(e, set, wcName, *front, *back)
		if err != nil {
			return err
		}
		spec := "webcam:" + id
		if err := writeControl(d, "source", spec); err != nil {
			return err
		}
		if err := e.Switch(d.UDID, []string{id}, webcamInUse()); err != nil {
			fmt.Fprintf(os.Stderr, "warning: %v\n", err)
		}
		fmt.Printf("%s: source %s (running apps switch within a second; apps launched later start with it; the camera is on "+
			"only while an app has a capture session running)\n", d.Name, spec)
		return nil
	}
	spec, err := stageSource(a)
	if err != nil {
		return usageError{err}
	}
	if err := writeControl(d, "source", spec); err != nil {
		return err
	}
	fmt.Printf("%s: source %s (running apps switch within a second; apps launched later start with it)\n", d.Name, spec)
	return endWebcam(d.UDID) // this simulator's webcam session ends; the helper stops unless another simulator uses it
}
