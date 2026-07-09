#!/usr/bin/env python3
"""Convert XPM files into a Windows .ico file."""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path


def _quoted_strings(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return re.findall(r'"((?:[^"\\]|\\.)*)"', text)


def _parse_xpm(path: Path) -> tuple[int, int, list[int]]:
    strings = _quoted_strings(path)
    if not strings:
        raise ValueError(f"{path}: no XPM strings found")

    width, height, color_count, chars_per_pixel = map(int, strings[0].split()[:4])
    colors: dict[str, tuple[int, int, int, int]] = {}

    for entry in strings[1 : 1 + color_count]:
        key = entry[:chars_per_pixel]
        fields = entry[chars_per_pixel:].split()
        color = None
        for index, field in enumerate(fields):
            if field == "c" and index + 1 < len(fields):
                color = fields[index + 1]
                break
        if color is None:
            raise ValueError(f"{path}: missing color for key {key!r}")
        if color.lower() == "none":
            colors[key] = (0, 0, 0, 0)
        elif color.startswith("#") and len(color) == 7:
            colors[key] = (
                int(color[1:3], 16),
                int(color[3:5], 16),
                int(color[5:7], 16),
                255,
            )
        else:
            raise ValueError(f"{path}: unsupported color {color!r}")

    pixels: list[int] = []
    rows = strings[1 + color_count : 1 + color_count + height]
    if len(rows) != height:
        raise ValueError(f"{path}: expected {height} rows, found {len(rows)}")

    for row in rows:
        if len(row) != width * chars_per_pixel:
            raise ValueError(f"{path}: malformed pixel row")
        for x in range(width):
            key = row[x * chars_per_pixel : (x + 1) * chars_per_pixel]
            red, green, blue, alpha = colors[key]
            pixels.append((alpha << 24) | (red << 16) | (green << 8) | blue)

    return width, height, pixels


def _ico_image(width: int, height: int, pixels: list[int]) -> bytes:
    xor = bytearray()
    and_mask = bytearray()
    and_stride = ((width + 31) // 32) * 4

    for y in range(height - 1, -1, -1):
        mask_row = bytearray(and_stride)
        for x in range(width):
            argb = pixels[y * width + x]
            alpha = (argb >> 24) & 0xFF
            red = (argb >> 16) & 0xFF
            green = (argb >> 8) & 0xFF
            blue = argb & 0xFF
            xor.extend((blue, green, red, alpha))
            if alpha == 0:
                mask_row[x // 8] |= 0x80 >> (x % 8)
        and_mask.extend(mask_row)

    header = struct.pack(
        "<IIIHHIIIIII",
        40,
        width,
        height * 2,
        1,
        32,
        0,
        len(xor),
        0,
        0,
        0,
        0,
    )
    return header + bytes(xor) + bytes(and_mask)


def build_ico(inputs: list[Path], output: Path) -> None:
    images = []
    for path in inputs:
        width, height, pixels = _parse_xpm(path)
        images.append((width, height, _ico_image(width, height, pixels)))

    offset = 6 + 16 * len(images)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    payload = bytearray()

    for width, height, image in images:
        directory.extend(
            struct.pack(
                "<BBBBHHII",
                width if width < 256 else 0,
                height if height < 256 else 0,
                0,
                0,
                1,
                32,
                len(image),
                offset,
            )
        )
        payload.extend(image)
        offset += len(image)

    output.write_bytes(bytes(directory) + bytes(payload))


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: xpm_to_ico.py OUTPUT.ico INPUT.xpm [INPUT.xpm ...]", file=sys.stderr)
        return 2

    output = Path(argv[1])
    inputs = [Path(arg) for arg in argv[2:]]
    build_ico(inputs, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
