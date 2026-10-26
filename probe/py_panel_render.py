"""_dev_panel_render.py — 把 Python 版卡片离屏渲染成 PNG（用于和 C 版逐像素对比）

窗口交互（抢焦点、点外部收起）会干扰截图，所以这里直接调用
BatteryTray._paint_content()：它本来就是「窗口绘制与离屏渲染共用」的那一层，
画到 DIB 上再存 PNG，得到的就是真实渲染结果。

用法：python _dev_panel_render.py out.png
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
import time
import zlib

import wsgui as ui
import tray_app as ta

SRCCOPY = 0x00CC0020


class FakeState:
    """够用的假设备状态：只提供 snapshot()"""

    def __init__(self, **kw):
        self.d = dict(name="", label="", kind="kbd", vidpid="", battery=None,
                      charging=False, online=None, raw="", note="", updated_ts=None)
        self.d.update(kw)

    def snapshot(self):
        return dict(self.d)


def capture_dib(hdc_mem, hbmp, w, h):
    bi = ui.BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(ui.BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    ui.gdi32.GetDIBits.argtypes = [wt.HDC, ui.HANDLE32, wt.UINT, wt.UINT,
                                   ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
    buf = ctypes.create_string_buffer(w * h * 4)
    ui.gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), 0)
    return buf.raw


def write_png(path, data, w, h):
    raw = bytearray()
    for row in range(h):
        off = row * w * 4
        line = data[off:off + w * 4]
        out = bytearray(w * 3)
        out[0::3] = line[2::4]
        out[1::3] = line[1::4]
        out[2::3] = line[0::4]
        raw.append(0)
        raw.extend(out)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    body = chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + body)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "py_panel.png"

    try:
        ui.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        ui.user32.SetProcessDPIAware()

    app = ta.BatteryTray()
    app.scale = ui.dpi_scale()
    app._create_resources()

    # 与 C 版当前真机状态对齐：键盘 100% 在线，鼠标还没有帧
    app.states = [
        FakeState(name="狼蛛 FN75 键盘", label="Compx 键盘", kind="kbd",
                  vidpid="3554:FA09", battery=100, charging=False,
                  online=True, raw="13 0A 01 00 04 05 64 01", updated_ts=time.time()),
        FakeState(name="迈从 G3 鼠标", label="迈从鼠标", kind="mouse",
                  vidpid="258A:0138", battery=None, charging=False,
                  online=None, note="等待数据"),
    ]

    w, h, *_ = app._panel_layout()

    hdc_screen = ui.user32.GetDC(0)
    hdc_mem = ui.gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = ui.gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    ui.gdi32.SelectObject(hdc_mem, hbmp)

    app._paint_content(hdc_mem, w, h)
    data = capture_dib(hdc_mem, hbmp, w, h)

    ui.gdi32.DeleteObject(hbmp)
    ui.gdi32.DeleteDC(hdc_mem)
    ui.user32.ReleaseDC(0, hdc_screen)

    write_png(out, data, w, h)
    print(f"{out}  {w}x{h}")


if __name__ == "__main__":
    main()
