# Generates assets/jolt.ico and assets/jolt-256.png (folder with a lightning bolt). Requires Pillow.
#   python -I assets/make_icon.py
import math, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8  # supersampling

def arc(cx, cy, r, a0, a1, n=24):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]

def folder(x0, y0, x1, y1, r, tab_w, tab_h):
    """Outline in 0..256 units; the tab takes up the top left."""
    p = []
    p += arc(x0 + r, y0 + r, r, 180, 270)
    p += [(x0 + tab_w, y0), (x0 + tab_w + tab_h, y0 + tab_h)]
    p += arc(x1 - r, y0 + tab_h + r, r, 270, 360)
    p += arc(x1 - r, y1 - r, r, 0, 90)
    p += arc(x0 + r, y1 - r, r, 90, 180)
    return p

BOLT = [(150, 72), (84, 152), (124, 152), (106, 216), (178, 128), (138, 128)]

def scale(pts, cx, cy, k):
    return [(cx + (x - cx) * k, cy + (y - cy) * k) for x, y in pts]

def gradient(n):
    a, b = (0xFF, 0xD8, 0x4D), (0xFF, 0x7A, 0x1A)
    t = Image.linear_gradient("L").resize((n, n))           # 0 at the top, 255 at the bottom
    t2 = t.rotate(90)                                        # horizontal
    mix = Image.blend(t, t2, 0.5)
    channels = [mix.point(lambda v, c0=a[i], c1=b[i]: int(c0 + (c1 - c0) * v / 255)) for i in range(3)]
    return Image.merge("RGB", channels)

def draw(n):
    N = n * SS
    s = N / 256
    small = n <= 24
    if small:   # at small sizes: full-width folder, simple tab and thicker bolt
        shape = folder(0, 26, 256, 234, 26, 104, 28)
        bolt = scale(BOLT, 130, 146, 1.16)
    elif n <= 48:
        shape = folder(6, 34, 250, 232, 25, 96, 26)
        bolt = scale(BOLT, 130, 146, 1.08)
    else:
        shape = folder(12, 40, 244, 230, 24, 88, 26)
        bolt = BOLT
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).polygon([(x * s, y * s) for x, y in shape], fill=255)
    hole = Image.new("L", (N, N), 0)
    ImageDraw.Draw(hole).polygon([(x * s, y * s) for x, y in bolt], fill=255)
    img = gradient(N).convert("RGBA")
    dark = Image.new("RGBA", (N, N), (0x17, 0x18, 0x1F, 255))
    img = Image.composite(dark, img, hole)
    img.putalpha(m)
    return img.resize((n, n), Image.LANCZOS)

imgs = [draw(n) for n in SIZES]
imgs[-1].save(os.path.join(HERE, "jolt-256.png"))
imgs[-1].save(os.path.join(HERE, "jolt.ico"), format="ICO",
              sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])

# contact sheet: each size at real scale, plus the small ones enlarged x6
sheet = Image.new("RGBA", (760, 300), (32, 33, 40, 255))
x = 10
for im in imgs:
    sheet.alpha_composite(im, (x, 10)); x += im.width + 10
x = 10
for im in imgs[:4]:
    z = im.resize((im.width * 6, im.height * 6), Image.NEAREST)
    sheet.alpha_composite(z, (x, 280 - z.height)); x += z.width + 14
sheet.save(os.environ.get("SHEET", os.path.join(HERE, "..", "build", "icon-sheet.png")))
