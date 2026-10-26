"""bmpdiff.py — 比较两张 24bpp BMP，输出放大对比图 + 差异统计

用法：python bmpdiff.py a.bmp b.bmp diff.png [scale]
图上三行：A / B / 绝对差×4
"""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 24:
        raise SystemExit(f"{path}: 只支持 24bpp，实际 {bpp}")
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        base = off + y * stride
        rows.append(d[base:base + w * 3])
    rows.reverse()                      # BMP 自下而上
    return w, h, rows


def write_png(path, rows, w, h):
    raw = bytearray()
    for r in rows:
        raw.append(0)
        raw.extend(r)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    body = chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + body)


def main():
    a_path, b_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
    scale = int(sys.argv[4]) if len(sys.argv) > 4 else 4

    w, h, A = read_bmp(a_path)
    w2, h2, B = read_bmp(b_path)
    if (w, h) != (w2, h2):
        raise SystemExit(f"尺寸不一致: {w}x{h} vs {w2}x{h2}")

    diff_rows, total, mx, cnt = [], 0, 0, 0
    for y in range(h):
        row = bytearray()
        line = bytearray()
        for x in range(w):
            pa = A[y][x * 3:x * 3 + 3]
            pb = B[y][x * 3:x * 3 + 3]
            dd = [abs(pa[i] - pb[i]) for i in range(3)]
            total += sum(dd)
            mx = max(mx, max(dd))
            if max(dd) > 2:
                cnt += 1
            line += bytes(min(255, v * 4) for v in dd)
        diff_rows.append(bytes(line))

    def up(rows):
        out_rows = []
        for r in rows:
            wide = bytearray()
            for x in range(w):
                wide += r[x * 3:x * 3 + 3] * scale
            out_rows.extend([bytes(wide)] * scale)
        return out_rows

    combined = up(A) + up(B) + up(diff_rows)
    write_png(out, combined, w * scale, h * scale * 3)

    n = w * h
    print(f"{out}  {w}x{h} -> {w*scale}x{h*scale*3}")
    print(f"平均误差 {total / (n * 3):.2f}/255   最大误差 {mx}   "
          f"差异像素 {cnt}/{n} ({cnt * 100.0 / n:.1f}%)")


if __name__ == "__main__":
    main()
