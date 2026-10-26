"""_dev_iconout.py — 把 Python 版托盘图标导出成 BMP，用于和 C 版逐像素对比

用和 C 版完全相同的路径：make_hicon() 造 HICON → DrawIconEx 画到 DIB，
所以两者的差异只可能来自栅格化本身。

用法：python _dev_iconout.py out.bmp [bg]
"""
import os
import sys

# 这个脚本要用到 Python 版项目的 wsgui / tray_app，把它的目录加进 sys.path
_HERE = os.path.dirname(os.path.abspath(__file__))
_PY_TRAY = os.path.normpath(os.path.join(_HERE, "..", "..", "2.4G电量托盘", "tray"))
if _PY_TRAY not in sys.path:
    sys.path.insert(0, _PY_TRAY)

import ctypes
import ctypes.wintypes as wt
import struct
import sys

import wsgui as ui

PCTS = (0, 15, 50, 100, -1)
CHG = (0, 0, 0, 0, 1)
SRCCOPY = 0x00CC0020


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "py_icon.bmp"
    bg = int(sys.argv[2]) if len(sys.argv) > 2 else 245
    size = 16
    W, H = size * len(PCTS), size

    user32, gdi32 = ui.user32, ui.gdi32
    gdi32.CreateCompatibleDC.restype = ui.HANDLE32
    gdi32.CreateDIBSection.restype = ui.HANDLE32
    user32.GetDC.restype = ui.HANDLE32

    screen = user32.GetDC(None)
    mdc = gdi32.CreateCompatibleDC(screen)

    bi = ui.BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(ui.BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = W
    bi.bmiHeader.biHeight = -H
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    bits = ctypes.c_void_p()
    dib = gdi32.CreateDIBSection(screen, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    gdi32.SelectObject(mdc, dib)

    gdi32.GetStockObject.restype = ui.HANDLE32
    gdi32.CreateSolidBrush.restype = ui.HANDLE32
    brush = gdi32.CreateSolidBrush(ui.rgb(bg, bg, bg))
    rect = wt.RECT(0, 0, W, H)
    user32.FillRect(mdc, ctypes.byref(rect), brush)
    gdi32.DeleteObject(brush)

    user32.DrawIconEx.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ui.HANDLE32,
                                  ctypes.c_int, ctypes.c_int, wt.UINT,
                                  ctypes.c_void_p, wt.UINT]
    for i, (pct, chg) in enumerate(zip(PCTS, CHG)):
        hic = ui.make_hicon(size, pct if pct >= 0 else 0, bool(chg))
        if hic:
            user32.DrawIconEx(mdc, i * size, 0, hic, size, size, 0, None, 3)
            user32.DestroyIcon(hic)

    # 取像素
    bmi2 = ui.BITMAPINFO()
    bmi2.bmiHeader.biSize = ctypes.sizeof(ui.BITMAPINFOHEADER)
    bmi2.bmiHeader.biWidth = W
    bmi2.bmiHeader.biHeight = -H
    bmi2.bmiHeader.biPlanes = 1
    bmi2.bmiHeader.biBitCount = 32
    bmi2.bmiHeader.biCompression = 0
    buf = ctypes.create_string_buffer(W * H * 4)
    gdi32.GetDIBits.argtypes = [wt.HDC, ui.HANDLE32, wt.UINT, wt.UINT,
                                ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
    gdi32.GetDIBits(mdc, dib, 0, H, buf, ctypes.byref(bmi2), 0)

    src = buf.raw
    rows = []
    for y in range(H - 1, -1, -1):          # BMP 自下而上
        line = bytearray()
        for x in range(W):
            o = (y * W + x) * 4
            line += bytes((src[o], src[o + 1], src[o + 2]))
        while len(line) % 4:
            line.append(0)
        rows.append(bytes(line))
    pix = b"".join(rows)

    fh = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(pix), 0, 0, 54)
    ih = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(pix), 0, 0, 0, 0)
    with open(out, "wb") as f:
        f.write(fh + ih + pix)

    gdi32.DeleteObject(dib)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(None, screen)
    print(f"{out}  {W}x{H}")


if __name__ == "__main__":
    main()
