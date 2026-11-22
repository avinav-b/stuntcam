// SPDX-License-Identifier: Apache-2.0

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// The qr source: `stuntcam source qr:TEXT` shows a QR code of TEXT, which the engine makes with Core Image's
// CIQRCodeGenerator (error correction M) inside each app. TEXT is everything after "qr:", byte for byte (scanners
// return strings and guess the charset of a byte-mode code, so text meant to be read back should be UTF-8).
//
// On the way to the engine (the control file's source= line, SIMCAM_SOURCE) the text is percent-encoded: every '%',
// space, control byte and byte from 0x7f up becomes %XX (uppercase hex), so the spec is one line of printable ASCII
// whatever the text holds. The engine decodes it in engine/src/providers/qr.c.

const qrPrefix = "qr:"

// qrMaxBytes is the most text a QR code holds at error correction M in byte mode (version 40), as CIQRCodeGenerator
// measured: longer text gets no code.
const qrMaxBytes = 2331

// qrSpec checks a qr:TEXT source and returns the spec the engine reads.
func qrSpec(text string) (string, error) {
	if text == "" {
		return "", fmt.Errorf("qr: needs text, e.g. qr:'https://example.com'")
	}
	if len(text) > qrMaxBytes {
		return "", fmt.Errorf("qr: the text is %d bytes; a QR code holds at most %d (UTF-8 bytes, error correction M)",
			len(text), qrMaxBytes)
	}
	return qrPrefix + qrEscape(text), nil
}

// qrEscape percent-encodes '%', space, control bytes and bytes from 0x7f up.
func qrEscape(text string) string {
	var escaped strings.Builder
	for i := 0; i < len(text); i++ {
		char := text[i]
		if char == '%' || char <= ' ' || char >= 0x7f {
			fmt.Fprintf(&escaped, "%%%02X", char)
		} else {
			escaped.WriteByte(char)
		}
	}

	return escaped.String()
}

// qrUnescape is the engine's decoding: %XX (two hex digits, either case) is the byte XX; every other byte, a '%'
// without two hex digits after it included, stands for itself.
func qrUnescape(encoded string) string {
	var decoded strings.Builder
	for i := 0; i < len(encoded); i++ {
		if encoded[i] == '%' && i+2 < len(encoded) {
			if value, err := strconv.ParseUint(encoded[i+1:i+3], 16, 8); err == nil {
				decoded.WriteByte(byte(value))
				i += 2
				continue
			}
		}

		decoded.WriteByte(encoded[i])
	}

	return decoded.String()
}

// noteOtherBuild warns when the sim's apps load another build than this stuntcam's: an engine from before 1.2.0 doesn't
// know qr: and shows the scene instead (an upgrade without `stuntcam up` again).
func noteOtherBuild(device simctl.Device) {
	_, loader, err := home.ReadCurrent(device.UDID)
	if err != nil {
		return
	}

	set, err := artifacts.Find("")
	if err != nil {
		return
	}

	// a build dir not named by its content hash (STUNTCAM_BUILD=~/mybuild) is staged under one: no comparable name
	if !isContentHash(set.BuildID()) {
		return
	}

	if armedBuild := filepath.Base(filepath.Dir(loader)); armedBuild != set.BuildID() {
		fmt.Fprintf(os.Stderr, "note: %s runs build %s, not this stuntcam's %s; a build from before stuntcam 1.2.0 shows "+
			"the scene for qr: (`stuntcam up` again to switch builds)\n", device.Name, armedBuild, set.BuildID())
	}
}

// isContentHash says whether name is a build dir name made from its content: 12 lowercase hex digits.
func isContentHash(name string) bool {
	if len(name) != 12 {
		return false
	}
	for _, char := range name {
		if !strings.ContainsRune("0123456789abcdef", char) {
			return false
		}
	}
	return true
}

// typedSource is a spec the engine reads as it was typed: a qr spec with its text decoded, any other spec as it is.
func typedSource(spec string) string {
	if enc, ok := strings.CutPrefix(spec, qrPrefix); ok {
		return qrPrefix + qrUnescape(enc)
	}
	return spec
}

// sourceText is a typed source spec as the CLI prints it: qr:TEXT as qr:"TEXT" (Go-quoted, so newlines, spaces and
// invalid UTF-8 stay visible), any other spec as it is.
func sourceText(typed string) string {
	if text, ok := strings.CutPrefix(typed, qrPrefix); ok {
		return qrPrefix + strconv.Quote(text)
	}
	return typed
}
