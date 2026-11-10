"""Recorta o placar (canto superior esquerdo, 1024x480) das capturas e monta
uma grade com o frame em cada recorte: crops.py <pasta> <saida.png> [colunas] [x0,y0,x1,y1]"""
import glob
import os
import sys

from PIL import Image, ImageDraw

folder, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
box = tuple(int(v) for v in sys.argv[4].split(",")) if len(sys.argv) > 4 else (70, 30, 290, 62)
tiles = []
for f in sorted(glob.glob(os.path.join(folder, "f*.png"))):
    im = Image.open(f).convert("RGB")
    sx = im.width / 1024
    crop = im.crop(tuple(int(v * sx) if i % 2 == 0 else int(v * im.height / 480) for i, v in enumerate(box)))
    crop = crop.resize((crop.width * 2, crop.height * 2), Image.NEAREST)
    frame = int(os.path.basename(f)[1:7])
    tile = Image.new("RGB", (crop.width + 110, crop.height), (0, 0, 0))
    tile.paste(crop, (110, 0))
    ImageDraw.Draw(tile).text((4, crop.height // 2 - 6), "%d %d:%02d" % (frame, frame // 3600, frame // 60 % 60),
                              fill=(255, 255, 0))
    tiles.append(tile)
w, h = tiles[0].size
rows = (len(tiles) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h), (40, 40, 40))
for n, t in enumerate(tiles):
    sheet.paste(t, ((n % cols) * w, (n // cols) * h))
sheet.save(out)
print(out, sheet.size, len(tiles))
