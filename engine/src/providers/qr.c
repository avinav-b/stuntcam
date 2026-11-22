// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit; included before providers/scene.inc): the plain-C parts of the `qr:`
// source, kept free of Objective-C so engine/src/providers/qr_test.sh can run them on any host.
//
// The spec is "qr:" + the code's text, percent-encoded: `stuntcam source qr:TEXT` writes every '%', space, control
// byte and non-ASCII byte as %XX, so the spec is one line of printable ASCII in the control file (which the engine
// reads line by line, trimmed, as UTF-8) and in SIMCAM_SOURCE, whatever bytes the text holds (newlines, leading
// spaces, invalid UTF-8).
#include <stddef.h>

// the share of the canvas's shorter side the code may take: what an aspect-fill preview on a phone screen still shows
// (a 9:16 frame on a 9:19.5 screen keeps 82% of its width), with a margin
#define kQRFill 0.75

static int QRHexValue(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// QRSpecDecode decodes the n bytes of a spec's text (what follows "qr:") into out, which has room for n bytes, and
// returns the decoded length. %XX (two hex digits, either case) is the byte XX; every other byte stands for itself,
// a '%' without two hex digits after it included, so a hand-written SIMCAM_SOURCE=qr:50% works.
static size_t QRSpecDecode(const char *in, size_t n, unsigned char *out) {
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        int hi, lo;
        if (in[i] == '%' && i + 2 < n && (hi = QRHexValue((unsigned char)in[i + 1])) >= 0 &&
            (lo = QRHexValue((unsigned char)in[i + 2])) >= 0) {
            out[len++] = (unsigned char)(hi << 4 | lo);
            i += 2;
        } else {
            out[len++] = (unsigned char)in[i];
        }
    }
    return len;
}

// QRModuleScale is the whole number of canvas pixels per module for a code image w pixels wide (one pixel per module,
// as CIQRCodeGenerator makes it) on a cw x ch canvas: the largest that keeps the code within kQRFill of the shorter
// side, and at least 1. Whole pixels keep every module edge sharp in the source buffer.
static int QRModuleScale(int w, double cw, double ch) {
    double side = (cw < ch ? cw : ch) * kQRFill;
    int k = w > 0 ? (int)(side / w) : 1;
    return k < 1 ? 1 : k;
}
