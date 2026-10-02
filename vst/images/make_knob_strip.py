#!/usr/bin/env python3
"""Renders moog_knob_voice.png: a 128-frame knob filmstrip (112x14336) from the 5-frame original moog_knob_5frames.png.
The 5 frames share one lighting, differing only in the white pointer tick. So: the median of the frames is the knob
without a tick; the tick is cut out of a frame, then rotated about the knob centre to each of 128 angles (a 270 degree sweep) and composited on the tick-free knob.
Needs only Pillow: docker run --rm -v "$PWD":/w -w /w mpc-vst-html-art python3 make_knob_strip.py"""
import math
from PIL import Image, ImageChops

N = 128
src = Image.open("moog_knob_5frames.png").convert("RGBA")
S = src.width
frames = [src.crop((0, i * S, S, (i + 1) * S)) for i in range(src.height // S)]
n5 = len(frames)
cx = cy = (S - 1) / 2.0

def median(ims):
    out = Image.new("RGBA", (S, S))
    px = [im.load() for im in ims]
    o = out.load()
    for y in range(S):
        for x in range(S):
            o[x, y] = tuple(sorted(p[x, y][c] for p in px)[len(ims) // 2] for c in range(4))
    return out

base = median(frames)

def tick_of(im):
    """-> (mask of pixels that differ from the tick-free knob, their centroid angle in degrees clockwise from 12 o'clock)"""
    d = ImageChops.difference(im.convert("RGB"), base.convert("RGB")).convert("L").point(lambda v: 255 if v > 40 else 0)
    px = d.load()
    sx = sy = n = 0
    for y in range(S):
        for x in range(S):
            if px[x, y]:
                sx += x - cx; sy += y - cy; n += 1
    return d, math.degrees(math.atan2(sx / n, -sy / n)), n

angles = []
for im in frames:
    m, a, n = tick_of(im)
    angles.append(a)
print("pointer angles of the 5 frames:", [round(a, 1) for a in angles])
a0, a1 = -135.0, 135.0   # the usual 270 degree knob sweep (the 5 original frames are not evenly spaced)

# the tick sprite: the frame whose pointer is nearest 12 o'clock, cut where it differs from the tick-free knob
ref = min(range(n5), key=lambda i: abs(angles[i]))
mask, aref, _ = tick_of(frames[ref])
sprite = Image.composite(frames[ref], Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask)
sprite.putalpha(mask.filter(__import__("PIL.ImageFilter", fromlist=["x"]).GaussianBlur(0.6)))

out = Image.new("RGBA", (S, S * N), (0, 0, 0, 0))
for k in range(N):
    a = a0 + (a1 - a0) * k / (N - 1)
    big = sprite.resize((S * 4, S * 4), Image.BICUBIC).rotate(-(a - aref), resample=Image.BICUBIC, center=(cx * 4 + 2, cy * 4 + 2))
    f = base.copy()
    f.alpha_composite(big.resize((S, S), Image.LANCZOS))
    out.paste(f, (0, k * S))
out.save("moog_knob_voice.png", optimize=True)
print("wrote moog_knob_voice.png", out.size)
