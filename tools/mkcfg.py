# SPDX-License-Identifier: Apache-2.0
# mkcfg.py out.plist [base.plist]: writes an iPhone-like device profile with 420v and 420f pairs of each format and a
# 4:3 photo format with 12 MP stills.
#
# It keeps everything in base.plist except the formats list (and formatTemplate.PixelFormatType, which becomes the
# "$pf" placeholder). base.plist defaults to engine/profiles/cfg-iphone.plist, which this script reproduces unchanged.
import os
import plistlib
import sys
from xml.parsers.expat import ExpatError

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BASE = os.path.join(ROOT, 'engine/profiles/cfg-iphone.plist')
USAGE = 'usage: mkcfg.py out.plist [base.plist]'


# Core Video pixel formats, as their FourCC codes read as big-endian integers.
PIXEL_FORMAT_420V = 875704438  # '420v': kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
PIXEL_FORMAT_420F = 875704422  # '420f': kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
PIXEL_FORMATS = ((PIXEL_FORMAT_420V, '420v'), (PIXEL_FORMAT_420F, '420f'))
# The default format of a profile is the full-range one.
DEFAULT_PIXEL_FORMAT = PIXEL_FORMAT_420F

HD_PRESETS = [
    "AVCaptureSessionPresetHigh",
    "AVCaptureSessionPresetMedium",
    "AVCaptureSessionPresetLow",
    "AVCaptureSessionPreset1920x1080",
    "AVCaptureSessionPreset1280x720",
    "AVCaptureSessionPreset640x480",
    "AVCaptureSessionPreset352x288",
    "AVCaptureSessionPresetiFrame960x540",
    "AVCaptureSessionPresetiFrame1280x720",
    "AVCaptureSessionPresetInputPriority",
]

# One entry per format: (width, height, fps, session presets, is the default format, still image sizes).
FORMAT_SPECS = [
    (1920, 1440, 30, ["AVCaptureSessionPresetPhoto", "AVCaptureSessionPresetInputPriority"], False,
     [(4032, 3024), (1920, 1440)]),
    (1920, 1080, 30, HD_PRESETS, True, [(4032, 2268), (1920, 1080)]),
    (1920, 1080, 60, [], False, [(1920, 1080)]),
    (3840, 2160, 30, ["AVCaptureSessionPreset3840x2160"], False, [(3840, 2160)]),
    (3840, 2160, 60, [], False, [(3840, 2160)]),
]


def profile_formats():
    """Returns the profile's formats list: each format spec in 420v, then in 420f."""
    formats = []
    for width, height, fps, presets, is_default, stills in FORMAT_SPECS:
        for pixel_format, pixel_format_name in PIXEL_FORMATS:
            still_sizes = [{'Width': still_width, 'Height': still_height} for still_width, still_height in stills]
            formats.append({
                'w': width,
                'h': height,
                'fps': fps,
                'presets': presets,
                'def': is_default and pixel_format == DEFAULT_PIXEL_FORMAT,
                'pf': pixel_format,
                'name': f'{width}x{height}@{fps}-{pixel_format_name}',
                'extra': {'HighResStillImageDimensions': still_sizes},
            })

    return formats


def usage():
    """Prints the usage line on stderr and exits 2."""
    print(USAGE, file=sys.stderr)
    sys.exit(2)


def fail(message):
    """Prints "mkcfg.py: <message>" on stderr and exits 1."""
    print(f'mkcfg.py: {message}', file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        usage()

    base_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_BASE
    try:
        profile = plistlib.load(open(base_path, 'rb'))
    except OSError as error:
        fail(f'{base_path}: {error.strerror}')
    except (plistlib.InvalidFileException, ExpatError):
        fail(f'{base_path}: not a property list')

    profile['formatTemplate']['PixelFormatType'] = '$pf'
    formats = profile_formats()
    profile['formats'] = formats
    try:
        plistlib.dump(profile, open(sys.argv[1], 'wb'))
    except OSError as error:
        fail(f'{sys.argv[1]}: {error.strerror}')

    print(len(formats), 'formats')


if __name__ == '__main__':
    main()
