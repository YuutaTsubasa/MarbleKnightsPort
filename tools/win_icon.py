"""The game's own icon for the Windows build: the largest AppIcon*.png of the iOS app -> work/win/icon.ico
(16-256 px, PNG entries), which tools/build_win_ga.sh puts into MarbleKnights.exe as icon group 103
(the one Unity's player uses for its window).

iOS app PNGs are Apple's CgBI variant: a CgBI chunk first, IDAT as raw deflate (no zlib header) and
pixels as premultiplied BGRA. Pure Python (no Pillow), since it reads game files: run with python -I.
The .ico is made from the game's art: work/ only, never in Git.
usage: python -I tools/win_icon.py
"""
import glob
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app")
OUT = os.path.join(ROOT, "work", "win", "icon.ico")
SIZES = (16, 24, 32, 48, 64, 128, 256)


def read_png(path):
    """-> (width, height, rows of RGBA bytes); handles CgBI and plain 8-bit RGBA/RGB PNGs."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, cgbi, ihdr = 8, b"", False, None
    while pos < len(data):
        n, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"CgBI":
            cgbi = True
        elif kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    w, h, depth, ctype, _, _, interlace = ihdr
    assert depth == 8 and ctype in (2, 6) and not interlace, (path, ihdr)
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat, -15 if cgbi else 15)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        out = bytearray(w * 4)
        for x in range(w):
            px = line[x * bpp:x * bpp + bpp]
            if cgbi:  # premultiplied BGR(A)
                b_, g_, r_ = px[0], px[1], px[2]
                al = px[3] if bpp == 4 else 255
                if 0 < al < 255:
                    r_, g_, b_ = (min(255, v * 255 // al) for v in (r_, g_, b_))
                out[x * 4:x * 4 + 4] = bytes((r_, g_, b_, al))
            else:
                out[x * 4:x * 4 + 4] = bytes(px) + (b"" if bpp == 4 else b"\xff")
        rows.append(out)
    return w, h, rows


def resize(w, h, rows, size):
    """Area average when shrinking, bilinear when growing (alpha-weighted colour)."""
    out = []
    for y in range(size):
        line = bytearray(size * 4)
        for x in range(size):
            if size <= w:
                x0, x1 = x * w // size, max(x * w // size + 1, (x + 1) * w // size)
                y0, y1 = y * h // size, max(y * h // size + 1, (y + 1) * h // size)
                pts = [(xx, yy, 1.0) for yy in range(y0, y1) for xx in range(x0, x1)]
            else:
                fx, fy = (x + 0.5) * w / size - 0.5, (y + 0.5) * h / size - 0.5
                ix, iy = int(fx // 1), int(fy // 1)
                dx, dy = fx - ix, fy - iy
                pts = [(min(max(ix + i, 0), w - 1), min(max(iy + j, 0), h - 1), (dx if i else 1 - dx) * (dy if j else 1 - dy))
                       for i in (0, 1) for j in (0, 1)]
            acc = [0.0] * 4
            for xx, yy, wt in pts:
                p = rows[yy][xx * 4:xx * 4 + 4]
                al = p[3] * wt
                acc[0] += p[0] * al
                acc[1] += p[1] * al
                acc[2] += p[2] * al
                acc[3] += al
            tw = sum(wt for _, _, wt in pts)
            a = acc[3] / tw
            line[x * 4:x * 4 + 4] = bytes(
                [round(acc[i] / acc[3]) if acc[3] else 0 for i in range(3)] + [round(a)])
        out.append(line)
    return out


def png_bytes(size, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(app=APP, out=OUT):
    icons = sorted(glob.glob(os.path.join(app, "AppIcon*.png")), key=lambda p: read_png(p)[0])
    assert icons, "no AppIcon*.png in " + app
    w, h, rows = read_png(icons[-1])
    images = [png_bytes(s, resize(w, h, rows, s)) for s in SIZES]
    head = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    entries = b""
    for s, img in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), off)
        off += len(img)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(head + entries + b"".join(images))
    print(f"{os.path.basename(icons[-1])} ({w}x{h}) -> {out}")


if __name__ == "__main__":
    main()
