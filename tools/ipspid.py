# SPDX-License-Identifier: Apache-2.0
# ipspid.py <proc> <since> <pid> ...: finds the crash reports of process <proc> written for one of the given pids at or
# after <since> (epoch seconds). It prints "<pid><TAB><path>" for each match. The separator is a tab because paths may
# hold spaces; scripts/activation/helpers.sh reads the output with awk -F'\t'.
#
# It looks in ~/Library/Logs/DiagnosticReports and its Retired folder, because reports move there within minutes. It
# matches by pid, not by count or modification time, so old reports whose modification time changed and other agents'
# crashes of the same app never count. The report's capture time (the body's "captureTime", or the file's modification
# time if that is missing) must be at or after <since>, so a reused pid can't match an older report.
import datetime
import glob
import json
import os
import re
import sys

REPORT_DIRS = ('~/Library/Logs/DiagnosticReports', '~/Library/Logs/DiagnosticReports/Retired')
PID_FIELD = re.compile(r'"pid" : (\d+),')
CAPTURE_TIME_FIELD = re.compile(r'"captureTime" : "([^"]+)"')
# For example "2026-11-13 18:45:38.1512 -0500", with or without the fraction of a second.
CAPTURE_TIME_FORMATS = ('%Y-%m-%d %H:%M:%S.%f %z', '%Y-%m-%d %H:%M:%S %z')
USAGE = 'usage: ipspid.py <proc> <since> <pid> ...'


def usage():
    """Prints the usage line on stderr and exits 2."""
    print(USAGE, file=sys.stderr)
    sys.exit(2)


def capture_time(raw, path):
    """Returns the capture time in epoch seconds, or the file's modification time if the report has no readable one."""
    match = CAPTURE_TIME_FIELD.search(raw)
    if match:
        for time_format in CAPTURE_TIME_FORMATS:
            try:
                return datetime.datetime.strptime(match.group(1), time_format).timestamp()
            except ValueError:
                pass

    return os.path.getmtime(path)


def report_pid(raw):
    """Returns the pid the report was written for, as text."""
    match = PID_FIELD.search(raw)
    if match:
        return match.group(1)

    body = raw.split('\n', 1)[1]
    return str(json.loads(body).get('pid'))


def main():
    if len(sys.argv) < 3:
        usage()

    process = sys.argv[1]
    try:
        since = float(sys.argv[2])
    except ValueError:
        usage()

    pids = set(sys.argv[3:])
    for report_dir in REPORT_DIRS:
        for path in glob.glob(os.path.expanduser(report_dir) + '/' + glob.escape(process) + '*.ips'):
            # Skip reports that can't be read or parsed.
            try:
                raw = open(path, errors='replace').read()
                pid = report_pid(raw)
                captured = capture_time(raw, path)
            except Exception:
                continue

            if pid in pids and captured >= since:
                print(pid, path, sep='\t')


if __name__ == '__main__':
    main()
