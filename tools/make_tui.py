#!/usr/bin/env python3
"""Native MPC skins for Maze Voice 2 and Maze Sequencer 2: plugins/<name>/skins/TUI.json + Q-Links.json + the PNGs, all drawn
from code (Pillow) in the realistic-hardware way (mpc-vst-plugins docs/SKIN_STUDIO.md "Realistic hardware skins"): a black
brushed module between wood cheeks, lit from the top left; knobs whose light stays put while only the pointer turns; bat
switches; LED step keys; drawn pick-lists on `ui_open`. Controls sit in blocks of four, one Q-Link group per block.

    python3 tools/make_tui.py voice|sequencer [--preview]     # --preview: docs/skin-preview/<name>-<tab>.png with default values

Coordinates are the plugin area, 1280 x 628. Labels are baked into the art (Roboto Condensed); live values are Titillium Web."""
import hashlib, json, math, os, shutil, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "tools", "fonts")
W, H, SS = 1280, 628, 2
GESTURE = "Instant"
SWEEP = 270   # the 1.x knob strip's travel
LIGHT = -45
FRAMES = 64
DEVICE_TEXT = 0.62   # the MPC draws a Label font about this fraction of its "height" (measured on the Live III)

# palette: the black panel, its white print, the knobs' aluminium, the LEDs
PANEL = 30
INK = (236, 236, 232)
DIM = (150, 150, 146)
RED, RED_DK = (255, 46, 40), (70, 14, 12)
GREEN, GREEN_DK = (120, 255, 90), (22, 52, 20)
VALUE = (214, 218, 222)


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.sin(a), cy - r * math.cos(a)


_fonts = {}


def font(size, weight=700):
    """Titillium Web, the 1.x skin's face: Bold for labels (weight >= 700), SemiBold otherwise."""
    k = (size, weight >= 700)
    if k not in _fonts:
        name = "TitilliumWeb-Bold.ttf" if weight >= 700 else "TitilliumWeb-SemiBold.ttf"
        _fonts[k] = ImageFont.truetype(os.path.join(FONTS, name), int(round(size * SS)))
    return _fonts[k]


def diag(w, h, lo, hi):
    gy = Image.linear_gradient("L").resize((w, h))
    gx = Image.linear_gradient("L").transpose(Image.Transpose.TRANSPOSE).resize((w, h))
    return ImageChops.add(gx, gy, 2.0).point(lambda v: int(hi + (lo - hi) * v / 255))


def rmask(size, box, r):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=r, fill=255)
    return m


def soft_shadow(size, box, blur, alpha, off, ellipse=True):
    sh = Image.new("RGBA", size, (0, 0, 0, 0))
    x0, y0, x1, y1 = box
    (ImageDraw.Draw(sh).ellipse if ellipse else ImageDraw.Draw(sh).rounded_rectangle)(
        (x0 + off[0] * SS, y0 + off[1] * SS, x1 + off[0] * SS, y1 + off[1] * SS), fill=(0, 0, 0, alpha))
    return sh.filter(ImageFilter.GaussianBlur(blur * SS))


# ------------------------------------------------------------------------------------------------ the module
FACE = (26, 8, 1254, 620)   # the black plate between the wood cheeks


def wood(w, h):
    """A walnut cheek: warm brown with long grain, lit from the left."""
    base = Image.new("RGB", (w, h), (92, 52, 30))
    grain = Image.effect_noise((max(1, w // 6), h // 2), 40).resize((w, h), Image.BILINEAR)
    streak = Image.effect_noise((max(1, w // 2), 8), 60).resize((w, h), Image.BICUBIC)
    g = ImageChops.add(grain, streak, 2.0)
    tint = Image.merge("RGB", (g.point(lambda v: v * 70 // 255), g.point(lambda v: v * 38 // 255), g.point(lambda v: v * 20 // 255)))
    out = ImageChops.add(base, tint, 1.0, -30)
    shade = Image.linear_gradient("L").transpose(Image.Transpose.TRANSPOSE).resize((w, h)).point(lambda v: 40 + v * 140 // 255)
    return Image.composite(out, Image.new("RGB", (w, h), (24, 12, 6)), shade.point(lambda v: 255 - v // 3))


PANEL_ART = KNOB_ART = None   # set per plugin: the 1.x panel background (vst/images/panel_bg.png, sequencer/vst/images/panel_bg.png)


def plate():
    """The page: the 1.x panel picture (dark brushed plate, walnut edges), at the art's 2x scale."""
    return Image.open(PANEL_ART).convert("RGBA").resize((W * SS, H * SS), Image.LANCZOS)


def screw(page, cx, cy, r=7):
    s = (r + 6) * 2 * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    c, R = s / 2, r * SS
    im.alpha_composite(soft_shadow((s, s), (c - R, c - R, c + R, c + R), 1.5, 160, (1, 2)))
    head = diag(2 * R, 2 * R, 18, 92)
    hm = Image.new("L", (2 * R, 2 * R), 0); ImageDraw.Draw(hm).ellipse((0, 0, 2 * R - 1, 2 * R - 1), fill=255)
    im.paste(Image.merge("RGB", (head,) * 3).convert("RGBA"), (int(c - R), int(c - R)), hm)
    d = ImageDraw.Draw(im)
    d.ellipse((c - R, c - R, c + R, c + R), outline=(8, 8, 8, 255), width=SS)
    x0, y0 = polar(c, c, R * 0.55, 0); x1, y1 = polar(c, c, R * 0.55, 180)
    d.line([(x0, y0), (x1, y1)], fill=(4, 4, 4, 255), width=int(1.6 * SS))
    x0, y0 = polar(c, c, R * 0.55, 90); x1, y1 = polar(c, c, R * 0.55, 270)
    d.line([(x0, y0), (x1, y1)], fill=(4, 4, 4, 255), width=int(1.6 * SS))
    page.alpha_composite(im, (int(cx * SS - c), int(cy * SS - c)))


# ------------------------------------------------------------------------------------------------ knobs
_body = {}


def knob_body(r, cap):
    """What doesn't turn: the black knurled skirt (round, so the same at any angle) with its rim light, and the cap: brushed
    aluminium (cap="alu", the big knobs) or black (cap="black", the small trimmers), with a fixed sheen."""
    key = (r, cap)
    if key in _body:
        return _body[key]
    R = r * SS
    s = int(2 * R + 12 * SS)
    c = s / 2
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    m = Image.new("L", (s, s), 0); ImageDraw.Draw(m).ellipse((c - R, c - R, c + R, c + R), fill=255)
    skirt = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    g = diag(s, s, 6, 40)
    skirt.paste(Image.merge("RGB", (g, g, g)).convert("RGBA"), (0, 0), m)
    im.alpha_composite(skirt)
    d = ImageDraw.Draw(im)
    n = max(36, int(r * 1.6))
    for i in range(n):   # knurls: each ridge brighter where it faces the light
        a = 360 * i / n
        lit = 0.5 + 0.5 * math.cos(math.radians(a - LIGHT))
        v = int(14 + 70 * lit ** 2)
        x0, y0 = polar(c, c, R * (0.80 if cap == "alu" else 0.72), a)
        x1, y1 = polar(c, c, R * 0.985, a)
        d.line([(x0, y0), (x1, y1)], fill=(v, v, v + 2, 255), width=max(1, int(1.2 * SS)))
    rim = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(rim).arc((c - R + SS, c - R + SS, c + R - SS, c + R - SS), start=190, end=300, fill=(200, 200, 205, 150), width=int(1.4 * SS))
    im.alpha_composite(rim.filter(ImageFilter.GaussianBlur(0.7 * SS)))
    rt = R * (0.70 if cap == "alu" else 0.62)
    st = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(st).ellipse((c - rt + 1.5 * SS, c - rt + 3 * SS, c + rt + 1.5 * SS, c + rt + 3 * SS), fill=(0, 0, 0, 210))
    im.alpha_composite(st.filter(ImageFilter.GaussianBlur(2 * SS)))
    cm = Image.new("L", (s, s), 0); ImageDraw.Draw(cm).ellipse((c - rt, c - rt, c + rt, c + rt), fill=255)
    capim = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    if cap == "alu":
        # spun aluminium: concentric fine rings over a top-left to bottom-right gradient, a bright band across the light axis
        base = diag(s, s, 120, 228)
        rings = Image.new("L", (s, s), 128)
        rd = ImageDraw.Draw(rings)
        k = 0
        rr = rt
        while rr > 2:
            v = 128 + (14 if k % 2 else -14) + (k * 37 % 11) - 5
            rd.ellipse((c - rr, c - rr, c + rr, c + rr), fill=v)
            rr -= 0.9 * SS; k += 1
        alu = ImageChops.add(base, rings, 1.0, -128)
        capim.paste(Image.merge("RGB", (alu, alu, alu.point(lambda v: min(255, v + 4)))).convert("RGBA"), (0, 0), cm)
        im.alpha_composite(capim)
        # the spun-metal "bow tie": two bright wedges facing the light and its opposite
        bow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        bd = ImageDraw.Draw(bow)
        for a0 in (LIGHT - 18, LIGHT + 180 - 18):
            pts = [(c, c)] + [polar(c, c, rt, a0 + 36 * i / 12) for i in range(13)]
            bd.polygon(pts, fill=(255, 255, 255, 70))
        bow = bow.filter(ImageFilter.GaussianBlur(5 * SS))
        im.alpha_composite(Image.composite(bow, Image.new("RGBA", (s, s), (0, 0, 0, 0)), cm))
        ImageDraw.Draw(im).ellipse((c - rt, c - rt, c + rt, c + rt), outline=(30, 30, 32, 255), width=SS)
    else:
        top = diag(s, s, 8, 46)
        capim.paste(Image.merge("RGB", (top, top, top)).convert("RGBA"), (0, 0), cm)
        im.alpha_composite(capim)
        sheen = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        ImageDraw.Draw(sheen).ellipse((c - rt * 0.75, c - rt * 0.85, c + rt * 0.05, c - rt * 0.15), fill=(255, 255, 255, 45))
        im.alpha_composite(sheen.filter(ImageFilter.GaussianBlur(3 * SS)))
        ImageDraw.Draw(im).ellipse((c - rt, c - rt, c + rt, c + rt), outline=(0, 0, 0, 255), width=SS)
    _body[key] = (im, rt)
    return _body[key]


def knob_frame(r, cap, t):
    body, rt = knob_body(r, cap)
    im = body.copy()
    c = im.width / 2
    deg = -SWEEP / 2 + SWEEP * t
    d = ImageDraw.Draw(im)
    if cap == "alu":   # a black line engraved across the cap from near the centre to its edge
        x0, y0 = polar(c, c, rt * 0.12, deg); x1, y1 = polar(c, c, rt * 0.96, deg)
        w = max(2, int(rt * 0.075))
        d.line([(x0, y0), (x1, y1)], fill=(16, 16, 18, 255), width=w)
    else:              # a white line on the black cap
        x0, y0 = polar(c, c, rt * 0.10, deg); x1, y1 = polar(c, c, rt * 0.95, deg)
        w = max(2, int(rt * 0.11))
        d.line([(x0, y0), (x1, y1)], fill=(240, 240, 236, 255), width=w)
    return im.resize((im.width // SS, im.height // SS), Image.LANCZOS)


# ------------------------------------------------------------------------------------------------ small parts
def _chrome_disc(d, cx, cy, r, lo=70, hi=250, steps=24):
    """A domed chrome disc lit from the top left: concentric fills drifting toward the light."""
    for i in range(steps):
        t = i / (steps - 1)
        rr = r * (1 - 0.82 * t)
        ox, oy = -r * 0.32 * t, -r * 0.36 * t
        v = int(lo + (hi - lo) * t ** 1.4)
        d.ellipse((cx + ox - rr, cy + oy - rr, cx + ox + rr, cy + oy + rr), fill=(v, v, v + 4, 255))


def _lever(im, c, cy, tip_y, s):
    """A bat lever from the bushing to tip_y: a tapered chrome shaft, shaded across its width, ending in the flattened bat."""
    up = tip_y < cy
    L = abs(tip_y - cy)
    shaft = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(shaft)
    w0, w1 = s * 0.075, s * 0.11          # half-widths at the bushing and under the bat
    n = 40
    for k in range(n):                    # vertical strips across the width, darker at the edges, a hot line left of centre
        u = -1 + 2 * (k + 0.5) / n
        light = max(0.0, 1 - abs(u + 0.35) * 1.4) ** 1.6
        v = int(60 + 190 * light + 30 * (1 - abs(u)))
        v = min(v, 255)
        x0b, x1b = c + u * w0, c + (u + 2 / n) * w0
        x0t, x1t = c + u * w1, c + (u + 2 / n) * w1
        d.polygon([(x0b, cy), (x1b, cy), (x1t, tip_y), (x0t, tip_y)], fill=(v, v, v + 4, 255))
    im.alpha_composite(shaft)
    d = ImageDraw.Draw(im)
    bw, bh = s * 0.17, s * 0.13           # the bat: an oval paddle, foreshortened because it leans toward or away from us
    by = tip_y + (bh * 0.25 if up else -bh * 0.25)
    for i in range(18):
        t = i / 17
        rx, ry = bw * (1 - 0.7 * t), bh * (1 - 0.7 * t)
        ox, oy = -bw * 0.25 * t, -bh * 0.35 * t
        v = int(55 + 200 * t ** 1.3)
        d.ellipse((c + ox - rx, by + oy - ry, c + ox + rx, by + oy + ry), fill=(v, v, v + 5, 255))


def bat_frame(pos, n=2, size=46):
    """A chrome bat toggle in a dark escutcheon, lit from the top left: a hex bushing nut with its washer, the shaft leaning
    to its position (pos of n, 0 = up) with its shadow thrown down-right; the centre position is seen end-on."""
    s = size * SS * 2                    # drawn at 4x, reduced at the end
    im = Image.new("RGBA", (s, s * 2), (0, 0, 0, 0))
    c, cy = s / 2, s
    lean = (-1 + 2 * pos / (n - 1)) if n > 1 else 0   # -1 up .. +1 down
    tip_y = cy + lean * s * 0.70
    # shadow of nut and lever
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh)
    off = s * 0.06
    sd.ellipse((c - s * 0.3 + off, cy - s * 0.3 + off, c + s * 0.3 + off, cy + s * 0.3 + off), fill=(0, 0, 0, 200))
    if abs(lean) > 0.01:
        sd.line([(c + off, cy + off), (c + off * 2.2, tip_y + off * 2.2)], fill=(0, 0, 0, 170), width=int(s * 0.2))
        sd.ellipse((c + off * 2.2 - s * 0.17, tip_y + off * 2.2 - s * 0.13, c + off * 2.2 + s * 0.17, tip_y + off * 2.2 + s * 0.13), fill=(0, 0, 0, 150))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(s * 0.035)))
    d = ImageDraw.Draw(im)
    # hex nut: six facets each shaded by how it faces the light, then a domed washer and the bushing
    R = s * 0.27
    pts = [polar(c, cy, R, 30 + 60 * i) for i in range(6)]
    for i in range(6):
        mid = 60 + 60 * i                 # facet direction (screen degrees, 0 = right, clockwise)
        face = math.cos(math.radians(mid - 225))   # the light sits top-left
        v = int(120 + 105 * face)
        d.polygon([(c, cy), pts[i], pts[(i + 1) % 6]], fill=(v, v, v + 5, 255))
    d.polygon(pts, outline=(30, 30, 34, 255), width=max(1, int(s * 0.012)))
    _chrome_disc(d, c, cy, R * 0.72, 80, 235)
    d.ellipse((c - R * 0.44, cy - R * 0.44, c + R * 0.44, cy + R * 0.44), fill=(28, 28, 30, 255))
    _chrome_disc(d, c, cy, R * 0.36, 90, 245)
    if abs(lean) < 0.01:                  # end-on: the bat faces us, a bright dome over the bushing
        _chrome_disc(d, c, cy, s * 0.15, 60, 255)
    else:
        _lever(im, c, cy, tip_y, s)
    return im.resize((size, size * 2), Image.LANCZOS)


def push_button(lit, d=40, color=(60, 60, 64)):
    """A round push button: a dark cap in a bezel; lit = pressed (a red ring round it)."""
    s = (d + 12) * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    c, R = s / 2, d * SS / 2
    im.alpha_composite(soft_shadow((s, s), (c - R, c - R, c + R, c + R), 2.5, 170, (2, 3)))
    dr = ImageDraw.Draw(im)
    dr.ellipse((c - R, c - R, c + R, c + R), fill=(12, 12, 13, 255))
    rc = R * 0.80
    top = diag(int(2 * rc), int(2 * rc), color[0] - 34, color[0] + 30)
    cm = Image.new("L", (int(2 * rc), int(2 * rc)), 0); ImageDraw.Draw(cm).ellipse((0, 0, 2 * rc - 1, 2 * rc - 1), fill=255)
    off = 1.2 * SS if lit else 0
    im.paste(Image.merge("RGB", (top, top.point(lambda v: v), top.point(lambda v: min(255, v + 3)))).convert("RGBA"), (int(c - rc + off), int(c - rc + off)), cm)
    if lit:
        ring = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        ImageDraw.Draw(ring).ellipse((c - R - 2 * SS, c - R - 2 * SS, c + R + 2 * SS, c + R + 2 * SS), outline=RED + (255,), width=int(2.4 * SS))
        im.alpha_composite(ring.filter(ImageFilter.GaussianBlur(1.2 * SS)))
        im.alpha_composite(ring)
    return im.resize((s // SS, s // SS), Image.LANCZOS)


def led(lit, d, on=RED, off_col=(52, 50, 50), glow=True):
    """An LED lens: off is a dull dome, on is bright with a halo."""
    s = (d + 18) * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    c, R = s / 2, d * SS / 2
    dr = ImageDraw.Draw(im)
    if lit and glow:
        halo = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        ImageDraw.Draw(halo).ellipse((c - R * 1.6, c - R * 1.6, c + R * 1.6, c + R * 1.6), fill=on + (110,))
        im.alpha_composite(halo.filter(ImageFilter.GaussianBlur(R * 0.45)))
    dr.ellipse((c - R - SS, c - R - SS, c + R + SS, c + R + SS), fill=(8, 8, 8, 255))
    col = on if lit else off_col
    lens = diag(int(2 * R), int(2 * R), 0, 255)
    lm = Image.new("L", (int(2 * R), int(2 * R)), 0); ImageDraw.Draw(lm).ellipse((0, 0, 2 * R - 1, 2 * R - 1), fill=255)
    k = 0.55 if lit else 0.35
    rgb = Image.merge("RGB", tuple(lens.point(lambda v, cc=cc: int(cc * (1 - k) + min(255, cc * 0.6 + v * 0.5) * k)) for cc in col))
    im.paste(rgb.convert("RGBA"), (int(c - R), int(c - R)), lm)
    ImageDraw.Draw(im).ellipse((c - R * 0.55, c - R * 0.62, c - R * 0.05, c - R * 0.15), fill=(255, 255, 255, 150 if lit else 70))
    return im.resize((s // SS, s // SS), Image.LANCZOS)


def key_image(w, h, caption, lit, size=17):
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), 6 * SS, fill=(150, 30, 26, 255) if lit else (22, 22, 24, 255), outline=(90, 90, 94, 255), width=SS)
    d.text((w * SS / 2, h * SS / 2), caption, font=font(size, 700), fill=(255, 255, 255, 255) if lit else INK + (255,), anchor="mm")
    return im.resize((w, h), Image.LANCZOS)


# ------------------------------------------------------------------------------------------------ TUI.json pieces
class Obj:
    def __init__(self, *pairs):
        self.pairs = list(pairs)


def dump(v, ind=0):
    sp = "  " * ind
    pairs = v.pairs if isinstance(v, Obj) else list(v.items()) if isinstance(v, dict) else None
    if pairs is not None:
        if not pairs:
            return "{}"
        return "{\n%s\n%s}" % (",\n".join("%s  %s: %s" % (sp, dump(k), dump(x, ind + 1)) for k, x in pairs), sp)
    if isinstance(v, list):
        return "[\n%s\n%s]" % (",\n".join(sp + "  " + dump(x, ind + 1) for x in v), sp) if v else "[]"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return '"%s"' % str(v).replace("\\", "\\\\").replace('"', '\\"')


def bounds(b, focus="No", visible="Always", vis=None):
    d = {"version": 2 if vis else 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": "Show", "whenVisible": visible,
         "boundsType": "Absolute", "bounds": "%d %d %d %d" % tuple(int(round(x)) for x in b)}
    if vis:
        d["additionalInvalidatingHandles"] = [vis]
    return d


def entry(name, typ, data, b, remap=None, focus="No", visible="Always", vis=None):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": [{"key": "Data", "value": remap}] if remap else []},
            "bounds": bounds(b, focus, visible, vis)}


def bg(colour="0", image=""):
    return {"version": 1, "focussed": {"version": 1, "colour": colour, "image": image}, "unfocussed": {"version": 1, "colour": colour, "image": image}}


def component(children, actions=None, ignore=False, background=None):
    return {"version": 2, "actions": actions or [], "backgroundData": background or bg(), "ignoreMousePresses": ignore,
            "disableCoarseDataWheel": False, "componentsData": children}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


KNOB_ACTIONS = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay"), act("Enter Pressed", "Show Overlay", "knob overlay")]


def shared_defs():
    focus = {"version": 1, "backgroundColour": "19ffffff", "outlineColour": "ffe81f40", "backgroundInset": 4.0, "outlineThickness": 1.0}
    slot = component([entry("focus", "Focus", focus, (0, 0, 278, 107), visible="WhenFocussed"),
                      entry("slider", "Slider", {"version": 1, "handleName": "Data"}, (15, 15, 248, 77))], actions=KNOB_ACTIONS)
    overlay = component([entry("overlay", "KnobOverlay", Obj(
        ("version", 2), ("version", 1),
        ("actions", [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost", "handleName": "Focus Lost", "additionalData": ""}]),
        ("overlayType", "Knob With Header"), ("scaleType", "Linear"), ("polarityType", "Unipolar"),
        ("showMinusSign", True), ("handleName", "Data")), (0, 0, 1280, 800), focus="Yes")], ignore=True)
    return [{"key": "sliderSlot", "value": slot}, {"key": "knob overlay", "value": overlay}]


def hexc(c, a=255):
    return "%02x%02x%02x%02x" % ((a,) + tuple(c))


# ------------------------------------------------------------------------------------------------ the skin builder
STEP_W = 55   # a step key's touch width (the 1.x LED art, 48 px, sits in the middle)


def _centre(art, canvas):
    c = canvas.copy()
    c.alpha_composite(art, ((c.width - art.width) // 2, (c.height - art.height) // 2))
    return c


class Skin:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(ROOT, "plugins", name)
        self.out = os.path.join(self.dir, "skins")
        pj = json.load(open(os.path.join(self.dir, "params.json")))
        self.params = list(pj["params"])
        extra = os.path.join(self.dir, "params_new.json")   # parameters added after the released ones (appended, as gen_params.py does)
        if os.path.exists(extra):
            self.params += json.load(open(extra))
        self.keys = [p["key"] for p in self.params]
        self.popups = [i for i, p in enumerate(self.params) if p.get("popup_of", -1) >= 0]
        self.ui_open = self.popups[0]
        self.ui_steps = len(self.popups) + 1
        self.defs, self.knob_defs, self.pick_defs = shared_defs(), {}, {}
        self.files = set()
        if os.path.isdir(self.out):
            shutil.rmtree(self.out)
        os.makedirs(self.out)
        self.clear = self.save(Image.new("RGBA", (8, 8), (0, 0, 0, 3)), "touch.png")   # alpha 3: invisible but touchable

    def P(self, key):
        return "Parameter %d" % (self.ui_open if key == "ui_open" else self.keys.index(key))

    def spec(self, key):
        return self.params[self.keys.index(key)]

    def save(self, im, name):
        if name not in self.files:
            im.save(os.path.join(self.out, name))
            self.files.add(name)
        return name

    def knob_comp(self, r, cap=None):
        """The 1.x Moog-style knob (moog_knob_voice.png: 128 frames of 112 px, 270 degrees), scaled so its cap is r px."""
        k = "knob_%d" % r
        if k not in self.knob_defs:
            src = Image.open(KNOB_ART).convert("RGBA")
            n, S = src.height // src.width, src.width
            fw = int(round(2 * r * 112 / 90))   # the knob fills ~90 of its 112 px
            frames = [src.crop((0, i * S, S, (i + 1) * S)).resize((fw, fw), Image.LANCZOS) for i in range(n)]
            strip = Image.new("RGBA", (fw, fw * n), (0, 0, 0, 0))
            for i, f in enumerate(frames):
                strip.paste(f, (0, i * fw))
            img = self.save(strip, k + ".png")
            comp = component([entry("knob", "Knob", {"version": 3, "knobType": "FilmStrip", "filmStrip": img, "numFrames": n, "invert": False,
                                                     "dragOrientation": "Vertical", "handleName": "Data"}, (0, 0, fw, fw))],
                             actions=KNOB_ACTIONS, background=bg("00000000"))
            self.defs.append({"key": k, "value": comp})
            self.knob_defs[k] = (fw, frames)
        return k, self.knob_defs[k]

    def pick_comp(self, key, opts):
        """A drawn pick-list (one radio Button per option, text baked in) and its close key on ui_open."""
        cols = 1 if len(opts) <= 8 else 3
        rows = (len(opts) + cols - 1) // cols
        cw, ch, gap, hdr = 170, 46, 6, 50
        pw, ph = cols * (cw + gap) + gap + 12, hdr + rows * (ch + gap) + gap + 6
        ck = "list_%d_%s" % (len(opts), hashlib.md5("|".join(opts).encode()).hexdigest()[:8])
        if ck not in self.pick_defs:
            im = Image.new("RGBA", (pw * SS, ph * SS), (0, 0, 0, 0))
            d = ImageDraw.Draw(im)
            d.rounded_rectangle((0, 0, pw * SS - 1, ph * SS - 1), 10 * SS, fill=(18, 18, 20, 250), outline=(120, 120, 124, 255), width=2 * SS)
            d.text((16 * SS, hdr / 2 * SS), self.spec(key)["name"].upper(), font=font(19, 700), fill=INK + (255,), anchor="lm")
            panel = self.save(im.resize((pw, ph), Image.LANCZOS), ck + "_bg.png")
            kids = [entry("panel", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": panel}, (0, 0, pw, ph))]
            for i, o in enumerate(opts):
                r, c = i % rows, i // rows
                on = self.save(key_image(cw, ch, o, True, 19), "%s_%d_on.png" % (ck, i))
                off = self.save(key_image(cw, ch, o, False, 19), "%s_%d_off.png" % (ck, i))
                kids.append(entry("opt %d" % i, "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": i, "numButtonsInGroup": len(opts),
                                                           "handleName": "Data", "gestureBehaviour": GESTURE}, (8 + gap + c * (cw + gap), hdr + r * (ch + gap), cw, ch)))
            xon, xoff = self.save(key_image(56, 36, "X", True, 20), "x_on.png"), self.save(key_image(56, 36, "X", False, 20), "x_off.png")
            kids.append(entry("close", "Button", {"version": 2, "onImage": xon, "offImage": xoff, "buttonId": 0, "numButtonsInGroup": self.ui_steps,
                                                  "handleName": "Data", "gestureBehaviour": GESTURE}, (pw - 66, 7, 56, 36), remap=self.P("ui_open")))
            self.defs.append({"key": ck, "value": component(kids)})
            self.pick_defs[ck] = (pw, ph, opts)
        return ck, self.pick_defs[ck]

    def replacement_strings(self, used):
        out = []
        for k in self.keys:
            p = self.spec(k)
            if k in used and p["nopts"] and p.get("popup_of", -1) < 0:
                out.append({"key": self.P(k), "value": p["opts"]})
        out.append({"key": self.P("ui_open"), "value": ["Closed"] + [self.params[self.params[j]["popup_of"]]["name"] for j in self.popups]})
        return out


class Page:
    def __init__(self, skin, name, fn, sub=0, tab=None):
        self.skin, self.name, self.fn, self.sub, self.tabname = skin, name, fn, sub, tab or name
        self.hi = plate()
        self.kids, self.lists, self.groups, self.qrows, self.pv = [], [], [], [], []
        self.used = set()

    # -- printing on the panel
    def text(self, x, y, s, size=17, weight=700, fill=INK, anchor="mm"):
        d = ImageDraw.Draw(self.hi)
        d.text((x * SS, (y + 1) * SS), s, font=font(size, weight), fill=(0, 0, 0, 160), anchor=anchor)
        d.text((x * SS, y * SS), s, font=font(size, weight), fill=fill + (255,), anchor=anchor)

    def line(self, pts, w=2, fill=INK):
        d = ImageDraw.Draw(self.hi)
        d.line([(x * SS, (y + 1) * SS) for x, y in pts], fill=(0, 0, 0, 150), width=int(w * SS))
        d.line([(x * SS, y * SS) for x, y in pts], fill=fill + (255,), width=int(w * SS))

    def put(self, im, cx, cy):   # a 1x picture into the 2x art
        big = im.resize((im.width * SS, im.height * SS), Image.LANCZOS)
        self.hi.alpha_composite(big, (int(cx * SS - big.width / 2), int(cy * SS - big.height / 2)))

    def section(self, x0, y0, x1, y1, title=None):
        """A printed section: a thin white outline all round, its title broken into the top edge."""
        if title:
            tw = font(19, 700).getlength(title) / SS + 18
            cx = (x0 + x1) / 2
            self.line([(cx - tw / 2, y0), (x0, y0), (x0, y1), (x1, y1), (x1, y0), (cx + tw / 2, y0)], 1.6, DIM)
            self.text(cx, y0, title, 19)
        else:
            self.line([(x0, y0), (x0, y1), (x1, y1), (x1, y0), (x0, y0)], 1.6, DIM)

    def wordmark(self, x, y, big, small):
        self.text(x, y, big, 40, 800, anchor="ls")
        bx = x + font(40, 800).getlength(big) / SS + 14
        self.line([(bx, y - 28), (bx, y - 4)], 2, INK)
        self.text(bx + 12, y - 7, small, 22, 700, anchor="ls")

    def ticks(self, cx, cy, r, n=11):
        for i in range(n):
            a = -SWEEP / 2 + SWEEP * i / (n - 1)
            long = i in (0, n // 2, n - 1)
            self.line([polar(cx, cy, r + 5, a), polar(cx, cy, r + (13 if long else 9), a)], 2.2 if long else 1.6, INK)

    # -- controls
    def knob(self, key, cx, cy, r, label, cap="alu", scale=None, value=True):
        """A knob: shadow, scale and label in the art; a 64-frame film strip; its value printed below."""
        s = self.skin
        k, (fw, frames) = s.knob_comp(r, cap)
        R = r * SS
        self.hi.alpha_composite(soft_shadow(self.hi.size, ((cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS), 6, 170, (4, 7)))
        self.hi.alpha_composite(soft_shadow(self.hi.size, ((cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS), 1.5, 200, (1, 2)))
        self.ticks(cx, cy, r, 11 if r >= 40 else 7)
        self.text(cx, cy - r - 26, label, 19 if r >= 40 else 17)
        if scale:
            self.text(cx - r - 4, cy + r + 4, scale[0], 13, 600, DIM, "rm")
            self.text(cx + r + 4, cy + r + 4, scale[1], 13, 600, DIM, "lm")
        b = (cx - fw / 2, cy - fw / 2, fw, fw)
        self.kids.append(entry(key, k, {"version": 1, "handleName": "Data"}, b, remap=s.P(key), focus="Yes"))
        self.pv.append(("knob", key, b, frames))
        if value:
            if r >= 30:
                self.value(key, (cx - 70, cy + r + 13, 140, 30), 23.0)
            else:   # small knobs in tight rows keep the smaller value
                self.value(key, (cx - 70, cy + r + 14, 140, 26))

    def value(self, key, b, h=20.0, colour=VALUE):
        data = {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "SemiBold", "height": h},
                                            "colour": hexc(colour), "justification": "horizontallyCentred verticallyCentred", "case": "Original"},
                "type": "Value", "handleName": "Data"}
        self.kids.append(entry(key + " value", "Label", data, b, remap=self.skin.P(key)))
        self.pv.append(("value", key, b, h, colour))
        self.used.add(key)

    def bat(self, key, cx, cy, labels, n=2):
        """A bat switch: an Image film strip shows the lever (one frame per option, top first), and a touch area per option
        label sets it (radio Buttons). Two options: a tap anywhere toggles it instead (one Button, buttonId 1)."""
        s = self.skin
        size = 60
        frames = [bat_frame(i if n > 2 else (1 - i), n, size) for i in range(n)]   # two-way: option 1 = lever up
        strip = Image.new("RGBA", (size, size * 2 * n), (0, 0, 0, 0))
        for i, f in enumerate(frames):
            strip.paste(f, (0, i * size * 2))
        img = s.save(strip, "bat_%d.png" % n)
        b = (cx - size / 2, cy - size, size, size * 2)
        self.kids.append(entry(key + " lever", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": img, "numFrames": n, "handleName": "Data"},
                               b, remap=s.P(key)))
        self.pv.append(("strip", key, b, frames))
        ys = [cy - 38, cy + 38] if n == 2 else [cy - 44, cy, cy + 44]
        order = list(range(n)) if n > 2 else [1, 0]   # the label on top is the option the lever shows when up
        for row, opt in enumerate(order):
            self.text(cx + 40, ys[row], labels[opt], 15, 700, anchor="lm")
        if n == 2:
            self.kids.append(entry(key, "Button", {"version": 2, "onImage": s.clear, "offImage": s.clear, "buttonId": 1, "numButtonsInGroup": 1,
                                                   "handleName": "Data", "gestureBehaviour": GESTURE}, (cx - 30, cy - 50, 150, 100), remap=s.P(key)))
        else:
            for row, opt in enumerate(order):
                self.kids.append(entry("%s %d" % (key, opt), "Button", {"version": 2, "onImage": s.clear, "offImage": s.clear, "buttonId": opt, "numButtonsInGroup": n,
                                                                         "handleName": "Data", "gestureBehaviour": GESTURE},
                                       (cx - 30, ys[row] - 17, 170, 34), remap=s.P(key)))

    def button(self, key, cx, cy, label, d=40, label_dy=-38):
        s = self.skin
        on, off = s.save(push_button(True, d), "push_%d_on.png" % d), s.save(push_button(False, d), "push_%d_off.png" % d)
        im = push_button(False, d)
        b = (cx - im.width / 2, cy - im.height / 2, im.width, im.height)
        self.kids.append(entry(key, "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": 1, "numButtonsInGroup": 1,
                                               "handleName": "Data", "gestureBehaviour": GESTURE}, b, remap=s.P(key)))
        self.pv.append(("button", key, b, off))
        self.text(cx, cy + label_dy, label, 16)

    def keys(self, key, cx, cy, kw, kh, label):
        """A row of radio keys, one per option (lit = current)."""
        s = self.skin
        opts = s.spec(key)["opts"]
        x0 = cx - len(opts) * (kw + 6) / 2 + 3
        self.text(cx, cy - kh / 2 - 16, label, 17)
        for i, o in enumerate(opts):
            on = s.save(key_image(kw, kh, o, True, 17), "key_%s_%d_on.png" % (key, i))
            off = s.save(key_image(kw, kh, o, False, 17), "key_%s_%d_off.png" % (key, i))
            b = (x0 + i * (kw + 6), cy - kh / 2, kw, kh)
            self.kids.append(entry("%s %d" % (key, i), "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": i, "numButtonsInGroup": len(opts),
                                                                   "handleName": "Data", "gestureBehaviour": GESTURE}, b, remap=s.P(key)))
            self.pv.append(("radio", key, b, (on, off, i)))

    def step(self, key, cx, cy):
        """A step: the 1.x LED key (sequencer/vst/images/led_off.png, led_on.png, used as they were), tappable."""
        s = self.skin
        art = os.path.join(ROOT, "sequencer", "vst", "images")
        on, off = Image.open(os.path.join(art, "led_on.png")).convert("RGBA"), Image.open(os.path.join(art, "led_off.png")).convert("RGBA")
        pad = Image.new("RGBA", (STEP_W, 56), (0, 0, 0, 0))   # the same art on a wider transparent canvas: a wider touch area
        on_n, off_n = s.save(_centre(on, pad), "led_on_w.png"), s.save(_centre(off, pad), "led_off_w.png")
        b = (cx - STEP_W / 2, cy - 28, STEP_W, 56)
        self.kids.append(entry(key, "Button", {"version": 2, "onImage": on_n, "offImage": off_n, "buttonId": 1, "numButtonsInGroup": 1,
                                               "handleName": "Data", "gestureBehaviour": GESTURE}, b, remap=s.P(key)))
        self.pv.append(("button", key, b, off_n))

    def runlights(self, key, xs, cy):
        """The play-head as in 1.x: a glowing red ring round the current step (sequencer/vst/images/make_led_art.py's ring,
        redrawn at this page's step spacing), one Image film strip on the run parameter (frame k = step k, 0 = none).
        Added before the steps so it sits under their keys and never takes a tap."""
        s = self.skin
        Q = 4
        x0, x1 = xs[0], xs[-1]
        pad = 36
        w, h = int(x1 - x0 + 2 * pad), 60
        frames = []
        for n in range(len(xs) + 1):
            im = Image.new("RGBA", (w * Q, h * Q), (0, 0, 0, 0))
            if n:
                cx, ccy = (xs[n - 1] - x0 + pad) * Q, h * Q // 2
                box = [cx - 26 * Q, ccy - 18 * Q, cx + 26 * Q, ccy + 18 * Q]
                g = Image.new("RGBA", im.size, (0, 0, 0, 0))
                ImageDraw.Draw(g).rounded_rectangle([v + (-4 if i < 2 else 4) * Q for i, v in enumerate(box)], radius=9 * Q, fill=(255, 70, 60, 150))
                im = Image.alpha_composite(im, g.filter(ImageFilter.GaussianBlur(6 * Q)))
                ImageDraw.Draw(im).rounded_rectangle(box, radius=7 * Q, outline=(255, 120, 105, 255), width=3 * Q)
            frames.append(im.resize((w, h), Image.LANCZOS))
        strip = Image.new("RGBA", (w, h * len(frames)), (0, 0, 0, 0))
        for i, f in enumerate(frames):
            strip.paste(f, (0, i * h))
        img = s.save(strip, "run_%s.png" % key)
        b = (x0 - pad, cy - h / 2, w, h)
        self.kids.append(entry(key + " lamps", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": img, "numFrames": len(frames), "handleName": "Data"},
                               b, remap=s.P(key)))
        self.pv.append(("strip", key, b, frames))

    def picker(self, key, cx, cy, label, w=170, h=44, label_pos="above"):
        """A list field: the choice as text in a recessed window, a touch area that opens the list (ui_open = n), the list placed last."""
        s = self.skin
        n = [s.params[j]["popup_of"] for j in s.popups].index(s.keys.index(key)) + 1
        d = ImageDraw.Draw(self.hi)
        box = ((cx - w / 2) * SS, (cy - h / 2) * SS, (cx + w / 2) * SS, (cy + h / 2) * SS)
        d.rounded_rectangle(box, 6 * SS, fill=(8, 8, 9, 255), outline=(110, 110, 114, 255), width=SS)
        ax = cx + w / 2 - 16
        d.polygon([((ax - 7) * SS, (cy - 3) * SS), ((ax + 7) * SS, (cy - 3) * SS), (ax * SS, (cy + 5) * SS)], fill=INK + (255,))
        if label_pos == "left":
            self.text(cx - w / 2 - 10, cy, label, 15, 700, anchor="rm")
        elif label_pos == "below":
            self.text(cx, cy + h / 2 + 11, label, 15)
        else:
            self.text(cx, cy - h / 2 - 16, label, 17)
        self.value(key, (cx - w / 2 + 6, cy - h / 2, w - 30, h), 21.0, (255, 120, 100))
        self.kids.append(entry(key + " field", "Button", {"version": 2, "onImage": s.clear, "offImage": s.clear, "buttonId": n, "numButtonsInGroup": s.ui_steps,
                                                          "handleName": "Data", "gestureBehaviour": GESTURE}, (cx - w / 2, cy - h / 2, w, h), remap=s.P("ui_open")))
        spec = s.spec(key)
        opts = spec["opts"] or [str(v) for v in range(int(spec["min"]), int(spec["max"]) + 1)]   # a numeric range lists its numbers
        ck, (pw, ph, opts) = s.pick_comp(key, opts)
        x = max(36, min(cx - pw / 2, W - 36 - pw))
        y = cy + h / 2 + 6 if cy + h / 2 + 6 + ph < H - 10 else cy - h / 2 - 6 - ph
        self.lists.append(entry("list " + key, ck, {"version": 1, "handleName": "Data"}, (x, max(10, y), pw, ph), remap=s.P(key),
                                vis="IndexedEnabling/%d/%d/%s" % (n, s.ui_steps, s.P("ui_open"))))

    def qgroup(self, box, keys):
        x0, y0, x1, y1 = box
        self.groups.append("%d %d %d %d" % (x0, y0, x1 - x0, y1 - y0))
        self.qrows.append(keys)

    def tab(self):
        im = self.hi.resize((W, H), Image.LANCZOS).convert("RGB")
        self.bgim = im
        name = self.skin.save(im, "bg_%s.png" % self.name.lower().replace(" ", "_"))
        kids = [entry("bg", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": name}, (0, 0, W, H))] + self.kids + self.lists
        return {"version": 1, "tabName": self.tabname, "fnKeyIndex": self.fn, "fnKeySubIndex": self.sub,
                "componentDefinition": component(kids, background=bg("ff0a0a0b")), "qlinkBoundsData": self.groups}

    def preview(self, path, values):
        """The page as the MPC would draw it with default values (a mock-up for review, not used by the device)."""
        im = self.bgim.copy().convert("RGBA")
        d = ImageDraw.Draw(im)
        vf = ImageFont.truetype(os.path.join(FONTS, "TitilliumWeb-SemiBold.ttf"), 20)
        for w in self.pv:
            kind, key = w[0], w[1]
            v, text = values(key)
            if kind == "knob":
                frames = w[3]
                im.alpha_composite(frames[int(round(v * (len(frames) - 1)))], (int(w[2][0]), int(w[2][1])))
            elif kind == "strip":
                frames = w[3]
                im.alpha_composite(frames[min(len(frames) - 1, int(round(v * (len(frames) - 1))))], (int(w[2][0]), int(w[2][1])))
            elif kind == "button":
                im.alpha_composite(Image.open(os.path.join(self.skin.out, w[3])).convert("RGBA"), (int(w[2][0]), int(w[2][1])))
            elif kind == "radio":
                on, off, i = w[3]
                n = self.skin.spec(key)["nopts"]
                cur = int(round(v * (n - 1)))
                im.alpha_composite(Image.open(os.path.join(self.skin.out, on if cur == i else off)).convert("RGBA"), (int(w[2][0]), int(w[2][1])))
            elif kind == "value":
                b = w[2]
                f = ImageFont.truetype(os.path.join(FONTS, "TitilliumWeb-SemiBold.ttf"), int(w[3] * DEVICE_TEXT))
                d.text((b[0] + b[2] / 2, b[1] + b[3] / 2), text, font=f, fill=w[4] + (255,), anchor="mm")
        for g in self.groups:   # the Q-Link frames as the MPC overlays them
            x, y, gw, gh = [int(t) for t in g.split()]
            d.rounded_rectangle((x, y, x + gw, y + gh), 6, outline=(232, 31, 64, 160), width=2)
        im.convert("RGB").save(path)


def qlinks(skin, pages):
    maps = []
    for p in pages:
        q = {"Q-Link %d" % n: -1 for n in range(1, 17)}
        for gi, keys in enumerate(p.qrows[:4]):
            for i, key in enumerate(keys):
                q["Q-Link %d" % (13 - 4 * gi + i)] = skin.keys.index(key)
        maps.append({"Tab": p.fn + 1, "SubTab": p.sub + 1, "Bank Direction": "RowTopDown", "Q-Links": q})
    return {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": maps}, "Program Mode Q-Links": {"Q-Link %d" % n: -1 for n in range(1, 17)}}


def lfo_switches(p, L):
    """The LFO block's lower half: the sync division list, and the sync and retrig switches."""
    p.picker(L + "div", 150, 440, "SYNC DIV", 150)
    p.bat(L + "sync", 290, 410, ["FREE", "SYNC"])
    p.bat(L + "retrig", 290, 530, ["RETRIG OFF", "RETRIG ON"])


# ------------------------------------------------------------------------------------------------ Maze Voice
def voice_pages(s):
    pages = []

    BIG, SMALL = 44, 34   # major and minor knobs (the hardware's two sizes, closer together)

    def cols(p, sections):
        """Sections side by side, each a 2 x 2 block (one Q-Link group). Cells: ("K", key, label[, scale]) a major knob,
        ("k", ...) a minor one, ("bat", key, label, options) a switch."""
        for x0, x1, title, cells in sections:
            p.section(x0, 92, x1, 600, title)
            w = x1 - x0
            cxs, cys = (x0 + w * 0.26, x0 + w * 0.74), (232, 456)
            keys = []
            for ci, cell in enumerate(cells):
                cx, cy = cxs[ci % 2] if len(cells) > 1 else (x0 + x1) / 2, cys[ci // 2] if len(cells) > 1 else cys[ci]
                kind, key = cell[0], cell[1]
                if kind in ("K", "k"):
                    p.knob(key, cx, cy, BIG if kind == "K" else SMALL, cell[2], scale=cell[3] if len(cell) > 3 else None)
                elif kind == "bat":
                    p.text(cx, cy - 78, cell[2], 19)
                    p.bat(key, cx - 22, cy, cell[3], len(cell[3]))
                keys.append(key)
            p.qgroup((x0 + 4, 104, x1 - 4, 596), keys)

    p = Page(s, "OSC", 0, 0, "VOICE")
    p.wordmark(56, 60, "MAZE", "VOICE")
    cols(p, [(52, 428, "VCO", [("K", "vco_tune", "VCO FREQUENCY", ("-24", "+24")), ("k", "vco_eg1", "EG1 AMT", ("-", "+")),
                               ("K", "fm_depth", "MOD > VCO FM"), ("k", "vco_key", "KEY TRACK")]),
             (452, 828, "MOD VCO", [("K", "mod_freq", "MOD VCO FREQ", ("LO", "1.3k")), ("k", "mod_eg1", "EG1 AMT", ("-", "+")),
                                    ("k", "fm_eg1", "FM EG1 AMT", ("-", "+")), ("k", "mod_key", "KEY TRACK")]),
             (852, 1228, "MIXER", [("k", "vco_lvl", "VCO LVL"), ("k", "mod_lvl", "MOD VCO LVL"), ("k", "ring_lvl", "RING MOD LVL"), ("k", "noise_lvl", "NOISE LVL")])])
    pages.append(p)

    p = Page(s, "FILTER", 1, 0, "FOLD/FILTER")
    p.wordmark(56, 60, "MAZE", "VOICE")
    cols(p, [(52, 356, "VCW", [("K", "fold_drive", "VCW FOLD"), ("k", "fold_bias", "VCW BIAS", ("-", "+")),
                               ("k", "fold_eg1", "EG1 AMT", ("-", "+")), ("k", "fold_key", "KEY TRACK")]),
             (548, 852, "VCF", [("K", "cutoff", "VCF CUTOFF", ("20Hz", "20k")), ("K", "reso", "RESONANCE"),
                                ("k", "cutoff_eg1", "EG1 AMT", ("-", "+")), ("k", "cutoff_key", "KEY TRACK")]),
             (876, 1228, "EG & FILTER", [("K", "env1_decay", "EG1 DECAY"), ("K", "env2_decay", "EG2 (VCA) DECAY"),
                                         ("k", "filter_mode", "FILTER MODE", ("LP", "BP")), ("k", "filt_drive", "FILTER DRIVE")])])
    p.section(372, 92, 532, 600, "ROUTE")   # between VCW and VCF, as the signal order it sets
    p.knob("blend", 452, 232, SMALL, "BLEND", scale=("VCW", "VCF"))
    p.bat("route", 404, 456, ["VCW > VCF", "PARALLEL", "VCF > VCW"], 3)
    p.qgroup((376, 104, 528, 596), ["blend", "route"])
    for lst in (p.groups, p.qrows):   # Q-Link banks left to right: VCW, ROUTE, VCF, EG & FILTER
        lst.insert(1, lst.pop())
    pages.append(p)

    p = Page(s, "OUTPUT", 2, 0, "TONE")
    p.wordmark(56, 60, "MAZE", "VOICE")
    p.section(52, 92, 428, 352, "TONE & OUT")
    p.knob("sat", 150, 232, BIG, "TONE / SAT")
    p.knob("level", 330, 232, BIG, "VOLUME")
    p.qgroup((56, 104, 424, 330), ["sat", "level"])
    p.section(452, 92, 828, 352, "NOISE & MODE")
    p.knob("noise_tone", 550, 232, 44, "NOISE TONE", scale=("DARK", "BRIGHT"))
    p.text(730, 154, "OUTPUT MODE", 19)
    p.bat("out_mode", 700, 232, ["INTERNAL", "EXTERNAL"])
    p.qgroup((456, 104, 824, 330), ["noise_tone", "out_mode"])
    p.section(852, 92, 1228, 600, "RANDOMISE")
    for i, (key, lab) in enumerate((("rnd_voice", "VOICE"), ("rnd_wavefolder", "WAVEFOLDER"), ("rnd_filter", "FILTER"), ("rnd_tone", "TONE"))):
        cx, cy = 912 + (i % 2) * 170, 210 + (i // 2) * 150
        p.text(cx + 22, cy - 66, lab, 17)
        p.bat(key, cx, cy, ["OFF", "ON"])
    p.button("rnd_go", 1040, 520, "GENERATE", 54, -48)
    pages.append(p)

    for n in (1, 2):
        p = Page(s, "LFO %d" % n, 3, n - 1, "LFO %d" % n)
        p.wordmark(56, 60, "MAZE", "VOICE")
        L = "lfo%d_" % n
        p.section(52, 92, 428, 600, "LFO %d" % n)
        p.knob(L + "rate", 150, 232, 44, "RATE")
        p.knob(L + "shape", 330, 232, 44, "SHAPE")
        lfo_switches(p, L)
        p.qgroup((56, 104, 424, 596), [L + "rate", L + "shape", L + "div", L + "sync"])
        dests = [("vco_pitch", "VCO PITCH"), ("mod_pitch", "MOD PITCH"), ("fm_depth", "FM DEPTH"), ("cutoff", "CUTOFF"),
                 ("env1_decay", "EG1 DECAY"), ("env2_decay", "EG2 DECAY"), ("filt_drive", "FILT DRIVE"), ("fold_amt", "FOLD AMT"),
                 ("fold_bias", "FOLD BIAS")]
        p.section(452, 92, 1228, 600, "DESTINATIONS")
        for row in range(3):
            chunk = dests[row * 4:row * 4 + 4]
            for i, (k, lab) in enumerate(chunk):
                p.knob(L + k, 546 + i * 190, 186 + row * 166, 32, lab, cap="black")
            if chunk:
                p.qgroup((462, 120 + row * 166, 462 + 190 * len(chunk) + 30, 266 + row * 166), [L + k for k, _ in chunk])
        pages.append(p)
    return pages


# ------------------------------------------------------------------------------------------------ Maze Sequencer
def seq_pages(s):
    pages = []
    p = Page(s, "SEQUENCER", 0)
    p.wordmark(56, 60, "MAZE", "SEQUENCER")
    p.section(52, 92, 908, 600, "SEQUENCERS")
    xs = [478 + i * 55 for i in range(8)]
    for L, cy, name in (("s1_", 218, "SEQ 1"), ("s2_", 468, "SEQ 2")):
        sy = cy + 2   # the step row sits a little above the knobs' centre line, leaving room for the buttons below it
        p.text(xs[0] - 14, cy - 30, name, 15, 700, DIM, "lm")
        p.knob(L + "corrupt", 134, cy - 2, 44, "CORRUPT")
        p.knob(L + "cv_range", 262, cy - 2, 44, "CV RANGE")
        p.knob(L + "length", 374, cy - 2, 30, "LENGTH")
        for i, x in enumerate(xs):   # step numbers under each row's keys
            p.text(x, sy + 28, str(i + 1), 15, 700, DIM)
        p.runlights(L + "run", xs, sy)
        for i, x in enumerate(xs):
            p.step(L + "step%d" % i, x, sy)
        for key, lab, bx in ((L + "adv", "ADVANCE", xs[1]), (L + "regen", "RESET", xs[3])):   # labels under their buttons
            p.button(key, bx, cy + 84, "", 38)
            p.text(bx, cy + 113, lab, 15, 700)
        p.picker(L + "channel", xs[6] + 8, cy + 84, "MIDI CH", 104, 38, label_pos="below")   # a pop-up list of 1-16
        p.qgroup((62, cy - 104, 898, cy + 128), [L + "corrupt", L + "cv_range", L + "length", L + "channel"])
    p.section(932, 92, 1228, 600, "OUTPUT")
    p.knob("trig_mix", 1009, 186, 44, "EG TRIG MIX", scale=("S1", "S2"))
    p.knob("transpose", 1151, 186, 44, "TRANSPOSE", scale=("-48", "+48"))
    p.knob("note_rate", 1009, 336, 30, "NOTE RATE")
    p.knob("note_length", 1151, 336, 30, "NOTE LENGTH")
    p.qgroup((938, 104, 1222, 400), ["trig_mix", "transpose", "note_rate", "note_length"])
    p.picker("scale", 1014, 478, "SCALE", 136)
    p.picker("key", 1155, 478, "KEY", 118)
    p.keys("g_reset", 1080, 568, 50, 32, "RESET EVERY (BARS)")
    pages.append(p)

    for n in (1, 2):
        p = Page(s, "LFO %d" % n, 1, n - 1, "LFO %d" % n)
        p.wordmark(56, 60, "MAZE", "SEQUENCER")
        L = "lfo%d_" % n
        p.section(52, 92, 428, 600, "LFO %d" % n)
        p.knob(L + "rate", 150, 232, 44, "RATE")
        p.knob(L + "shape", 330, 232, 44, "SHAPE")
        lfo_switches(p, L)
        p.qgroup((56, 104, 424, 596), [L + "rate", L + "shape", L + "div", L + "sync"])
        p.section(452, 92, 1228, 600, "DESTINATIONS")
        rows = [[("corrupt1", "SEQ1 CORRUPT"), ("range1", "SEQ1 RANGE"), ("length1", "SEQ1 LENGTH"), ("trigmix", "TRIG MIX")],
                [("corrupt2", "SEQ2 CORRUPT"), ("range2", "SEQ2 RANGE"), ("length2", "SEQ2 LENGTH"), ("notelen", "NOTE LENGTH")]]
        for ri, row in enumerate(rows):
            for i, (k, lab) in enumerate(row):
                p.knob(L + k, 546 + i * 190, 222 + ri * 220, 34, lab, scale=("-", "+"))
            p.qgroup((462, 120 + ri * 220, 1218, 320 + ri * 220), [L + k for k, _ in row])
        pages.append(p)
    return pages


def build(name, preview=False):
    global PANEL_ART, KNOB_ART
    old = os.path.join(ROOT, "vst" if name == "voice" else "sequencer/vst", "images")
    PANEL_ART, KNOB_ART = os.path.join(old, "panel_bg.png"), os.path.join(old, "moog_knob_voice.png")
    _fonts.clear()
    s = Skin(name)
    pages = voice_pages(s) if name == "voice" else seq_pages(s)
    tabs = [p.tab() for p in pages]
    used = set().union(*[p.used for p in pages]) | {k for p in pages for k in [w[1] for w in p.pv]}
    tui = {"pageData": {"version": 1, "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": s.defs},
                        "replacementStrings": s.replacement_strings(used), "info": {"version": 1, "type": "CompleteDescription"}, "tabs": tabs}}
    open(os.path.join(s.out, "TUI.json"), "w").write(dump(tui) + "\n")
    open(os.path.join(s.out, "Q-Links.json"), "w").write(dump(qlinks(s, pages)) + "\n")
    json.load(open(os.path.join(s.out, "TUI.json")))
    bound = {k for p in pages for w in p.pv for k in [w[1]]}
    missing = [k for i, k in enumerate(s.keys) if k not in bound and i not in s.popups]
    print("%s: %d tabs, %d widgets, %d files; not on any page: %s" % (name, len(tabs), sum(len(p.kids) + len(p.lists) for p in pages), len(os.listdir(s.out)), missing or "none"))
    if preview:
        pdir = os.path.join(ROOT, "docs", "skin-preview")
        os.makedirs(pdir, exist_ok=True)

        def values(key):
            sp = s.spec(key)
            n = sp["nopts"]
            v = sp["def"]
            if n:
                i = int(round(v * (n - 1)))
                return (i / (n - 1) if n > 1 else 0), sp["opts"][i]
            plain = sp["min"] + (sp["max"] - sp["min"]) * v
            return v, ("%.0f" % plain if abs(sp["max"] - sp["min"]) > 20 or sp.get("int_display") else "%.1f" % plain)
        for p in pages:
            p.preview(os.path.join(pdir, "%s-%s.png" % (name, p.name.lower().replace(" ", ""))), values)
        print("previews in docs/skin-preview/")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    for n in args or ["voice", "sequencer"]:
        build(n, "--preview" in sys.argv)
