"""验证卡片的外部点击收起行为。

单实例互斥决定了「同一个进程只能弹一次卡片」，所以每个用例都单独起一次进程：

  case "inside"  点卡片内部      -> 不应收起
  case "outside" 点卡片外部      -> 应收起
  case "esc"     按 Esc          -> 应收起
  case "tray"    点托盘图标      -> 应收起（且不应立刻弹回来）

判据只认 tray.log 里 card_show / card_hide 的先后，不靠肉眼看屏幕。
用法：python probe/clicktest.py [case ...]      不带参数就跑全部
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
LOG = os.path.join(ROOT, "tray.log")
EXE = os.environ.get("TRAY_EXE") or os.path.join(ROOT, "tray.exe")

user32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD),
                ("Data4", ctypes.c_byte * 8)]


class NOTIFYICONIDENTIFIER(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("hWnd", wt.HWND),
                ("uID", wt.UINT), ("guidItem", GUID)]


shell32.Shell_NotifyIconGetRect.argtypes = [ctypes.POINTER(NOTIFYICONIDENTIFIER),
                                           ctypes.POINTER(wt.RECT)]
shell32.Shell_NotifyIconGetRect.restype = ctypes.c_long

try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    user32.SetProcessDPIAware()

LEFTDOWN, LEFTUP = 0x0002, 0x0004
VK_ESCAPE = 0x1B


def stop(p):
    """结束进程并等它真的退出。

    只 terminate 不等的话，单实例互斥还占着，下一个用例起不来；
    FindWindowW 还会找到那个正在退出的窗口，PostMessage 就石沉大海。
    """
    try:
        p.terminate()
        p.wait(timeout=4)
    except Exception:
        pass
    time.sleep(0.35)


def read_events():
    if not os.path.exists(LOG):
        return []
    ev = []
    with open(LOG, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "card_show" in line:
                m = re.search(r"card_show\s+(\d+)x(\d+)\s+at\s+(-?\d+),(-?\d+)", line)
                ev.append(("show", tuple(int(v) for v in m.groups()) if m else None))
            elif "card_hide" in line:
                ev.append(("hide", None))
    return ev


def click(x, y, button="left"):
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.18)
    if button == "left":
        user32.mouse_event(LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.09)
        user32.mouse_event(LEFTUP, 0, 0, 0, 0)
    else:
        user32.mouse_event(0x0008, 0, 0, 0, 0)   # RIGHTDOWN
        time.sleep(0.09)
        user32.mouse_event(0x0010, 0, 0, 0, 0)   # RIGHTUP
    time.sleep(0.5)


def press_esc():
    user32.keybd_event(VK_ESCAPE, 0, 0, 0)
    time.sleep(0.07)
    user32.keybd_event(VK_ESCAPE, 0, 2, 0)
    time.sleep(0.5)


def tray_click():
    """模拟 Explorer 发来的托盘回调。

    直接点屏幕坐标是不可靠的：图标可能被折叠进「隐藏的图标」面板里，
    Shell_NotifyIconGetRect 返回的是溢出区坐标，点下去什么都不会发生。
    真正驱动应用的是 Explorer 往主窗口投的 WM_TRAY_MSG，直接投这条消息
    测到的才是我们自己的逻辑。
    """
    hwnd = user32.FindWindowW("BattTrayMain", None)
    if not hwnd:
        return False, "找不到 BattTrayMain 窗口"
    user32.PostMessageW(hwnd, 0x8000 + 1, 0, 0x0202)   # WM_APP+1, WM_LBUTTONUP
    time.sleep(0.7)
    return True, None


def start():
    if os.path.exists(LOG):
        os.remove(LOG)
    p = subprocess.Popen([EXE, "--card"])
    time.sleep(3.2)
    ev = read_events()
    if not ev or ev[0][0] != "show":
        return p, None, ev
    return p, ev[0][1], ev


def case_inside():
    p, rect, ev = start()
    if not rect:
        stop(p)
        return False, "卡片没弹出来"
    w, h, x0, y0 = rect
    click(x0 + w // 2, y0 + h // 2)
    ev = read_events()
    stop(p)
    if any(e[0] == "hide" for e in ev):
        return False, "点卡片内部被误收起"
    return True, f"点中心 ({x0 + w // 2},{y0 + h // 2}) 后仍显示"


def case_outside():
    p, rect, ev = start()
    if not rect:
        stop(p)
        return False, "卡片没弹出来"
    w, h, x0, y0 = rect
    # 卡片右侧 60px、垂直居中的位置，保证落在卡片外
    click(x0 + w + 60, y0 + h // 2)
    ev = read_events()
    stop(p)
    if not any(e[0] == "hide" for e in ev):
        return False, "点卡片外部没有收起"
    return True, "点卡片外 %d px 处已收起" % 60


def case_esc():
    p, rect, ev = start()
    if not rect:
        stop(p)
        return False, "卡片没弹出来"
    press_esc()
    ev = read_events()
    stop(p)
    if not any(e[0] == "hide" for e in ev):
        return False, "按 Esc 没有收起"
    return True, "Esc 已收起"


def case_tray():
    p, rect, ev = start()
    if not rect:
        stop(p)
        return False, "卡片没弹出来"
    ok, err = tray_click()
    if not ok:
        stop(p)
        return False, err
    time.sleep(0.5)
    ev = read_events()
    stop(p)
    hides = [e for e in ev if e[0] == "hide"]
    if not hides:
        return False, "点托盘图标没有收起"
    if ev[-1][0] == "show":
        return False, "收起后又自己弹回来了"
    return True, "点托盘图标已收起且没弹回"


def case_open_then_outside():
    """完整链路：从托盘图标点开卡片，再点别处收起（不走 --card 自检）"""
    if os.path.exists(LOG):
        os.remove(LOG)
    p = subprocess.Popen([EXE])            # 正常启动，卡片先不显示
    time.sleep(2.5)
    if read_events():
        stop(p)
        return False, "启动就弹了卡片，不该这样"

    ok, err = tray_click()                 # 第一次点：打开
    if not ok:
        stop(p)
        return False, err

    ev = read_events()
    if not ev or ev[-1][0] != "show":
        stop(p)
        return False, "点托盘图标没有弹出卡片"
    rect = ev[-1][1]
    w, h, x0, y0 = rect
    # 点卡片左侧空白处（卡片外）
    click(x0 - 60, y0 + h // 2)
    ev = read_events()
    stop(p)
    if ev[-1][0] != "hide":
        return False, f"点卡片外 ({x0 - 60},{y0 + h // 2}) 没有收起"
    return True, "托盘点开 -> 点卡片外收起"


CASES = {"inside": case_inside, "outside": case_outside,
         "esc": case_esc, "tray": case_tray,
         "open_then_outside": case_open_then_outside}


def main():
    names = sys.argv[1:] or ["inside", "outside", "esc", "tray",
                             "open_then_outside"]
    bad = 0
    for n in names:
        fn = CASES.get(n)
        if not fn:
            print(f"未知用例 {n}")
            bad += 1
            continue
        ok, msg = fn()
        print(("PASS " if ok else "FAIL ") + n + ": " + msg)
        if not ok:
            bad += 1
        time.sleep(0.4)
    print("---- %d/%d 通过" % (len(names) - bad, len(names)))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
