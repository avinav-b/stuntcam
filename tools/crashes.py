# SPDX-License-Identifier: Apache-2.0
# crashes.py <since-epoch>: lists the crash reports (.ips files in DiagnosticReports and its Retired folder) that are
# newer than <since-epoch> and come from processes running in simulators.
#
# Our simulators are the UDIDs in the $SIMCAM_UDID_<label> variables (for example SIMCAM_UDID_263 or SIMCAM_UDID_ci).
# A report is ours if one of those UDIDs appears anywhere in it (in image paths, the process path or the environment):
# apps and their frameworks carry the device's data path.
#
# Simulator daemons (notifyd, syslogd, and so on) run from the runtime root and often carry no UDID, so matching on
# UDIDs alone misses them. Those that clearly come from a simulator (their parent is launchd_sim, or a path is inside a
# simulator runtime) are listed with the label "sim?", so they are never silently dropped. A "sim?" report can come
# from any simulator on this host.
#
# Each row says whether our loader or engine was loaded in the process (engine=yes or engine=no). A report labelled
# with our UDID can be any process on our simulators, including system daemons; engine=yes narrows it to apps that
# stuntcam was loaded in. The boot stub (libsimcamstub.dylib) is in every process of a stub-booted simulator, so it
# doesn't count.
import glob
import json
import os
import re
import sys

UDID_VARIABLE_PREFIX = 'SIMCAM_UDID_'
REPORT_DIRS = [
    os.path.expanduser('~/Library/Logs/DiagnosticReports'),
    os.path.expanduser('~/Library/Logs/DiagnosticReports/Retired'),
]
SIMULATOR_HINT = re.compile(r'launchd_sim|/CoreSimulator/Volumes/|\.simruntime/|/RuntimeRoot/')
# How much of a report to search for "launchd_sim" when the process and parent paths don't show a simulator.
SIMULATOR_HINT_SEARCH_BYTES = 20000
# How many frames of the faulting thread each row shows.
TOP_FRAMES = 6
# The sanitizer builds of the engine and the loader use the same image names.
ENGINE_IMAGES = ('libsimcam.dylib', 'libsimcamloader.dylib')
USAGE = 'usage: crashes.py <since-epoch>'


def usage():
    """Prints the usage line on stderr and exits 2."""
    print(USAGE, file=sys.stderr)
    sys.exit(2)


def fail(message):
    """Prints "crashes.py: <message>" on stderr and exits 1."""
    print(f'crashes.py: {message}', file=sys.stderr)
    sys.exit(1)


def our_udids():
    """Maps each UDID in a $SIMCAM_UDID_<label> variable to its label."""
    udids = {}
    for name, value in os.environ.items():
        if name.startswith(UDID_VARIABLE_PREFIX) and value.strip():
            udids[value.strip()] = name[len(UDID_VARIABLE_PREFIX):]

    return udids


def looks_like_simulator(raw, body):
    """Tells whether a report without one of our UDIDs clearly comes from a simulator process."""
    paths = (body.get('procPath') or '') + ' ' + (body.get('parentProc') or '')
    return bool(SIMULATOR_HINT.search(paths) or 'launchd_sim' in raw[:SIMULATOR_HINT_SEARCH_BYTES])


def report_row(path, label, header, body):
    """Returns one row: (label, file name, process, engine=yes|no, exception type, top frames)."""
    images = body.get('usedImages') or []
    threads = body.get('threads') or []
    faulting_index = body.get('faultingThread', 0)
    if isinstance(faulting_index, int) and 0 <= faulting_index < len(threads):
        faulting_thread = threads[faulting_index]
    else:
        faulting_thread = {'frames': []}

    def image_name(index):
        if isinstance(index, int) and 0 <= index < len(images):
            return images[index].get('name') or '?'

        return '?'

    frames = (faulting_thread.get('frames') or [])[:TOP_FRAMES]
    top = [f"{image_name(frame.get('imageIndex'))}:{frame.get('symbol')}" for frame in frames]
    engine_loaded = any((image.get('name') or '') in ENGINE_IMAGES for image in images)
    process = (body.get('bundleInfo') or {}).get('CFBundleIdentifier') or body.get('procName') or header.get('name')
    process = process or '?'
    exception_type = (body.get('exception') or {}).get('type') or '?'
    engine = 'engine=yes' if engine_loaded else 'engine=no'
    return (label, os.path.basename(path), process, engine, exception_type, top)


def main():
    if len(sys.argv) < 2:
        usage()

    try:
        since = float(sys.argv[1])
    except ValueError:
        usage()

    udids = our_udids()
    rows = []
    for report_dir in REPORT_DIRS:
        for path in glob.glob(report_dir + '/*.ips'):
            if os.path.getmtime(path) < since:
                continue

            # A report is a JSON header line followed by a JSON body. Skip any that can't be read.
            try:
                raw = open(path, errors='replace').read()
                header_text, body_text = raw.split('\n', 1)
                header = json.loads(header_text)
                body = json.loads(body_text)
            except Exception:
                continue

            label = next((udid_label for udid, udid_label in udids.items() if udid in raw), None)
            if not label:
                if not looks_like_simulator(raw, body):
                    continue

                label = 'sim?'

            rows.append(report_row(path, label, header, body))

    for row in sorted(rows, key=lambda row: (row[0], row[1])):
        print(row)

    ours = sum(1 for row in rows if row[0] != 'sim?')
    unattributed = sum(1 for row in rows if row[0] == 'sim?')
    engine_loaded = sum(1 for row in rows if row[3] == 'engine=yes')
    print('TOTAL', len(rows), '(ours:', ours, ', unattributed simulator processes:', unattributed,
          ', engine loaded:', engine_loaded, ')')


if __name__ == '__main__':
    main()
