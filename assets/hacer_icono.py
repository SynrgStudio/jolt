# Genera assets/jolt.ico y assets/jolt-256.png (carpeta con rayo). Requiere Pillow.
#   python -I assets/hacer_icono.py
import math, os
from PIL import Image, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))
TAMANOS = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8  # sobremuestreo

def arco(cx, cy, r, a0, a1, n=24):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]

def carpeta(x0, y0, x1, y1, r, solapa_w, solapa_h):
    """Contorno en unidades 0..256; la solapa ocupa la parte superior izquierda."""
    p = []
    p += arco(x0 + r, y0 + r, r, 180, 270)
    p += [(x0 + solapa_w, y0), (x0 + solapa_w + solapa_h, y0 + solapa_h)]
    p += arco(x1 - r, y0 + solapa_h + r, r, 270, 360)
    p += arco(x1 - r, y1 - r, r, 0, 90)
    p += arco(x0 + r, y1 - r, r, 90, 180)
    return p

RAYO = [(150, 72), (84, 152), (124, 152), (106, 216), (178, 128), (138, 128)]

def escalar(pts, cx, cy, k):
    return [(cx + (x - cx) * k, cy + (y - cy) * k) for x, y in pts]

def degradado(n):
    a, b = (0xFF, 0xD8, 0x4D), (0xFF, 0x7A, 0x1A)
    t = Image.linear_gradient("L").resize((n, n))           # 0 arriba, 255 abajo
    t2 = t.rotate(90)                                        # horizontal
    mezcla = Image.blend(t, t2, 0.5)
    canales = [mezcla.point(lambda v, c0=a[i], c1=b[i]: int(c0 + (c1 - c0) * v / 255)) for i in range(3)]
    return Image.merge("RGB", canales)

def dibujar(n):
    N = n * SS
    s = N / 256
    chico = n <= 24
    if chico:   # en tamanos chicos: carpeta a todo lo ancho, solapa simple y rayo mas grueso
        forma = carpeta(0, 26, 256, 234, 26, 104, 28)
        rayo = escalar(RAYO, 130, 146, 1.16)
    elif n <= 48:
        forma = carpeta(6, 34, 250, 232, 25, 96, 26)
        rayo = escalar(RAYO, 130, 146, 1.08)
    else:
        forma = carpeta(12, 40, 244, 230, 24, 88, 26)
        rayo = RAYO
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).polygon([(x * s, y * s) for x, y in forma], fill=255)
    hueco = Image.new("L", (N, N), 0)
    ImageDraw.Draw(hueco).polygon([(x * s, y * s) for x, y in rayo], fill=255)
    img = degradado(N).convert("RGBA")
    oscuro = Image.new("RGBA", (N, N), (0x17, 0x18, 0x1F, 255))
    img = Image.composite(oscuro, img, hueco)
    img.putalpha(m)
    return img.resize((n, n), Image.LANCZOS)

imgs = [dibujar(n) for n in TAMANOS]
imgs[-1].save(os.path.join(AQUI, "jolt-256.png"))
imgs[-1].save(os.path.join(AQUI, "jolt.ico"), format="ICO",
              sizes=[(n, n) for n in TAMANOS], append_images=imgs[:-1])

# hoja de control: cada tamano a escala real y ampliado x6 los chicos
hoja = Image.new("RGBA", (760, 300), (32, 33, 40, 255))
x = 10
for im in imgs:
    hoja.alpha_composite(im, (x, 10)); x += im.width + 10
x = 10
for im in imgs[:4]:
    z = im.resize((im.width * 6, im.height * 6), Image.NEAREST)
    hoja.alpha_composite(z, (x, 280 - z.height)); x += z.width + 14
hoja.save(os.environ.get("HOJA", os.path.join(AQUI, "..", "build", "icono-hoja.png")))
