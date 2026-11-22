# SPDX-License-Identifier: Apache-2.0
# ips.py <report.ips> ...: prints a summary of each crash report: its app, launch time, exception type and signal,
# and termination indicator; the first loaded images whose names contain "simcam" or "fig"; and the faulting
# thread's first 18 frames (image, symbol, and offset in the image).
import json
import sys

OUR_IMAGE_HINTS = ('simcam', 'fig')
MAX_OUR_IMAGES = 3
MAX_FRAMES = 18


def fail(message):
    """Prints "ips.py: <message>" on stderr and exits 1."""
    print(f'ips.py: {message}', file=sys.stderr)
    sys.exit(1)


def is_our_image(image):
    """Tells whether a loaded image's name contains "simcam" or "fig"."""
    name = image.get('name') or ''
    return any(hint in name for hint in OUR_IMAGE_HINTS)


def summarize(path):
    """Prints the summary of one report."""

    # A report is a JSON header line followed by a JSON body.
    try:
        lines = open(path).read().split('\n', 1)
        header = json.loads(lines[0])
        body = json.loads(lines[1])
    except OSError as error:
        fail(f'{path}: {error.strerror}')
    except (IndexError, ValueError):
        fail(f'{path}: not a crash report (a JSON header line, then a JSON body)')
    if not isinstance(body, dict):
        fail(f'{path}: not a crash report (a JSON header line, then a JSON body)')

    # A body without the fields, threads, frames or images of a report ends the run with a message, after the lines
    # printed so far.
    try:
        exception = body.get('exception', {})
        print('==', path, header.get('app_name'), body.get('procLaunch'), exception.get('type'),
              exception.get('signal'), body.get('termination', {}).get('indicator'))
        images = body['usedImages']
        faulting_thread = body['threads'][body.get('faultingThread', 0)]
        print('  env DYLD:', [image.get('name') for image in images if is_our_image(image)][:MAX_OUR_IMAGES])
        for frame in faulting_thread['frames'][:MAX_FRAMES]:
            image = images[frame['imageIndex']]
            print('   ', image.get('name'), frame.get('symbol'), hex(frame.get('imageOffset', 0)))
    except (KeyError, IndexError, TypeError, AttributeError):
        fail(f'{path}: a field, thread, frame or image is not in the crash report\'s format')


def main():
    for path in sys.argv[1:]:
        summarize(path)


if __name__ == '__main__':
    main()
