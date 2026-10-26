"""lowtest.py — 低电量提醒测试矩阵（诊断构建 tray_lowtest.exe）

真机鼠标 43%、键盘 100%，没法等它们掉到 20% 以下，所以用 `build.bat lowtest`
编一个 **只在诊断构建里存在** 的注入开关：exe 目录下的 fake_batt.txt 写多少，
就把鼠标伪装成那个电量（正式 tray.exe 里没有这段代码）。

覆盖的用例：
  A 跌破阈值        15% / low_pct=20  -> 报一次
  B 不重复报        再等 8 秒         -> 仍只有 1 条
  C 阈值下不武装    18%（<=20 但 <25）-> 不再报
  D 重新武装        30%（>=recover 25）后再回 15% -> 第 2 条
  E 低位错误色      fake=8            -> dwInfoFlags=3 (NIIF_ERROR)
  F 关掉提醒        low_notify=0      -> 一条都不报
  G 气泡真的弹出    截图右下角像素统计

用法：
    python lowtest.py matrix     # 跑 A~F，结束后自动还原并重启正式版
    python lowtest.py shot       # 只跑 A 并截屏（快捷人工看气泡）
    python lowtest.py restore    # 只还原
"""
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "tray.exe")
DIAG = os.path.join(ROOT, "tray_lowtest.exe")
INI = os.path.join(ROOT, "settings.ini")
LOG = os.path.join(ROOT, "tray.log")
FAKE = os.path.join(ROOT, "fake_batt.txt")
ORIG = os.path.join(HERE, "settings.orig.ini")
SHOT = os.path.join(HERE, "low_shot.png")

NIIF_INFO, NIIF_WARNING, NIIF_ERROR = 1, 2, 3

fails = []


def ok(cond, msg):
    print(("  [ok]   " if cond else "  [FAIL] ") + msg)
    if not cond:
        fails.append(msg)
    return cond


def kill(name):
    subprocess.run(["taskkill", "/IM", name, "/F"], capture_output=True)
    time.sleep(0.7)


def start(exe):
    # 用 ShellExecute（os.startfile）拉起：完全独立于本进程，
    # 测试脚本退出不会把它带走。别用 Popen——那是子进程，会被一起收走。
    os.startfile(exe)
    time.sleep(1.5)


def set_fake(v):
    """v 可以是 15，也可以是 (15, 12)：第二个值伪装「其余设备」，用来测多台合并"""
    with open(FAKE, "w", encoding="ascii") as f:
        f.write(" ".join(str(x) for x in (v if isinstance(v, tuple) else (v,))) + "\n")


def patch_ini(low, notify):
    with open(INI, "r", encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"(?m)^low_pct\s*=.*$", "low_pct = %d" % low, text)
    text = re.sub(r"(?m)^recover_pct\s*=.*$", "recover_pct = %d" % (low + 5), text)
    text = re.sub(r"(?m)^low_notify\s*=.*$", "low_notify = %d" % notify, text)
    with open(INI, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def read_log():
    try:
        with open(LOG, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def notices():
    """返回 (设备名, 电量) 列表，按出现顺序"""
    return re.findall(r"低电量提醒: (.+?) (\d+)%", read_log())


def ncount():
    return len(re.findall(r"低电量提醒:", read_log()))


def tcount():
    return len(re.findall(r"toast ok=", read_log()))


def last_toast():
    """(ok, flags, title, info, err) —— 诊断构建记录的外壳返回值与最终文案"""
    m = re.findall(r"toast ok=(\d+) flags=(\d+) title=(.*?) info=(.*?) err=(\d+)$",
                   read_log(), re.M)
    if not m:
        return None
    ok_, flags, title, info, err = m[-1]
    return int(ok_), int(flags), title, info, int(err)


def wait_notice(timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ncount():
            return True
        time.sleep(0.15)
    return False


def last_flags():
    m = re.findall(r"notice flags=(\d+) pct=(-?\d+) title=(\S+)", read_log())
    return (int(m[-1][0]), int(m[-1][1]), m[-1][2]) if m else None


def shot(out):
    import ctypes

    import shot as sh
    try:
        sh.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        sh.user32.SetProcessDPIAware()
    w = sh.user32.GetSystemMetrics(0)
    h = sh.user32.GetSystemMetrics(1)
    raw, w2, h2 = sh.capture(0, 0, w, h)
    sh.write_png(out, raw, w2, h2)
    return w2, h2


def region_stats(path, x0f, y0f):
    """统计指定起始比例之后的区域里「亮像素/暗像素」占比，
    气泡是纯白底 + 深色文字，两者会同时出现。"""
    import struct
    import zlib

    with open(path, "rb") as f:
        data = f.read()
    pos, w, h, idat, ctype, depth = 8, 0, 0, b"", 2, 8
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
            depth, ctype = body[8], body[9]
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    if depth != 8 or ctype != 2:
        return None
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    x0, y0 = int(w * x0f), int(h * y0f)
    bright = dark = tot = 0
    for y in range(y0, h):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(x0, w):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            tot += 1
            if r > 235 and g > 235 and b > 235:
                bright += 1
            elif r < 90 and g < 90 and b < 90:
                dark += 1
    if not tot:
        return None
    return bright / tot, dark / tot


def boot(low, notify, fake):
    """改配置 + 起诊断版，返回启动后新增的通知条数基线"""
    kill("tray.exe")
    kill("tray_lowtest.exe")
    patch_ini(low, notify)
    set_fake(fake)
    base = ncount()
    start(DIAG)
    return base


def case_matrix():
    with open(INI, "rb") as f, open(ORIG, "wb") as g:
        g.write(f.read())
    print("[..] 备份 settings.ini -> probe/settings.orig.ini")

    # ---------- A 跌破阈值 ----------
    print("\nA 跌破阈值：low_pct=20，fake=15")
    tb = tcount()
    base = boot(20, 1, 15)
    hit = wait_notice()
    ok(hit, "日志出现 low battery notice")
    got = notices()
    ok(len(got) == (base + 1) and got[-1][1] == "15",
       "只报 1 条且电量=15（实际 %d 条，最后一条 %s）" % (len(got), got[-1] if got else "-"))
    ok("鼠标" in got[-1][0], "报的是鼠标（%s）" % got[-1][0])
    t = last_toast()
    ok(t is not None and t[0] == 1, "外壳受理（Shell_NotifyIconW 返回 TRUE）")
    ok(t is not None and t[1] == NIIF_WARNING,
       "dwInfoFlags=%s（期望 NIIF_WARNING=2，与 Python 版一致）" % (t[1] if t else "无"))
    ok(t is not None and t[2] == "设备电量不足", "标题=%s" % (t[2] if t else "无"))
    ok(t is not None and t[3] == "迈从 G3 鼠标 剩余 15%，建议尽快充电（点击查看详情）",
       "正文=%s" % (t[3] if t else "无"))
    ok(tcount() == tb + 1, "只发了一条气泡（实际 +%d）" % (tcount() - tb))

    # ---------- G 气泡截图 ----------
    w, h = shot(SHOT)
    st = region_stats(SHOT, 0.55, 0.5)
    print("  [..] 截屏 %dx%d -> %s  右下亮/暗占比 %s"
          % (w, h, os.path.basename(SHOT),
             "(%.3f, %.3f)" % st if st else "n/a"))

    # ---------- B 不重复报 ----------
    print("\nB 不重复报：等 8 秒")
    time.sleep(8)
    ok(ncount() == base + 1, "8 秒后仍只有 %d 条（实际 %d）" % (base + 1, ncount()))

    # ---------- C 阈值下不武装 ----------
    print("\nC 阈值下方不上报：fake=18（<=20 但 <recover 25）")
    set_fake(18)
    time.sleep(4)
    ok(ncount() == base + 1, "仍是 %d 条（实际 %d）" % (base + 1, ncount()))

    # ---------- D 重新武装 ----------
    print("\nD 重新武装：fake=30（>=recover 25）后再回 15")
    set_fake(30)
    time.sleep(4)
    ok(ncount() == base + 1, "抬高后不报（%d 条）" % ncount())
    set_fake(15)
    t0 = time.time()
    while time.time() - t0 < 6 and ncount() == base + 1:
        time.sleep(0.2)
    ok(ncount() == base + 2, "回落后再报 1 条（共 %d 条，期望 %d）"
       % (ncount(), base + 2))

    # ---------- E 极低电量：文案随电量走，等级仍是警告 ----------
    print("\nE 极低电量：fake=8，文案要跟着变、等级不变")
    base = boot(20, 1, 8)
    wait_notice()
    t = last_toast()
    ok(t is not None and t[1] == NIIF_WARNING,
       "dwInfoFlags=%s（Python 版不按等级变色，仍应为 2）" % (t[1] if t else "无"))
    ok(t is not None and "剩余 8%" in t[3], "正文含「剩余 8%%」（%s）" % (t[3] if t else "无"))
    ok(ncount() == base + 1, "只报 1 条")

    # ---------- G 两台同时低，合并成一条 ----------
    print("\nG 多台合并：fake=(15,12) 键盘 12% / 鼠标 15%")
    tb = tcount()
    base = boot(20, 1, (15, 12))
    t0 = time.time()
    while time.time() - t0 < 8 and tcount() == tb:
        time.sleep(0.2)
    t = last_toast()
    ok(t is not None and tcount() == tb + 1, "只发 1 条气泡（实际 +%d）" % (tcount() - tb))
    ok(t is not None and t[3] ==
       "狼蛛 FN75 键盘 剩余 12%、迈从 G3 鼠标 剩余 15%，建议尽快充电（点击查看详情）",
       "正文=%s" % (t[3] if t else "无"))
    ok(ncount() == base + 2, "两台各记一条日志（实际 +%d）" % (ncount() - base))
    set_fake(15)

    # ---------- F 关掉提醒 ----------
    print("\nF low_notify=0：fake=15，不该有任何通知")
    base = boot(20, 0, 15)
    time.sleep(8)
    ok(ncount() == base, "0 条新通知（实际 %d）" % (ncount() - base))


def case_restore():
    kill("tray_lowtest.exe")
    if os.path.exists(ORIG):
        shutil.copyfile(ORIG, INI)
        os.remove(ORIG)
    if os.path.exists(FAKE):
        os.remove(FAKE)
    start(EXE)
    cfg = open(INI, encoding="utf-8").read()
    print("[ok] settings.ini 已还原，正式版 tray.exe 已重启")
    print("[..] 当前配置：", " / ".join(
        l.strip() for l in cfg.splitlines()
        if l.startswith(("low_pct", "recover_pct", "low_notify"))))


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "matrix"
    try:
        if cmd == "restore":
            case_restore()
        elif cmd == "shot":
            with open(INI, "rb") as f, open(ORIG, "wb") as g:
                g.write(f.read())
            boot(20, 1, 15)
            print("notice:", wait_notice())
            shot(SHOT)
            print("shot ->", SHOT)
        else:
            case_matrix()
    finally:
        case_restore()
    print("\n==== %s ====" % ("全部通过" if not fails else "失败 %d 项" % len(fails)))
    for f in fails:
        print("  -", f)
    sys.exit(1 if fails else 0)
