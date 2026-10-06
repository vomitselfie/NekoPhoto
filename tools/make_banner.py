#!/usr/bin/env python3
# /// script
# dependencies = ["cairosvg", "pillow"]
# ///
"""Builds docs/images/nekophoto.png, the README banner, from the logo (packaging/nekophoto.svg) and typeset text.
No generated imagery: the logo is the traced mascot vector, the lettering is Noto Sans CJK JP (SIL Open Font
License). Usage: uv run tools/make_banner.py [output.png]"""
import io, os, sys
import cairosvg
from PIL import Image, ImageDraw, ImageFont

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "docs", "images", "nekophoto.png")
W, H = 1280, 640
PINK_BG, PINK, INK, WHITE = (252, 232, 238), (240, 30, 120), (24, 24, 28), (255, 255, 255)

def font(weight, size):
    for path in (f"/usr/share/fonts/noto-cjk/NotoSansCJK-{weight}.ttc", f"/usr/share/fonts/opentype/noto/NotoSansCJK-{weight}.ttc"):
        if os.path.exists(path): return ImageFont.truetype(path, size, index=0)   # index 0: JP
    raise SystemExit("Noto Sans CJK (" + weight + ") is needed")

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
logo = Image.open(io.BytesIO(cairosvg.svg2png(url=os.path.join(root, "packaging", "nekophoto.svg"), output_width=logo_px, output_height=logo_px))).convert("RGBA")
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
