// SPDX-License-Identifier: Apache-2.0
package main

import (
	"strings"
	"testing"
)

func TestQRSpec(t *testing.T) {
	cases := []struct {
		name, text, want, err string
	}{
		{name: "url", text: "https://example.com/pay?id=42", want: "qr:https://example.com/pay?id=42"},
		{name: "wifi", text: `WIFI:S:my net;T:WPA;P:p@ss:w;rd\;"x"%20;;`,
			want: `qr:WIFI:S:my%20net;T:WPA;P:p@ss:w;rd\;"x"%2520;;`},
		{name: "unicode", text: "Grüße 日本", want: "qr:Gr%C3%BC%C3%9Fe%20%E6%97%A5%E6%9C%AC"},
		{name: "newlines and tabs", text: "a\nb\r\tc", want: "qr:a%0Ab%0D%09c"},
		{name: "spaces at both ends", text: "  x  ", want: "qr:%20%20x%20%20"},
		{name: "binary", text: "\x00\x7f\xff=", want: "qr:%00%7F%FF="},
		{name: "longest", text: strings.Repeat("a", qrMaxBytes), want: "qr:" + strings.Repeat("a", qrMaxBytes)},
		{name: "empty", text: "", err: "qr: needs text"},
		{name: "too long", text: strings.Repeat("a", qrMaxBytes+1),
			err: "qr: the text is 2332 bytes; a QR code holds at most 2331"},
		// the limit counts UTF-8 bytes: 778 three-byte characters are 2334 bytes
		{name: "too long in bytes", text: strings.Repeat("日", 778), err: "qr: the text is 2334 bytes"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, err := qrSpec(c.text)
			if c.err != "" {
				if err == nil || !strings.HasPrefix(err.Error(), c.err) {
					t.Fatalf("qrSpec: got %q, %v; want error %q", got, err, c.err)
				}
				return
			}
			if err != nil || got != c.want {
				t.Fatalf("qrSpec: got %q, %v; want %q", got, err, c.want)
			}
			// one line of printable ASCII: what the control file and SIMCAM_SOURCE can carry
			for i := 0; i < len(got); i++ {
				if got[i] <= ' ' || got[i] >= 0x7f {
					t.Fatalf("byte %#x at %d in %q", got[i], i, got)
				}
			}
			if back := typedSource(got); back != qrPrefix+c.text {
				t.Fatalf("typedSource: got %q, want %q", back, qrPrefix+c.text)
			}
		})
	}
}

func TestQREscapeRoundTripsEveryByte(t *testing.T) {
	b := make([]byte, 256)
	for i := range b {
		b[i] = byte(i)
	}
	if got := qrUnescape(qrEscape(string(b))); got != string(b) {
		t.Fatalf("round trip lost bytes: %q", got)
	}
}

// the same cases as engine/src/providers/qr_test.c: the CLI's reading of a spec is the engine's
func TestQRUnescapeMatchesEngine(t *testing.T) {
	for in, want := range map[string]string{
		"https://example.com/pay?id=42": "https://example.com/pay?id=42",
		"50%25%20off":                   "50% off",
		"a%0Ab%0d%0A":                   "a\nb\r\n",
		"%E6%97%A5%e6%9c%ac":            "日本",
		"%00x%FF":                       "\x00x\xff",
		"50%":                           "50%",
		"50%2":                          "50%2",
		"%zz%2":                         "%zz%2",
		"%%41":                          "%A",
		"%4":                            "%4",
		"%+1":                           "%+1",
	} {
		if got := qrUnescape(in); got != want {
			t.Errorf("qrUnescape(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestSourceText(t *testing.T) {
	for typed, want := range map[string]string{
		"scene":                    "scene",
		"image:/a b/pic.png":       "image:/a b/pic.png",
		"webcam:0x1234":            "webcam:0x1234",
		"qr:https://example.com":   `qr:"https://example.com"`,
		"qr:line 1\nline 2\t\xff ": `qr:"line 1\nline 2\t\xff "`,
		"qr:日本":                    `qr:"日本"`,
	} {
		if got := sourceText(typed); got != want {
			t.Errorf("sourceText(%q) = %s, want %s", typed, got, want)
		}
	}
}

func TestIsContentHash(t *testing.T) {
	for s, want := range map[string]bool{
		"0123456789ab": true, "aaaaaaaaaaaa": true,
		"mybuild": false, "0123456789AB": false, "0123456789abc": false, "0123456789a": false, "": false,
	} {
		if got := isContentHash(s); got != want {
			t.Errorf("isContentHash(%q) = %v, want %v", s, got, want)
		}
	}
}
