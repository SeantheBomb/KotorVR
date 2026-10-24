"""Tile a sim run's screenshots into one labelled contact sheet.

usage: python contact_sheet.py <run_dir> [suffix=eye0] [cols=4]
Writes <run_dir>/contact_<suffix>.png
"""
import pathlib
import sys

from PIL import Image, ImageDraw

run = pathlib.Path(sys.argv[1])
suffix = sys.argv[2] if len(sys.argv) > 2 else "eye0"
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
shots = sorted(p for p in run.glob(f"*_{suffix}.png") if not p.name.startswith("contact"))
if not shots:
    sys.exit(f"no *_{suffix}.png in {run}")
tw, th = 360, 270
rows = (len(shots) + cols - 1) // cols
sheet = Image.new("RGB", (cols * tw, rows * (th + 18)), (20, 20, 24))
draw = ImageDraw.Draw(sheet)
for i, p in enumerate(shots):
    x, y = (i % cols) * tw, (i // cols) * (th + 18)
    sheet.paste(Image.open(p).convert("RGB").resize((tw, th)), (x, y + 18))
    draw.text((x + 4, y + 3), p.name[: -len(f"_{suffix}.png")], fill=(230, 230, 230))
out = run / f"contact_{suffix}.png"
sheet.save(out)
print(out)
