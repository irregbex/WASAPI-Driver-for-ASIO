#!/usr/bin/env python3
# WASAPI Driver for ASIO - application icon generator (0.9.3). SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: python3 scripts/make_icon.py [--previews <folder>]
#   Writes src/res/app.ico (16, 20, 24, 32, 40, 48, 64, 96, 128, 256; 256 as PNG, the others as 32-bit DIBs with
#   alpha and an AND mask) and src/res/icon-256.png. With --previews also writes the three design options
#   (A = the icon used, B and C = alternatives) at 256, 32 and 16 px.
# Design A (original artwork of the project, no third-party shapes): a dark rounded-square tile; two small
#   nodes (WASAPI on the left, ASIO host on the right) joined by a bright arch (a sine half-wave), a "bridge"
#   between them, with a short sine wave under it from 40 px up, with a teal-to-cyan gradient that matches the Dark theme accent. Every size is drawn on its own at 8x
#   and reduced with Lanczos; sizes 16-32 use hand-tuned parameters (thicker stroke, simpler wave, node centers
#   on pixel centers) so the shape stays readable at 16x16.
# Needs Pillow (python3-pil). Deterministic: the same script gives the same bytes.
import io, math, os, struct, sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8                                     # supersampling factor

TEAL, CYAN = (0x2E, 0xD3, 0x9A), (0x3C, 0xC8, 0xF0)
TILE_TOP, TILE_BOT, TILE_EDGE = (0x34, 0x37, 0x3F), (0x15, 0x16, 0x1A), (0x4A, 0x4E, 0x58)
DARK = (0x10, 0x11, 0x14)

def lerp(a, b, t): return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

# Per-size parameters (in pixels of the final size): tile corner radius, stroke width, node radius, ripple
# amplitude (0 = plain arch), node y, arch height.
def params(n):
    tuned = {16: (3.0, 2.0, 2.0, 0.0, 10.5, 6.5), 20: (4.0, 2.2, 2.4, 0.0, 13.0, 8.0),
             24: (4.5, 2.4, 2.6, 0.0, 15.5, 9.5), 32: (6.0, 2.8, 3.0, 0.0, 21.0, 12.5)}
    if n in tuned: return tuned[n]
    return (n * 0.19, n * 0.075, n * 0.085, 0.0, n * 0.66, n * 0.40)

def tile(d, n, s):
    r, _, _, _, _, _ = params(n)
    S = n * s
    # vertical gradient, clipped to the rounded square through a mask
    grad = Image.new('RGB', (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S): gd.line([(0, y), (S, y)], fill=lerp(TILE_TOP, TILE_BOT, y / max(1, S - 1)))
    mask = Image.new('L', (S, S), 0)
    m = ImageDraw.Draw(mask)
    inset = 0.5 * s if n <= 32 else n * 0.02 * s
    m.rounded_rectangle([inset, inset, S - 1 - inset, S - 1 - inset], radius=r * s, fill=255)
    d.paste(grad, (0, 0), mask)
    edge = ImageDraw.Draw(d)
    edge.rounded_rectangle([inset, inset, S - 1 - inset, S - 1 - inset], radius=r * s, outline=TILE_EDGE + (255,),
                           width=max(1, int(s * (1.0 if n <= 32 else n / 48.0))))

def wave_points(n, s, x0, x1, ny, h, ripple):
    pts = []
    steps = 200
    for i in range(steps + 1):
        t = i / steps
        x = x0 + (x1 - x0) * t
        y = ny - h * math.sin(math.pi * t) - ripple * math.sin(5 * math.pi * t) * math.sin(math.pi * t)
        pts.append((x * s, y * s))
    return pts

def stroke(img, pts, width, s):
    # gradient stroke as a chain of overlapping discs (smooth edges and round caps at every size)
    d = ImageDraw.Draw(img)
    W = img.size[0]
    rr = width * s / 2
    dense = []
    for a, b in zip(pts, pts[1:]):
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        k = max(1, int(L / max(1.0, rr / 3)))
        for j in range(k): dense.append((a[0] + (b[0] - a[0]) * j / k, a[1] + (b[1] - a[1]) * j / k))
    dense.append(pts[-1])
    for q in dense:
        c = lerp(TEAL, CYAN, min(1.0, max(0.0, q[0] / W)))
        d.ellipse([q[0] - rr, q[1] - rr, q[0] + rr, q[1] + rr], fill=c + (255,))

def node(img, cx, cy, r, color, s, ring):
    d = ImageDraw.Draw(img)
    R = (r + ring) * s
    d.ellipse([cx * s - R, cy * s - R, cx * s + R, cy * s + R], fill=DARK + (255,))
    R = r * s
    d.ellipse([cx * s - R, cy * s - R, cx * s + R, cy * s + R], fill=color + (255,))

def design_a(n):
    s = SS
    img = Image.new('RGBA', (n * s, n * s), (0, 0, 0, 0))
    tile(img, n, s)
    _, w, nr, ripple, ny, h = params(n)
    # node centers on pixel centers for the small sizes (hinting)
    lx, rx = (round(n * 0.25) + 0.5, round(n * 0.75) - 0.5) if n <= 32 else (n * 0.25, n * 0.75)
    pts = wave_points(n, s, lx, rx, ny, h, ripple)
    stroke(img, pts, w, s)
    if n >= 40:                              # inner detail: a short sine wave under the arch (large sizes only)
        inner = []
        for i in range(121):
            t = i / 120
            inner.append(((0.37 + 0.26 * t) * n * s, (ny - h * 0.2 - n * 0.05 * math.sin(2 * math.pi * t)) * s))
        stroke(img, inner, w * 0.55, s)
    ring = 1.0 if n <= 32 else n / 40.0
    node(img, lx, ny, nr, TEAL, s, ring)
    node(img, rx, ny, nr, CYAN, s, ring)
    return img.resize((n, n), Image.LANCZOS)

def design_b(n):                            # alternative: full-width sine over a deck line, square ends
    s = SS
    img = Image.new('RGBA', (n * s, n * s), (0, 0, 0, 0))
    tile(img, n, s)
    d = ImageDraw.Draw(img)
    w = max(1.6, n * 0.07)
    d.line([(n * 0.18 * s, n * 0.72 * s), (n * 0.82 * s, n * 0.72 * s)], fill=(0x8A, 0x8D, 0x93, 255), width=int(w * 0.6 * s))
    pts = []
    for i in range(201):
        t = i / 200
        pts.append(((0.18 + 0.64 * t) * n * s, (0.48 - 0.2 * math.sin(2 * math.pi * t)) * n * s))
    stroke(img, pts, w, s)
    return img.resize((n, n), Image.LANCZOS)

def design_c(n):                            # alternative: round badge with five level bars forming an arch
    s = SS
    img = Image.new('RGBA', (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    S = n * s
    d.ellipse([0.5 * s, 0.5 * s, S - 0.5 * s, S - 0.5 * s], fill=TILE_BOT + (255,), outline=TILE_EDGE + (255,), width=max(1, s))
    hs = [0.30, 0.48, 0.58, 0.48, 0.30]
    bw = n * 0.11
    for i, hh in enumerate(hs):
        cx = n * (0.26 + 0.12 * i)
        c = lerp(TEAL, CYAN, i / 4)
        d.rounded_rectangle([(cx - bw / 2) * s, (0.70 - hh) * n * s, (cx + bw / 2) * s, 0.70 * n * s], radius=bw * s / 3, fill=c + (255,))
    return img.resize((n, n), Image.LANCZOS)

def dib(img):
    n = img.size[0]
    px = img.load()
    hdr = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = bytearray()
    for y in range(n - 1, -1, -1):          # bottom-up BGRA
        for x in range(n):
            r, g, b, a = px[x, y]
            rows += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    mask = bytearray()
    for y in range(n - 1, -1, -1):
        row = bytearray(stride)
        for x in range(n):
            if px[x, y][3] == 0: row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return hdr + bytes(rows) + bytes(mask)

def write_ico(path, images):
    entries, blobs = [], []
    for img in images:
        n = img.size[0]
        if n >= 256:
            b = io.BytesIO(); img.save(b, 'PNG', optimize=True); data = b.getvalue()
        else:
            data = dib(img)
        entries.append((n, data)); blobs.append(data)
    out = struct.pack('<HHH', 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    for n, data in entries:
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    out += b''.join(blobs)
    with open(path, 'wb') as f: f.write(out)

def main():
    res = os.path.join(ROOT, 'src', 'res')
    imgs = [design_a(n) for n in SIZES]
    write_ico(os.path.join(res, 'app.ico'), imgs)
    imgs[-1].save(os.path.join(res, 'icon-256.png'), 'PNG', optimize=True)
    if '--previews' in sys.argv:
        out = sys.argv[sys.argv.index('--previews') + 1]
        os.makedirs(out, exist_ok=True)
        for name, fn in (('A', design_a), ('B', design_b), ('C', design_c)):
            for n in (256, 32, 16):
                im = fn(n)
                im.save(os.path.join(out, 'icon-option-%s-%d.png' % (name, n)))
                if n < 256:                 # enlarged copy to judge the pixels
                    im.resize((n * 8, n * 8), Image.NEAREST).save(os.path.join(out, 'icon-option-%s-%d-x8.png' % (name, n)))
    print('app.ico:', ', '.join(str(n) for n in SIZES))

if __name__ == '__main__':
    main()
