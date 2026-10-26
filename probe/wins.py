"""wins.py — 列出某窗口类的可见性/位置/z 序，用来诊断「窗口到底显没显示」

用法：python wins.py BattTrayCard
"""
import ctypes
import ctypes.wintypes as wt
import sys

user32 = ctypes.WinDLL("user32", use_last_error=True)

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]


def main():
    # 必须自己做 DPI 感知，否则被测窗口的坐标会被系统按本进程的
    # DPI 上下文虚拟化一遍（125% 缩放下会看到 0.8 倍的数字）
    try:
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            user32.SetProcessDPIAware()
        except Exception:
            pass

    want = sys.argv[1] if len(sys.argv) > 1 else "BattTrayCard"
    found = []

    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        name = buf.value
        if want.lower() in name.lower():
            t = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, t, 256)
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            found.append((hwnd, name, t.value, bool(user32.IsWindowVisible(hwnd)),
                          r.left, r.top, r.right - r.left, r.bottom - r.top,
                          user32.GetWindowLongW(hwnd, -16),      # GWL_STYLE
                          user32.GetWindowLongW(hwnd, -20)))     # GWL_EXSTYLE
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    if not found:
        print(f"no window class matching {want!r}")
    for h, name, title, vis, x, y, w, hh, style, ex in found:
        print(f"hwnd=0x{h:X} class={name} title={title!r} visible={vis} "
              f"rect={x},{y} {w}x{hh} style=0x{style & 0xFFFFFFFF:08X} "
              f"ex=0x{ex & 0xFFFFFFFF:08X}")


if __name__ == "__main__":
    main()
