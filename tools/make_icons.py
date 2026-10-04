"""Draws the app icons (web/icon-*.png) to match web/icon.svg. Needs Pillow."""
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")


def icon(size, maskable=False, corner=True):
    s = size / 64.0
    big = Image.new("RGBA", (size * 4, size * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    k = s * 4
    pad = 6 if maskable else 0  # keep the drawing inside the safe zone for maskable icons
    if corner and not maskable:
        d.rounded_rectangle([0, 0, 64 * k, 64 * k], radius=14 * k, fill="#f4b13d")
    else:
        d.rectangle([0, 0, 64 * k, 64 * k], fill="#f4b13d")

    def P(x, y):
        c = 32
        return ((c + (x - c) * (64 - 2 * pad) / 64) * k, (c + (y - c) * (64 - 2 * pad) / 64) * k)

    back = Image.new("RGBA", big.size, (0, 0, 0, 0))
    ImageDraw.Draw(back).rounded_rectangle([*P(14, 16), *P(50, 46)], radius=3 * k, fill="#e9dcc0")
    back = back.rotate(8, center=P(32, 31), resample=Image.BICUBIC)
    big.alpha_composite(back)
    d.rounded_rectangle([*P(14, 16), *P(50, 46)], radius=3 * k, fill="#fffaf0", outline="#4a3104", width=int(2.5 * k))
    pts = []
    for i in range(41):
        t = i / 40
        x = 20 + 24 * t
        y = 38 - 14 * t + 7 * __import__("math").sin(t * 6.28)
        pts.append(P(x, y))
    d.line(pts, fill="#e22c20", width=int(3 * k), joint="curve")
    cx, cy = P(24, 24)
    d.ellipse([cx - 3 * k, cy - 3 * k, cx + 3 * k, cy + 3 * k], fill="#2850dc")
    d.line([P(14, 50), P(50, 50)], fill="#4a3104", width=int(3 * k))
    return big.resize((size, size), Image.LANCZOS)


icon(192).save(os.path.join(OUT, "icon-192.png"))
icon(512).save(os.path.join(OUT, "icon-512.png"))
icon(512, maskable=True).save(os.path.join(OUT, "icon-maskable-512.png"))
icon(180, corner=False).convert("RGB").save(os.path.join(OUT, "apple-touch-icon.png"))
print("icons written")
