"""The Vita's bubble and LiveArea for the port: original art, from the same
"B3" letters as the PC icon (port/tools/make_icon_art.py: polygons, no font,
nothing of the game's) on a night road of the port's own - a dark ground,
a vanishing road and streaks of speed. Written to port/vita/sce_sys/:

    icon0.png                          128 x 128, the bubble
    livearea/contents/bg.png           840 x 500, the LiveArea's background
    livearea/contents/startup.png      280 x 158, the gate's
    livearea/contents/template.xml

As 8-bit palette PNGs, as the Vita's LiveArea wants them.

    python games/burnout3/port/vita/tools/make_livearea.py [--preview out.png]
"""

import argparse
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "tools")))
import make_icon_art  # noqa: E402  (the port's own "B3" letters)

OUT = os.path.normpath(os.path.join(HERE, "..", "sce_sys"))
SUPER = 4

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def night_road(width, height, seed, horizon=0.42, letters=None):
    """A dark ground: sky to road, a road narrowing to a vanishing point,
    its edge lines and dashes, and streaks of light leaving the point;
    `letters` (a size, a centre) drawn over it."""
    w, h = width * SUPER, height * SUPER
    image = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(image)
    hy = int(h * horizon)
    for y in range(h):
        if y < hy:          # the sky: deep blue to a warm glow at the horizon
            t = y / hy
            colour = (int(10 + 70 * t ** 3), int(12 + 18 * t ** 3), int(30 + 30 * t ** 2))
        else:               # the ground: dark, darkest at the foot
            t = (y - hy) / (h - hy)
            colour = (int(26 - 18 * t), int(20 - 14 * t), int(30 - 20 * t))
        d.line([(0, y), (w, y)], fill=colour)
    vx = w // 2
    # The road and its edges, from the vanishing point to beyond the foot.
    road = [(vx - w * 0.01, hy), (vx + w * 0.01, hy), (vx + w * 0.75, h), (vx - w * 0.75, h)]
    d.polygon(road, fill=(34, 32, 40))
    for side in (-1, 1):
        d.line([(vx + side * w * 0.01, hy), (vx + side * w * 0.72, h)], fill=(220, 210, 190), width=max(1, w // 300))
    # The centre's dashes, longer as they near.
    z = 1.0
    while z > 0.02:
        y0, y1 = hy + (h - hy) * z ** 2.2, hy + (h - hy) * (z * 0.93) ** 2.2
        half = max(1, int(w * 0.006 * z))
        d.polygon([(vx - half, y0), (vx + half, y0), (vx + half * 0.9, y1), (vx - half * 0.9, y1)], fill=(240, 200, 80))
        z *= 0.82
    # Streaks of light from the vanishing point: warm, a few cool.
    streaks = Image.new("RGB", (w, h))
    sd = ImageDraw.Draw(streaks)
    rng = random.Random(seed)
    for _ in range(46):
        angle = rng.uniform(-math.pi * 0.92, -math.pi * 0.08) if rng.random() < 0.5 else rng.uniform(0.15, math.pi - 0.15)
        near = rng.uniform(0.25, 0.6) * w
        far = near + rng.uniform(0.15, 0.5) * w
        colour = (255, rng.randint(90, 160), 40) if rng.random() < 0.75 else (90, 170, 255)
        a = (vx + math.cos(angle) * near, hy + math.sin(angle) * near * 0.55)
        b = (vx + math.cos(angle) * far, hy + math.sin(angle) * far * 0.55)
        sd.line([a, b], fill=colour, width=max(1, int(w / 400 * rng.uniform(0.6, 1.6))))
    streaks = streaks.filter(ImageFilter.GaussianBlur(w / 500))
    image = Image.composite(streaks, image, streaks.convert("L").point(lambda v: min(255, v * 2)))
    image = image.resize((width, height), Image.LANCZOS).convert("RGBA")
    if letters:     # drawn at their own size (make_icon_art supersamples them itself)
        size, (cx, cy) = letters
        art = make_icon_art.draw(size)
        image.alpha_composite(art, (int(cx * width - art.width / 2), int(cy * height - art.height / 2)))
    return image


def palette(image):
    """8-bit palette, as the LiveArea takes it."""
    return image.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", help="also write the three pictures side by side as one PNG")
    args = parser.parse_args()
    icon = Image.new("RGBA", (128, 128))
    icon.alpha_composite(night_road(128, 128, seed=3, horizon=0.5))
    letters = make_icon_art.draw(112)
    icon.alpha_composite(letters, (8, 8))
    bg = night_road(840, 500, seed=1, letters=(300, (0.5, 0.38)))
    startup = night_road(280, 158, seed=2, letters=(120, (0.5, 0.42)))
    contents = os.path.join(OUT, "livearea", "contents")
    os.makedirs(contents, exist_ok=True)
    palette(icon).save(os.path.join(OUT, "icon0.png"), optimize=True)
    palette(bg).save(os.path.join(contents, "bg.png"), optimize=True)
    palette(startup).save(os.path.join(contents, "startup.png"), optimize=True)
    with open(os.path.join(contents, "template.xml"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(TEMPLATE)
    print("wrote", OUT)
    if args.preview:
        sheet = Image.new("RGB", (840 + 280 + 128 + 40, 500 + 20), (128, 128, 128))
        sheet.paste(bg.convert("RGB"), (10, 10))
        sheet.paste(startup.convert("RGB"), (860, 10))
        sheet.paste(icon.convert("RGB"), (1150, 10))
        sheet.save(args.preview)
        print("preview", args.preview)


if __name__ == "__main__":
    main()
