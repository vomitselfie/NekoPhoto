#!/usr/bin/env python3
"""Writes a small synthetic DNG (an uncompressed RGGB Bayer mosaic) for the RAW tests.

The scene is a grey ramp above a row of colour patches, shot under a warm light: the sensor records neutral
grey as red 0.5, green 1, blue 0.7, and the file's AsShotNeutral says so, so a developer that honours the as-shot
white balance gets the grey back neutral. The camera space is linear sRGB (ColorMatrix1 is the XYZ to linear sRGB
matrix), so the colours come out as drawn.

    python3 tools/make_test_dng.py out.dng [width height]

Standard library only. The fixture under tests/fixtures/raw/ was made with the defaults.
"""
import struct
import sys

NEUTRAL = (0.5, 1.0, 0.7)
BLACK, WHITE = 64, 4095


def scene(x, y, w, h):
    """Linear scene colour at (x, y): a grey ramp on top, six patches below."""
    if y < h // 2:
        v = 0.02 + 0.9 * x / (w - 1)
        return (v, v, v)
    patches = [(0.6, 0.1, 0.1), (0.1, 0.5, 0.1), (0.1, 0.15, 0.6), (0.6, 0.55, 0.1), (0.18, 0.18, 0.18), (0.8, 0.8, 0.8)]
    return patches[min(len(patches) - 1, x * len(patches) // w)]


def mosaic(w, h):
    data = bytearray()
    for y in range(h):
        for x in range(w):
            channel = (0 if x % 2 == 0 else 1) if y % 2 == 0 else (1 if x % 2 == 0 else 2)   # RGGB
            value = scene(x, y, w, h)[channel] * NEUTRAL[channel]
            data += struct.pack("<H", BLACK + round(max(0.0, min(1.0, value)) * (WHITE - BLACK)))
    return bytes(data)


def rational(v, signed=False):
    den = 10000
    return (round(v * den), den)


def write(path, w=128, h=96):
    pixels = mosaic(w, h)
    xyz_to_srgb = [3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570]
    # (tag, type, values): types 1 BYTE, 2 ASCII, 3 SHORT, 4 LONG, 5 RATIONAL, 10 SRATIONAL
    tags = [
        (254, 4, [0]),
        (256, 4, [w]), (257, 4, [h]),
        (258, 3, [16]), (259, 3, [1]), (262, 3, [32803]),
        (271, 2, b"NekoPhoto\0"), (272, 2, b"Test Bayer\0"),
        (273, 4, [0]),   # strip offset, patched below
        (274, 3, [1]), (277, 3, [1]), (278, 4, [h]), (279, 4, [len(pixels)]), (284, 3, [1]),
        (33421, 3, [2, 2]), (33422, 1, [0, 1, 1, 2]),
        (50706, 1, [1, 4, 0, 0]), (50707, 1, [1, 1, 0, 0]),
        (50708, 2, b"NekoPhoto Test Bayer\0"),
        (50714, 4, [BLACK]), (50717, 4, [WHITE]),
        (50721, 10, [rational(v) for v in xyz_to_srgb]),
        (50728, 5, [rational(v) for v in NEUTRAL]),
        (50778, 3, [21]),   # CalibrationIlluminant1: D65
    ]
    tags.sort(key=lambda t: t[0])
    sizes = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 10: 8}
    ifd_offset = 8
    ifd_size = 2 + 12 * len(tags) + 4
    extra = bytearray()
    extra_base = ifd_offset + ifd_size
    entries = []
    strip_entry = None
    for tag, kind, values in tags:
        if kind == 2:
            raw = bytes(values)
            count = len(raw)
        elif kind in (5, 10):
            raw = b"".join(struct.pack("<ii" if kind == 10 else "<II", n, d) for n, d in values)
            count = len(values)
        else:
            fmt = {1: "B", 3: "H", 4: "I"}[kind]
            raw = struct.pack("<" + fmt * len(values), *values)
            count = len(values)
        if len(raw) <= 4:
            entries.append([tag, kind, count, raw.ljust(4, b"\0"), None])
        else:
            entries.append([tag, kind, count, struct.pack("<I", extra_base + len(extra)), None])
            extra += raw
            if len(extra) % 2:
                extra += b"\0"
        if tag == 273:
            strip_entry = entries[-1]
    strip_offset = extra_base + len(extra)
    strip_entry[3] = struct.pack("<I", strip_offset)
    out = bytearray(b"II*\0" + struct.pack("<I", ifd_offset))
    out += struct.pack("<H", len(tags))
    for tag, kind, count, value, _ in entries:
        out += struct.pack("<HHI", tag, kind, count) + value
    out += struct.pack("<I", 0)
    out += extra
    out += pixels
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    write(sys.argv[1], *(int(v) for v in sys.argv[2:4])) if len(sys.argv) == 4 else write(sys.argv[1])
