"""The engine tests' Vita bubble: engine/platform/vita/tests_sce_sys/icon0.png
(128 x 128, 8-bit palette as the Vita wants it). Original art: a grid of
squares, one lit.

    python engine/platform/tools/make_vita_test_art.py
"""

import os

from PIL import Image, ImageDraw


def main() -> None:
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vita", "tests_sce_sys")
    os.makedirs(out_dir, exist_ok=True)
    image = Image.new("RGB", (128, 128), (22, 28, 44))
    draw = ImageDraw.Draw(image)
    for row in range(4):
        for column in range(4):
            x, y = 14 + column * 26, 14 + row * 26
            lit = (row, column) == (1, 2)
            colour = (255, 196, 60) if lit else (70, 96, 140) if (row + column) % 2 else (52, 72, 108)
            draw.rounded_rectangle((x, y, x + 20, y + 20), radius=4, fill=colour)
    image.convert("P", palette=Image.Palette.ADAPTIVE, colors=64).save(os.path.join(out_dir, "icon0.png"), bits=8)
    print("wrote", os.path.normpath(os.path.join(out_dir, "icon0.png")))


if __name__ == "__main__":
    main()
