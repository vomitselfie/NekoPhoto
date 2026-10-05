#!/usr/bin/env python3
"""Writes the synthetic Procreate brushes in this folder: a baseline and copies of it with one setting changed.

They test NekoPhoto's reader and brush engine, not what Procreate itself does with each setting: the brushes made
in Procreate on an iPad (pc_00 and on) are the reference for that. Each file is a .brush (a ZIP of one brush
folder) holding Brush.archive, an NSKeyedArchiver binary plist of the settings, and Shape.png, an ellipse twice as
wide as it is tall so a turn of the tip shows; the grain fixtures add Grain.png. The output is byte for byte the
same on every run.

    tests/fixtures/brushes/procreate/make_fixtures.py
"""
import os
import plistlib
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

BASELINE = {
    "plotSpacing": 0.1,
    "maxSize": 0.12,        # 24 pixels
    "maxOpacity": 0.12,
    "shapeRoundness": 1.0,
}

class Curve:
    """A ValkyrieMagnitudinalCurve: its points as "{x, y}" strings in an NSArray, in the order given."""
    def __init__(self, points):
        self.points = points


# name: (settings changed from the baseline, grain)
FIXTURES = {
    "syn_00_baseline": ({}, False),
    "syn_01_speed_size_pos": ({"dynamicsSpeedSize": 0.8}, False),
    "syn_02_speed_size_neg": ({"dynamicsSpeedSize": -0.8}, False),
    "syn_03_speed_opacity_pos": ({"dynamicsSpeedOpacity": 0.8}, False),
    "syn_04_speed_opacity_neg": ({"dynamicsSpeedOpacity": -0.8}, False),
    "syn_05_speed_spacing": ({"plotSpacingSpeed": 1.0}, False),
    "syn_06_tilt_size": ({"dynamicsTiltSize": 0.8}, False),
    "syn_07_tilt_opacity": ({"dynamicsTiltOpacity": 0.8}, False),
    "syn_08_tilt_angle_30": ({"dynamicsTiltSize": 0.8, "sizeTiltAngle": 30 / 90}, False),
    "syn_09_tilt_bleed": ({"dynamicsTiltBleed": 0.8}, False),
    "syn_10_tilt_roundness": ({"dynamicsTiltShapeRoundness": 1.0, "dynamicsTiltShapeRoundnessMinimum": 0.2}, False),
    "syn_11_azimuth": ({"shapeAzimuth": True}, False),
    "syn_12_roll": ({"shapeRoll": True}, False),
    "syn_13_grain_moving": ({"textureApplication": 0, "textureMovement": 1.0, "grainDepth": 1.0, "textureScale": 1.0}, True),
    "syn_14_grain_texturized": ({"textureApplication": 1, "grainDepth": 1.0, "textureScale": 1.0}, True),
    # Pressure on size through a curve that rises fast (half the size at a quarter of the pressure), as most of the
    # pressure curves in real brush sets do.
    "syn_15_pressure_curve": ({"dynamicsPressureSize": 1.0,
                               "dynamicsPressureSizeCurve": Curve([(0, 0), (1, 1), (0.25, 0.5)])}, False),
    # The pencil's taper: a third of the slider at each end, down to nothing in size; the touch taper only at the start.
    "syn_16_taper": ({"pencilTaperStartLength": 1 / 3, "pencilTaperEndLength": 1 / 3, "pencilTaperSize": 1.0,
                      "taperStartLength": 0.25, "taperSize": 1.0, "taperOpacity": 1.0}, False),
}



def keyed_archive(settings):
    """One root object with the settings as its keys, as Procreate's Brush.archive stores them."""
    root = {"$class": plistlib.UID(3)}
    objects = ["$null", root, settings.get("name", ""), {"$classname": "SilicaBrush", "$classes": ["SilicaBrush", "NSObject"]}]
    for key in sorted(settings):
        value = settings[key]
        if isinstance(value, Curve):
            strings = []
            for x, y in value.points:
                objects.append("{%f, %f}" % (x, y))
                strings.append(plistlib.UID(len(objects) - 1))
            objects.append({"$classname": "NSArray", "$classes": ["NSArray", "NSObject"]})
            objects.append({"NS.objects": strings, "$class": plistlib.UID(len(objects) - 1)})
            array = plistlib.UID(len(objects) - 1)
            objects.append({"$classname": "ValkyrieMagnitudinalCurve", "$classes": ["ValkyrieMagnitudinalCurve", "NSObject"]})
            objects.append({"points": array, "$class": plistlib.UID(len(objects) - 1)})
            root[key] = plistlib.UID(len(objects) - 1)
        else:
            root[key] = plistlib.UID(2) if key == "name" else value
    archive = {"$archiver": "NSKeyedArchiver", "$version": 100000, "$top": {"root": plistlib.UID(1)}, "$objects": objects}
    return plistlib.dumps(archive, fmt=plistlib.FMT_BINARY, sort_keys=True)


def png(width, height, pixel):
    """An 8-bit RGB PNG of pixel(x, y) -> 0..255 grey."""
    raw = b"".join(b"\0" + bytes(v for x in range(width) for v in (pixel(x, y),) * 3) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def shape_png():
    # White paints on black: an ellipse 64 wide and 32 tall, centred in a 64 x 64 square.
    def pixel(x, y):
        d = ((x + 0.5 - 32) / 32) ** 2 + ((y + 0.5 - 32) / 16) ** 2
        return 255 if d <= 1 else 0
    return png(64, 64, pixel)


def grain_png():
    # Diagonal stripes, 8 pixels apart, so a grain that moves with the stroke shows it.
    return png(32, 32, lambda x, y: 255 if (x + y) % 8 < 4 else 64)


def write(name, changes, grain):
    settings = dict(BASELINE)
    settings.update(changes)
    settings["name"] = name
    folder = "00000000-0000-0000-0000-%012d/" % int(name[4:6])
    files = [(folder + "Brush.archive", keyed_archive(settings)), (folder + "Shape.png", shape_png())]
    if grain:
        files.append((folder + "Grain.png", grain_png()))
    path = os.path.join(HERE, name + ".brush")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for entry, data in files:
            info = zipfile.ZipInfo(entry, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)


if __name__ == "__main__":
    for name, (changes, grain) in FIXTURES.items():
        write(name, changes, grain)
