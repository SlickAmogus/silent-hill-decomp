# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the package's tile and splash images from the desktop icon.

usage: make_logos.py <app_icon.ico> <out dir>
"""
import os
import sys

from PIL import Image

# name -> (canvas width, canvas height, icon edge)
LOGOS = {
    "StoreLogo.png": (50, 50, 50),
    "Square44x44Logo.png": (44, 44, 44),
    "Square150x150Logo.png": (150, 150, 110),
    "Square310x310Logo.png": (310, 310, 220),
    "Wide310x150Logo.png": (310, 150, 120),
    "SplashScreen.png": (620, 300, 200),
}


def main():
    ico_path, out_dir = sys.argv[1], sys.argv[2]
    ico = Image.open(ico_path)
    ico.size = max(ico.info["sizes"])
    icon = ico.convert("RGBA")

    os.makedirs(out_dir, exist_ok=True)
    for name, (w, h, edge) in LOGOS.items():
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 255))
        scaled = icon.resize((edge, edge), Image.LANCZOS)
        canvas.paste(scaled, ((w - edge) // 2, (h - edge) // 2), scaled)
        canvas.save(os.path.join(out_dir, name))


if __name__ == "__main__":
    main()
