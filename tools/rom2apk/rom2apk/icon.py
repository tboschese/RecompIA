"""Turns a game frame into a full-bleed adaptive launcher icon layer."""

from __future__ import annotations

from collections import Counter
from typing import Tuple

from .png import Rows

# Adaptive icons are 108dp, of which only the central 72dp is guaranteed to be visible.
LAYER_SIZE = 432  # 108dp at xxxhdpi
SAFE_FRACTION = 72 / 108
MIN_ZOOM_FRACTION = 0.55  # never zoom into less than this share of the screen height


def compose(frame: Rows, aspect: float) -> Rows:
    height = len(frame)
    width = len(frame[0])
    x_scale = aspect * height / width  # display pixels per source pixel, horizontally
    display_width = width * x_scale

    background = _border_color(frame)
    x0, y0, x1, y1 = _content_box(frame, background)
    left, right = x0 * x_scale, (x1 + 1) * x_scale
    top, bottom = y0, y1 + 1

    # Zoom on the content (e.g. a title logo on a plain background) so the visible area is
    # always filled with art, even if the sides of a wide logo get cropped.
    side = min(right - left, bottom - top) * 1.08
    side = max(side, height * MIN_ZOOM_FRACTION)
    side = min(side, display_width, height)
    cx = _clamp((left + right) / 2, side / 2, display_width - side / 2)
    cy = _clamp((top + bottom) / 2, side / 2, height - side / 2)

    span = side / SAFE_FRACTION
    rows: Rows = []
    # Outside the safe zone the layer may extend past the frame; repeating the edge pixels
    # looks more natural there than a flat color.
    columns = [
        min(max(int((cx + ((i + 0.5) / LAYER_SIZE - 0.5) * span) / x_scale), 0), width - 1)
        for i in range(LAYER_SIZE)
    ]
    for j in range(LAYER_SIZE):
        sy = min(max(int(cy + ((j + 0.5) / LAYER_SIZE - 0.5) * span), 0), height - 1)
        source = frame[sy]
        rows.append([source[x] for x in columns])
    return rows


def _clamp(value: float, low: float, high: float) -> float:
    if low > high:
        return (low + high) / 2
    return max(low, min(value, high))


def _border_color(frame: Rows) -> Tuple[int, int, int]:
    border = frame[0] + frame[-1] + [row[0] for row in frame] + [row[-1] for row in frame]
    return Counter(border).most_common(1)[0][0]


def _content_box(frame: Rows, background: Tuple[int, int, int]) -> Tuple[int, int, int, int]:
    height = len(frame)
    width = len(frame[0])
    rows = [y for y in range(height) if any(pixel != background for pixel in frame[y])]
    if not rows:
        return 0, 0, width - 1, height - 1
    columns = [x for x in range(width) if any(frame[y][x] != background for y in rows)]
    return columns[0], rows[0], columns[-1], rows[-1]
