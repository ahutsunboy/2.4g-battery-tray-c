"""shot.py — 纯 ctypes 截屏（GDI BitBlt + 手写 PNG），不依赖 Pillow

用法：
    python shot.py                       # 全屏
    python shot.py out.png               # 全屏存到 out.png
    python shot.py out.png 100 100 400 300   # 只截 x y w h

为什么不用 PIL：本机没装，装它只为截一张图不值当。
PNG 编码用 zlib，扫描线过滤一律 0（None），足够。
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def capture(x, y, w, h):
    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)
    gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, x, y, SRCCOPY)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h          # 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), DIB_RGB_COLORS)

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(0, hdc_screen)

    raw = bytearray()
    data = buf.raw
    for row in range(h):
        off = row * w * 4
        line = data[off:off + w * 4]
        # BGRA -> RGB
        out = bytearray(w * 3)
        out[0::3] = line[2::4]
        out[1::3] = line[1::4]
        out[2::3] = line[0::4]
        raw.append(0)
        raw.extend(out)
    return bytes(raw), w, h


def write_png(path, raw, w, h):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8bit truecolor
    body = chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + body)


def main():
    # 先声明 DPI 感知：否则系统会把本进程看到的桌面按缩放比虚拟化，
    # GetSystemMetrics 返回 2048x1152（实际 2560x1440），截出来的图也会糊
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            user32.SetProcessDPIAware()
        except Exception:
            pass

    args = sys.argv[1:]
    out = args[0] if args else "shot.png"
    if len(args) >= 5:
        x, y, w, h = (int(v) for v in args[1:5])
    else:
        vs_w = user32.GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
        vs_h = user32.GetSystemMetrics(79)
        x, y, w, h = 0, 0, vs_w, vs_h
    raw, w, h = capture(x, y, w, h)
    write_png(out, raw, w, h)
    print(f"{out}  {w}x{h}  {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
