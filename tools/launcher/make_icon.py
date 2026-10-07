#!/usr/bin/env python3
"""make_icon.py <out.iconset> [preview.png]: original tf2mt app icon (no Valve artwork).
Rounded square on the Apple icon grid, warm TF2-style colours, "tf2" + "mt" set in Futura."""
import os, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FUT = '/System/Library/Fonts/Supplemental/Futura.ttc'
def futura(size, style):
    for i in range(8):
        try:
            f = ImageFont.truetype(FUT, size, index=i)
            if f.getname()[1] == style: return f
        except OSError: break
    return ImageFont.truetype(FUT, size)

S = 1024; R = 824; O = (S - R) // 2
icon = Image.new('RGBA', (S, S), (0, 0, 0, 0))
# body: vertical gradient brown → near-black
body = Image.new('RGBA', (R, R))
d = ImageDraw.Draw(body)
for y in range(R):
    t = y / R
    d.line([(0, y), (R, y)], fill=(int(74 - 50 * t), int(44 - 30 * t), int(34 - 24 * t), 255))
# diagonal orange band (TF2 orange #CF6A32 → lighter)
band = Image.new('RGBA', (R, R), (0, 0, 0, 0)); bd = ImageDraw.Draw(band)
bd.polygon([(0, R * 0.62), (R, R * 0.30), (R, R * 0.47), (0, R * 0.79)], fill=(207, 106, 50, 255))
body = Image.alpha_composite(body, band)
td = ImageDraw.Draw(body)
big = futura(400, 'Condensed ExtraBold'); small = futura(170, 'Condensed ExtraBold')
cream = (236, 227, 203, 255)
def shadowed(draw_xy, text, font, fill):
    sh = Image.new('RGBA', (R, R), (0, 0, 0, 0)); ImageDraw.Draw(sh).text(draw_xy, text, font=font, fill=(0, 0, 0, 170))
    return sh.filter(ImageFilter.GaussianBlur(10)), (draw_xy, text, font, fill)
w = td.textlength('tf2', font=big)
s1, a = shadowed(((R - w) / 2 - 20, 120), 'tf2', big, cream)
body = Image.alpha_composite(body, s1); ImageDraw.Draw(body).text(*a[:1], a[1], font=a[2], fill=a[3])
s2, b = shadowed((R - 300, R - 300), 'mt', small, (255, 255, 255, 255))
body = Image.alpha_composite(body, s2); ImageDraw.Draw(body).text(*b[:1], b[1], font=b[2], fill=b[3])
mask = Image.new('L', (R, R), 0); ImageDraw.Draw(mask).rounded_rectangle((0, 0, R - 1, R - 1), radius=185, fill=255)
icon.paste(body, (O, O), mask)

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
for s in (16, 32, 128, 256, 512):
    icon.resize((s, s), Image.LANCZOS).save(f'{out}/icon_{s}x{s}.png')
    icon.resize((2 * s, 2 * s), Image.LANCZOS).save(f'{out}/icon_{s}x{s}@2x.png')
if len(sys.argv) > 2: icon.save(sys.argv[2])
