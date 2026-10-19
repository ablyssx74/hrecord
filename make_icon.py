#!/usr/bin/env python3
"""Writes hrecord_mixer.rdef: a vector icon (HVIF) of four level meters and a record dot.
Run it to regenerate the icon; the shapes below are in a 64x64 space."""
import math, sys

def coord(c):
    if abs(c - round(c)) < 1e-6 and -32 <= round(c) <= 95:
        return bytes([int(round(c)) + 32])
    v = int(round((c + 128) * 102))
    return bytes([0x80 | (v >> 8), v & 0xFF])

styles, paths, shapes = [], [], []
def style(rgb):  # (r, g, b) or (r, g, b, alpha)
    if rgb in styles: return styles.index(rgb)
    styles.append(rgb); return len(styles) - 1
def shape(points, rgb):
    paths.append(points); shapes.append((style(rgb), len(paths) - 1))

def rounded(x0, y0, x1, y1, r, steps=4):
    pts = []
    for cx, cy, a0 in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for i in range(steps + 1):
            a = math.radians(a0 + 90.0 * i / steps)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

def circle(cx, cy, r, n=16):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]

# a soft shadow to the bottom right, as on most Haiku icons (two layers, drawn first)
shape(rounded(4, 4.5, 62.5, 62.5, 11), (0, 0, 0, 40))
shape(rounded(3.5, 3.5, 61, 61, 10), (0, 0, 0, 85))
shape(rounded(2.5, 2.5, 59, 59, 9), (38, 42, 52))
GREEN, YELLOW, RED = (70, 200, 90), (240, 205, 50), (232, 64, 52)
lit = [3, 4, 2, 5]
base = 54
for b, n in enumerate(lit):
    x = 10 + b * 12
    for s in range(n):
        y1 = base - s * 8
        shape([(x - 1, y1 - 7), (x + 8, y1 - 7), (x + 8, y1 - 1), (x - 1, y1 - 1)], GREEN if s < 3 else (YELLOW if s == 3 else RED))
shape(circle(51, 8.5, 3.6), (240, 70, 60))

out = bytearray(b"ncif")
out.append(len(styles))
for st in styles:
    out += bytes([3]) + bytes(st) if len(st) == 3 else bytes([1]) + bytes(st)
out.append(len(paths))
for pts in paths:
    out += bytes([0x0A, len(pts)])
    for x, y in pts:
        out += coord(x) + coord(y)
out.append(len(shapes))
for st, p in shapes:
    out += bytes([10, st, 1, p, 0])

hexs = out.hex().upper()
lines = ['\t$"' + hexs[i:i + 64] + '"' for i in range(0, len(hexs), 64)]
rdef = open("hrecord_mixer.rdef.in").read()
open("hrecord_mixer.rdef", "w").write(rdef.replace("@ICON@", "\n".join(lines)))
open("hrecord_mixer.hvif", "wb").write(out)
print(len(out), "bytes")
