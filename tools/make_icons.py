# tools/make_icons.py <out folder> - draws res/aotr_gold.ico, aotr_yellow.ico, aotr_red.ico (the One Ring, its inscription glowing). Needs Pillow.
import math, sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
S = 1024  # supersampled canvas
def lerp(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))
def make(variant):
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    # background: rounded square, near-black with a deep ember glow in the middle
    bg = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    m = Image.new('L', (S, S), 0); ImageDraw.Draw(m).rounded_rectangle([24, 24, S - 24, S - 24], radius=190, fill=255)
    glow_col = {'gold': (150, 45, 10), 'yellow': (150, 110, 10), 'red': (170, 10, 10)}[variant]
    grad = Image.new('RGBA', (S, S))
    gp = grad.load()
    for y in range(S):
        for x in range(S):
            d = math.hypot(x - S / 2, y - S / 2) / (S * 0.62)
            t = max(0.0, min(1.0, 1 - d))
            gp[x, y] = lerp((6, 5, 5, 255), glow_col + (255,), t ** 3.2 * 0.85)
    bg.paste(grad, (0, 0), m)
    # rim
    ImageDraw.Draw(bg).rounded_rectangle([24, 24, S - 24, S - 24], radius=190, outline=(60, 40, 20, 255), width=10)
    img = Image.alpha_composite(img, bg)
    # the ring: an ellipse band (seen slightly from above) with metallic shading
    cx, cy = S / 2, S / 2 + 10
    Ro, Ri = 330, 262          # outer / inner radii (x); flattened in y
    fy = 0.80
    ring = Image.new('RGBA', (S, S), (0, 0, 0, 0)); rp = ring.load()
    gold_dark, gold_mid, gold_hi = (70, 38, 4), (205, 142, 30), (255, 244, 185)
    for y in range(S):
        for x in range(S):
            dx = (x - cx); dy = (y - cy) / fy
            r = math.hypot(dx, dy)
            if Ri <= r <= Ro:
                u = (r - Ri) / (Ro - Ri)            # across the band 0..1
                ang = math.atan2(dy, dx)
                # cylinder-like cross-section shading + a light from the upper left
                prof = math.sin(u * math.pi)
                light = 0.5 + 0.5 * math.cos(ang - math.radians(-130))
                t = 0.12 + 0.88 * (prof ** 0.7) * (0.35 + 0.65 * light)
                c = lerp(gold_dark, gold_mid, min(1, t * 1.4)) if t < 0.7 else lerp(gold_mid, gold_hi, (t - 0.7) / 0.3)
                # specular streak
                spec = max(0.0, math.cos(ang - math.radians(-125))) ** 18 * max(0.0, 1 - abs(u - 0.42) * 3.2)
                c = lerp(c, (255, 250, 220), min(1.0, spec))
                rp[x, y] = c + (255,)
    # soft edge
    ring = ring.filter(ImageFilter.GaussianBlur(1.2))
    # the fiery inscription: little curved strokes along the middle of the band
    ins = Image.new('RGBA', (S, S), (0, 0, 0, 0)); di = ImageDraw.Draw(ins)
    fire = {'gold': (255, 110, 20), 'yellow': (255, 200, 40), 'red': (255, 40, 25)}[variant]
    rm = (Ro + Ri) / 2
    n = 30
    for k in range(n):
        a0 = 2 * math.pi * k / n + 0.05
        # each "letter": a short arc plus a hook, slightly varied
        L = 0.11 + 0.04 * ((k * 7) % 3)
        pts = []
        for j in range(8):
            a = a0 + L * j / 7
            rr = rm + 11 * math.sin(j / 7 * math.pi * (1 + (k % 2)))
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a) * fy))
        di.line(pts, fill=fire + (255,), width=6, joint='curve')
        a = a0 + L
        di.ellipse([cx + (rm - 12) * math.cos(a) - 4, cy + (rm - 12) * math.sin(a) * fy - 4, cx + (rm - 12) * math.cos(a) + 4, cy + (rm - 12) * math.sin(a) * fy + 4], fill=fire + (255,))
    # only on the ring's band
    band = Image.new('L', (S, S), 0); bd = band.load(); rpa = ring.split()[3].load()
    for y in range(S):
        for x in range(S):
            bd[x, y] = rpa[x, y]
    ins.putalpha(ImageChops.multiply(ins.split()[3], band))
    glow = ins.filter(ImageFilter.GaussianBlur(14))
    # outer glow behind the ring
    halo = Image.new('RGBA', (S, S), (0, 0, 0, 0)); dh = ImageDraw.Draw(halo)
    dh.ellipse([cx - Ro - 30, cy - (Ro + 30) * fy, cx + Ro + 30, cy + (Ro + 30) * fy], outline=fire + (150,), width=70)
    halo = halo.filter(ImageFilter.GaussianBlur(40))
    img = Image.alpha_composite(img, halo)
    img = Image.alpha_composite(img, ring)
    img = Image.alpha_composite(img, glow)
    img = Image.alpha_composite(img, ins)
    # keep everything inside the rounded square
    a = ImageChops.multiply(img.split()[3], m)
    img.putalpha(a)
    return img
out = sys.argv[1]
for v in ('gold', 'yellow', 'red'):
    big = make(v)
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
    big.resize((256, 256), Image.LANCZOS).save(f'{out}/aotr_{v}.ico', sizes=sizes)
    big.resize((256, 256), Image.LANCZOS).save(f'{out}/aotr_{v}_preview.png')
print('ok')
