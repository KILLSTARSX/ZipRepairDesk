from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
S = 1024
image = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# Indigo-to-blue rounded tile with a restrained shadow.
shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sd = ImageDraw.Draw(shadow)
sd.rounded_rectangle((72, 82, 952, 962), radius=208, fill=(30, 41, 59, 58))
tile_mask = Image.new("L", (S, S), 0)
md = ImageDraw.Draw(tile_mask)
md.rounded_rectangle((56, 48, 944, 936), radius=208, fill=255)
gradient = Image.new("RGBA", (S, S))
gp = gradient.load()
for y in range(S):
    for x in range(S):
        t = min(1.0, max(0.0, (x * 0.35 + y * 0.65 - 90) / 1100))
        gp[x, y] = (
            round(99 * (1 - t) + 37 * t),
            round(102 * (1 - t) + 99 * t),
            round(241 * (1 - t) + 235 * t),
            255,
        )
image.alpha_composite(shadow)
image.paste(gradient, (0, 0), tile_mask)
d = ImageDraw.Draw(image)

# Soft top highlight.
d.rounded_rectangle((88, 76, 912, 460), radius=180, fill=(255, 255, 255, 16))

# Document/archive body and folded corner.
d.rounded_rectangle((254, 196, 724, 818), radius=72, fill=(255, 255, 255, 255))
d.polygon([(566, 196), (724, 196), (724, 354)], fill=(224, 231, 255, 255))
d.polygon([(566, 196), (566, 354), (724, 354)], fill=(238, 242, 255, 255))

# Zipper teeth and pull tab.
zip_color = (79, 70, 229, 255)
for y in range(386, 578, 48):
    d.rounded_rectangle((459, y, 519, y + 28), radius=9, fill=zip_color)
d.rounded_rectangle((429, 574, 549, 690), radius=34, outline=zip_color, width=24)
d.rounded_rectangle((474, 608, 504, 722), radius=15, fill=zip_color)

# Restored/check badge.
d.ellipse((574, 574, 858, 858), fill=(255, 255, 255, 255))
d.ellipse((594, 594, 838, 838), fill=(34, 197, 94, 255))
d.line([(650, 716), (700, 764), (790, 660)], fill=(255, 255, 255, 255), width=34, joint="curve")
d.ellipse((633, 699, 667, 733), fill=(255, 255, 255, 255))
d.ellipse((683, 747, 717, 781), fill=(255, 255, 255, 255))
d.ellipse((773, 643, 807, 677), fill=(255, 255, 255, 255))

image.save(ROOT / "logo.png")
image.save(ROOT / "ZipRepairDesk.ico", sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
print(ROOT / "ZipRepairDesk.ico")
