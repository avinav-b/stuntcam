# SPDX-License-Identifier: Apache-2.0
# crash.py <report.ips> [n]: prints one crash report's file name, exception and application-specific information
# (asi), then the first n frames (16 by default) of its triggered thread: image, symbol, and offset in the symbol.
import json
import sys

DEFAULT_FRAMES = 16
USAGE = 'usage: crash.py <report.ips> [n]'


def usage():
    """Prints the usage line on stderr and exits 2."""
    print(USAGE, file=sys.stderr)
    sys.exit(2)


def fail(message):
    """Prints "crash.py: <message>" on stderr and exits 1."""
    print(f'crash.py: {message}', file=sys.stderr)
    sys.exit(1)


def read_body(path):
    """Returns the report's JSON body (a report is a JSON header line followed by a JSON body; only the body is
    needed). An unreadable file, or one that isn't a report, ends the run with a message."""
    try:
        body = json.loads(open(path).read().split('\n', 1)[1])
    except OSError as error:
        fail(f'{path}: {error.strerror}')
    except (IndexError, ValueError):
        fail(f'{path}: not a crash report (a JSON header line, then a JSON body)')

    if not isinstance(body, dict):
        fail(f'{path}: not a crash report (a JSON header line, then a JSON body)')

    return body


def frame_limit():
    """Returns n, the number of frames to print (DEFAULT_FRAMES without one). A bad n prints the usage."""
    if len(sys.argv) <= 2:
        return DEFAULT_FRAMES

    try:
        return int(sys.argv[2])
    except ValueError:
        usage()


def print_triggered_frames(body, path):
    """Prints the first n frames of each triggered thread. A body without the threads, frames or images of a report
    ends the run with a message, after the lines printed so far."""
    try:
        images = body['usedImages']
        limit = None  # n is read at the first triggered thread: a report with none prints no usage for a bad n
        for thread in body['threads']:
            if not thread.get('triggered'):
                continue

            if limit is None:
                limit = frame_limit()

            for frame in thread['frames'][:limit]:
                image = images[frame['imageIndex']]
                print(' ', image.get('name'), frame.get('symbol'), frame.get('symbolLocation'))
    except (KeyError, IndexError, TypeError, AttributeError):
        fail(f'{path}: a thread, frame or image is not in the crash report\'s format')


def main():
    if len(sys.argv) < 2:
        usage()

    path = sys.argv[1]
    body = read_body(path)
    print(path.split('/')[-1], body.get('exception'), body.get('asi'))
    print_triggered_frames(body, path)


if __name__ == '__main__':
    main()
