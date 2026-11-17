import sys

from PIL import Image

source, target = sys.argv[1], sys.argv[2]
art = Image.open(source).convert("RGBA")
w, h = art.size
box = (round(w * 118 / 320), round(h * 4 / 176), round(w * 222 / 320), round(h * 108 / 176))
art.crop(box).resize((256, 256), Image.LANCZOS).save(target, sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
