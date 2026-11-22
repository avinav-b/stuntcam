#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# qr-check.py <app.log> <expected text file> <src,...> [--switch UNIX_MS FIRST_FILE] — checks the QRSEEN lines a
# fixture logged against the exact text, for scripts/qr-test.sh. The fixtures are fixtures/plain in qr mode,
# fixtures/expo as vcqr or expoqr, and fixtures/flutter in qr mode.
#
# It prints one line for each scanner (src):
#   <src> OK first=<ms>      the scanner reported the text first, <ms> after the session started
#   <src> WRONG got=<repr>   it reported something else first
#   <src> NONE               it reported nothing
#
# With --switch, the text was set while the app ran, by a `stuntcam source` started at UNIX_MS, after the app showed
# FIRST_FILE's text.
#   - The scanner must have reported FIRST_FILE's text before UNIX_MS. Otherwise the line is `<src> NOFIRST`.
#   - The OK line is `<src> OK latency=<ms>`, where <ms> runs from UNIX_MS to the first report of the new text. It
#     includes the command's own time.
#
# Exit 0 if every scanner is OK.
import re
import sys
import urllib.parse
from typing import NamedTuple, Optional

# One QRSEEN line: the scanner, ms since the session started, the Unix time in ms, and the text, percent-encoded.
LINE = re.compile(r'QRSEEN src=(\S+) t=(\d+) at=(\d+) pct=(\S*)')


class Report(NamedTuple):
    since_start_ms: int    # t=
    unix_ms: int           # at=
    text: Optional[str]    # None when the percent-encoding isn't valid UTF-8


def read_text(path):
    with open(path, encoding='utf-8', newline='') as f:
        return f.read()


def read_reports(log):
    """Every scanner's reports in the log, in order: {src: [Report, ...]}."""
    reports = {}
    with open(log, encoding='utf-8', errors='replace') as f:
        for match in LINE.finditer(f.read()):
            src = match.group(1)
            try:
                text = urllib.parse.unquote(match.group(4), errors='strict')
            except UnicodeDecodeError:
                text = None

            reports.setdefault(src, []).append(Report(int(match.group(2)), int(match.group(3)), text))

    return reports


def check_scanner(src, reports, want, switch_ms, first):
    """Prints the scanner's result line; returns whether it is OK."""
    if switch_ms is not None:
        saw_first = any(report.unix_ms < switch_ms and report.text == first for report in reports)
        if not saw_first:
            print(f'{src} NOFIRST (no report of the first text before the switch)')
            return False

        reports = [report for report in reports if report.unix_ms >= switch_ms]

    hit = next((report for report in reports if report.text == want), None)
    if hit and switch_ms is not None:
        print(f'{src} OK latency={hit.unix_ms - switch_ms}')
        return True

    if hit and reports[0].text == want:
        print(f'{src} OK first={hit.since_start_ms}')
        return True

    if hit:   # the right text, but something else came first
        print(f'{src} WRONG first got={reports[0].text!r:.80} (then the right text at {hit.since_start_ms} ms)')
        return False

    if reports:
        print(f'{src} WRONG got={reports[0].text!r:.120}')
        return False

    print(f'{src} NONE')
    return False


def main():
    args = sys.argv[1:]
    switch_ms = None
    first = None
    if '--switch' in args:
        i = args.index('--switch')
        switch_ms = int(args[i + 1])
        first = read_text(args[i + 2])
        del args[i:i + 3]

    log, expected_file, srcs = args[0], args[1], args[2].split(',')
    want = read_text(expected_file)
    reports = read_reports(log)
    results = [check_scanner(src, reports.get(src, []), want, switch_ms, first) for src in srcs]
    sys.exit(0 if all(results) else 1)


main()
