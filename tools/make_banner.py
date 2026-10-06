#!/usr/bin/env python3
# /// script
# dependencies = ["cairosvg", "pillow"]
# ///
"""Builds docs/images/nekophoto.png, the README banner, from the logo (packaging/nekophoto.svg) and typeset text.
No generated imagery: the logo is the traced mascot vector, the lettering is Noto Sans CJK JP (SIL Open Font
License). Usage: uv run tools/make_banner.py [output.png]
       uv run tools/make_banner.py --welcome [output.jpg]   (the first-launch Welcome screen's picture,
       src/app/images/welcome.jpg: its pages pan a band at 42% of the height, so the logos run along it)"""
import io, os, sys
import cairosvg
from PIL import Image, ImageDraw, ImageFont

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
welcome = "--welcome" in sys.argv
args = [a for a in sys.argv[1:] if a != "--welcome"]
out = args[0] if args else os.path.join(root, "src", "app", "images", "welcome.jpg") if welcome else os.path.join(root, "docs", "images", "nekophoto.png")
W, H = 1280, 640
PINK_BG, PINK, INK, WHITE = (252, 232, 238), (240, 30, 120), (24, 24, 28), (255, 255, 255)

def font(weight, size):
    for path in (f"/usr/share/fonts/noto-cjk/NotoSansCJK-{weight}.ttc", f"/usr/share/fonts/opentype/noto/NotoSansCJK-{weight}.ttc"):
        if os.path.exists(path): return ImageFont.truetype(path, size, index=0)   # index 0: JP
    raise SystemExit("Noto Sans CJK (" + weight + ") is needed")

def logo_image(px):
    return Image.open(io.BytesIO(cairosvg.svg2png(url=os.path.join(root, "packaging", "nekophoto.svg"), output_width=px, output_height=px))).convert("RGBA")

if welcome:
    import math
    W, H = 1400, 788
    SKY = (200, 232, 248)
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):   # pink at the top easing into sky blue at the bottom
        t = max(0.0, min(1.0, (y / H - 0.35) / 0.65)) ** 1.4
        c = tuple(round(PINK_BG[k] + (SKY[k] - PINK_BG[k]) * t) for k in range(3))
        for x in range(W): px[x, y] = c
    d = ImageDraw.Draw(img, "RGBA")
    def paw(cx, cy, r, colour):
        d.ellipse((cx - r, cy - r * 0.8, cx + r, cy + r * 0.9), fill=colour)
        for dx, dy in ((-1.05, -1.15), (-0.38, -1.6), (0.38, -1.6), (1.05, -1.15)):
            tr = r * 0.38
            d.ellipse((cx + dx * r - tr, cy + dy * r - tr, cx + dx * r + tr, cy + dy * r + tr), fill=colour)
    for i in range(14):   # a trail of paw prints along the bottom
        x = 60 + i * 100
        paw(x, 690 + (22 if i % 2 else -22) + 18 * math.sin(i * 0.7), 20, (240, 30, 120, 70))
    fn, ft = font("Black", 118), font("Bold", 36)
    name, tag = "NekoPhoto", "みんなのためのエディタ"
    nb = d.textbbox((0, 0), name, font=fn)
    tb = d.textbbox((0, 0), tag, font=ft)
    d.text(((W - (nb[2] - nb[0])) // 2 - nb[0], 40 - nb[1]), name, font=fn, fill=PINK)
    pad_x, pad_y = 24, 11
    pw, ph = (tb[2] - tb[0]) + 2 * pad_x, (tb[3] - tb[1]) + 2 * pad_y
    py = 40 + (nb[3] - nb[1]) + 22
    d.rounded_rectangle(((W - pw) // 2, py, (W + pw) // 2, py + ph), radius=ph // 2, fill=INK)
    d.text(((W - pw) // 2 + pad_x - tb[0], py + pad_y - tb[1]), tag, font=ft, fill=WHITE)
    band = int(H * 0.42) + 40   # the logos' centre line, inside the band the Welcome pages slice
    for i, (x, size) in enumerate(((150, 210), (420, 250), (700, 290), (980, 250), (1250, 210))):
        logo = logo_image(size)
        img.paste(logo, (x - size // 2, band - size // 2 + (12 if i % 2 else 0)), logo)
    img.save(out, quality=92, optimize=True)
    print("wrote", out)
    raise SystemExit

banner = Image.new("RGB", (W, H), PINK_BG)
draw = ImageDraw.Draw(banner)
name, tag = "NekoPhoto", "みんなのためのエディタ"
logo_px, gap, margin = 400, 50, 90
# The name at the largest size that fits beside the logo, the tagline in proportion.
size = 160
while True:
    fn = font("Black", size)
    nb = draw.textbbox((0, 0), name, font=fn)
    if nb[2] - nb[0] <= W - 2 * margin - logo_px - gap or size <= 60: break
    size -= 2
ft = font("Bold", max(28, size * 30 // 100))
tb = draw.textbbox((0, 0), tag, font=ft)
pad_x, pad_y = 26, 13
text_w = max(nb[2] - nb[0], (tb[2] - tb[0]) + 2 * pad_x)
x0 = (W - (logo_px + gap + text_w)) // 2          # the whole group centred
logo = logo_image(logo_px)
banner.paste(logo, (x0, (H - logo_px) // 2), logo)
x = x0 + logo_px + gap
name_h, tag_h = nb[3] - nb[1], (tb[3] - tb[1]) + 2 * pad_y
top = (H - (name_h + 28 + tag_h)) // 2
draw.text((x - nb[0], top - nb[1]), name, font=fn, fill=PINK)
ty = top + name_h + 28
pill = (x + 2, ty, x + 2 + (tb[2] - tb[0]) + 2 * pad_x, ty + tag_h)
draw.rounded_rectangle(pill, radius=tag_h // 2, fill=INK)
draw.text((pill[0] + pad_x - tb[0], pill[1] + pad_y - tb[1]), tag, font=ft, fill=WHITE)
banner.save(out, optimize=True)
print("wrote", out)
