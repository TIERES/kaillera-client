"""Grade de capturas com o frame/tempo em cada uma: montage.py <pasta> <saida.png> [largura] [colunas] [filtro]"""
import glob
import os
import sys

from PIL import Image, ImageDraw

folder, out = sys.argv[1], sys.argv[2]
width = int(sys.argv[3]) if len(sys.argv) > 3 else 320
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 4
pattern = sys.argv[5] if len(sys.argv) > 5 else "f*.png"
files = sorted(glob.glob(os.path.join(folder, pattern)))
ims = []
for f in files:
    im = Image.open(f).convert("RGB")
    im = im.resize((width, int(im.height * width / im.width)))
    frame = int(os.path.basename(f)[1:7])
    d = ImageDraw.Draw(im)
    label = "%d  %d:%02d" % (frame, frame // 3600, frame // 60 % 60)
    d.rectangle([0, 0, 8 * len(label) + 6, 14], fill=(0, 0, 0))
    d.text((3, 2), label, fill=(255, 255, 0))
    ims.append(im)
h = max(i.height for i in ims)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * width, rows * h))
for n, im in enumerate(ims):
    sheet.paste(im, ((n % cols) * width, (n // cols) * h))
sheet.save(out)
print(out, sheet.size, len(ims))
