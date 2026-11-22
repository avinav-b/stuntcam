#!/usr/bin/env python3 -I
# SPDX-License-Identifier: Apache-2.0
# orient.py <png> [expect]: finds the scene's red, green and blue top bar in a picture and reports how the picture is
# rotated or mirrored relative to upright.
#
# The result is the clockwise rotation of the content in degrees (0 means upright), followed by "m" if the picture is
# mirrored horizontally. An expected value has the same form, for example "0", "90" or "0m". With one, the script exits
# 1 if the picture does not match it. It exits 2 if it cannot find all three bar colours, cannot read the picture, or
# is run without one.
import sys

from PIL import Image

# The picture is scaled so that its longer side has this many pixels: enough to find the bar, and fast to scan.
SCAN_SIZE_PX = 400

USAGE = 'usage: orient.py <png> [expect]'

# A pixel belongs to a bar colour if that channel is bright and the other two are dark. Green has a wider margin
# for the dark channels than red and blue.
BRIGHT_MIN = 170
RED_DARK_MAX = 80
GREEN_DARK_MAX = 90
BLUE_DARK_MAX = 80

# Each colour needs at least MIN_BAR_PIXELS pixels, and at least 1/MIN_BAR_AREA_DIVISOR of all the pixels.
MIN_BAR_PIXELS = 20
MIN_BAR_AREA_DIVISOR = 4000

# The direction of "up" in the picture (an axis unit vector, y pointing down) for each clockwise rotation.
ROTATION_FOR_UP = {(0, -1): 0, (1, 0): 90, (0, 1): 180, (-1, 0): 270}


def bar_colour(red, green, blue):
    """Returns 'r', 'g' or 'b' if the pixel is one of the bar's colours, otherwise None."""
    if red > BRIGHT_MIN and green < RED_DARK_MAX and blue < RED_DARK_MAX:
        return 'r'

    if green > BRIGHT_MIN and red < GREEN_DARK_MAX and blue < GREEN_DARK_MAX:
        return 'g'

    if blue > BRIGHT_MIN and red < BLUE_DARK_MAX and green < BLUE_DARK_MAX:
        return 'b'

    return None


def main_axis(x, y):
    """Returns the axis unit vector closest to the direction (x, y). A tie goes to the x axis."""
    if abs(x) >= abs(y):
        return (1 if x > 0 else -1, 0)

    return (0, 1 if y > 0 else -1)


def centroid(points):
    """Returns the mean (x, y) of a list of points."""
    return (sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points))


def main():
    if len(sys.argv) < 2:
        print(USAGE, file=sys.stderr)
        sys.exit(2)

    try:
        picture = Image.open(sys.argv[1]).convert('RGB')
    except OSError as error:   # a missing file, or one Pillow can't decode
        print(f'orient.py: {sys.argv[1]}: {error.strerror or error}', file=sys.stderr)
        sys.exit(2)

    full_width, full_height = picture.size
    scale = SCAN_SIZE_PX / max(full_width, full_height)
    scaled_size = (max(1, int(full_width * scale)), max(1, int(full_height * scale)))
    picture = picture.resize(scaled_size, Image.NEAREST)
    width, height = picture.size

    pixels = picture.load()
    points = {'r': [], 'g': [], 'b': []}
    for y in range(height):
        for x in range(width):
            colour = bar_colour(*pixels[x, y])
            if colour:
                points[colour].append((x, y))

    counts = {colour: len(found) for colour, found in points.items()}
    if min(counts.values()) < max(MIN_BAR_PIXELS, width * height // MIN_BAR_AREA_DIVISOR):
        print(f"NOBAR size={full_width}x{full_height} counts={counts}")
        sys.exit(2)

    centres = {colour: centroid(found) for colour, found in points.items()}
    red, green, blue = centres['r'], centres['g'], centres['b']

    # The bar runs from red to blue, which is "right" in the scene.
    right_x = blue[0] - red[0]
    right_y = blue[1] - red[1]

    # The bar's centre relative to the picture's centre points "up" in the scene.
    up_x = (red[0] + green[0] + blue[0]) / 3 - width / 2
    up_y = (red[1] + green[1] + blue[1]) / 3 - height / 2
    right = main_axis(right_x, right_y)
    up = main_axis(up_x, up_y)
    if right[0] * up[0] + right[1] * up[1] != 0:
        # The bar's centre is not clear of the centre line, so "up" came out parallel to "right". Use the
        # perpendicular to "right" that points towards the bar instead.
        if right[1] * up_x - right[0] * up_y > 0:
            up = (right[1], -right[0])
        else:
            up = (-right[1], right[0])

    rotation = ROTATION_FOR_UP[up]

    # Upright and unmirrored, right = (1, 0) and up = (0, -1), so the cross product is -1.
    cross = right[0] * up[1] - right[1] * up[0]
    mirrored = cross > 0
    result = f"{rotation}{'m' if mirrored else ''}"

    expected = sys.argv[2] if len(sys.argv) > 2 else None
    matches = expected is None or expected == result
    expect_text = '' if expected is None else ' expect=' + expected
    print(f"{'PASS' if matches else 'FAIL'} orient={result}{expect_text} size={full_width}x{full_height}")
    sys.exit(0 if matches else 1)


if __name__ == '__main__':
    main()
