"""png2bmp.py — 把本项目的 PNG 转成 24bpp BMP（只处理自己写的那种：8bit truecolor、过滤 0）

用途：probe/shot.py 与 _dev_panel_render.py 输出的是 PNG，而 bmpdiff.py 读 BMP，
本机没有 Pillow，用 zlib 手解足够。

用法：python png2bmp.py in.png out.bmp
"""
import struct
import sys
import zlib


def convert(src, dst):
    d = open(src, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{src}: 不是 PNG")
    pos, idat, w, h = 8, b"", None, None
    while pos < len(d):
        ln = struct.unpack_from(">I", d, pos)[0]
        tag = d[pos + 4:pos + 8]
        payload = d[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, bd, ct = struct.unpack_from(">IIBB", payload, 0)
            if bd != 8 or ct != 2:
                raise SystemExit(f"{src}: 只支持 8bit truecolor，实际 bd={bd} ct={ct}")
        elif tag == b"IDAT":
            idat += payload
        pos += 12 + ln

    raw = zlib.decompress(idat)
    stride = w * 3
    rows, p = [], 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = raw[p:p + stride]
        p += stride
        if f == 2:                      # Up 过滤（万一别处生成的 PNG 用了）
            prev = rows[-1] if rows else bytes(stride)
            line = bytes((line[i] + prev[i]) & 0xFF for i in range(stride))
        elif f != 0:
            raise SystemExit(f"{src}: 第 {y} 行过滤类型 {f}，不支持")
        rows.append(bytes(line))

    fh = struct.pack("<2sIHHI", b"BM", 14 + 40 + stride * h, 0, 0, 54)
    ih = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 0, 0, 0, 0)
    open(dst, "wb").write(fh + ih + b"".join(reversed(rows)))
    return w, h


if __name__ == "__main__":
    w, h = convert(sys.argv[1], sys.argv[2])
    print(f"{sys.argv[2]}  {w}x{h}")
