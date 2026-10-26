/* tray.c — 托盘主程序（C 版）
 *
 * 结构：
 *   主窗口（隐藏）  接托盘图标的消息、定时器、菜单
 *   卡片窗口        WS_POPUP，点托盘弹出，点外部 / Esc 收起
 *   监控线程        devmon，收帧解析后 PostMessage 通知主窗口
 *
 * 设计目标是「极轻」：不用 CRT 以外的任何库，图标与卡片全部 GDI 手绘，
 * 空闲时把工作集还给系统。
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "art.h"
#include "devmon.h"
#include "log.h"
#include "settings.h"

#define WM_TRAY_MSG     (WM_APP + 1)
#define WM_DM_CHANGED   (WM_APP + 2)
#define WM_HIDE_CARD    (WM_APP + 3)
#define TIMER_TICK      1
#define TIMER_SELFTEST  2
#define TIMER_DISMISS   3
#define IDI_TRAY        1
#define TICK_MS         1000
#define DISMISS_MS      20       /* 外部点击检测间隔；只在卡片显示时才开这个定时器 */

#define IDM_AUTOSTART   101
#define IDM_NOTIFY      102
#define IDM_LOG         103
#define IDM_EXIT        104

#define RUN_KEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE       L"2.4GBatteryTray"

/* 版式：与 Python 版 BatteryTray 的类常量一一对应（逻辑像素 @96dpi） */
#define CARD_W          352
#define CARD_PAD        18
#define CARD_HEADER     44
#define CARD_ROW_H      100
#define CARD_FOOTER     34
#define CARD_RADIUS     12
#define CHIP_SIZE       42       /* 设备图标底衬边长 */
#define ICON_SIZE       28       /* 设备图标边长 */

/* 配色：取自 Python 版 tray_app.py 顶部（白底、深灰大字、亮绿胶囊进度条） */
#define C_BG            RGB(0xFF, 0xFF, 0xFF)
#define C_BORDER        RGB(0xEC, 0xEA, 0xEF)
#define C_TITLE         RGB(0x30, 0x31, 0x3A)
#define C_NAME          RGB(0x4C, 0x50, 0x5B)
#define C_UNIT          RGB(0xA9, 0xA6, 0xAE)
#define C_META          RGB(0xAF, 0xB2, 0xBA)
#define C_FOOT          RGB(0xBC, 0xBF, 0xC6)
#define C_TRACK         RGB(0xF1, 0xEF, 0xF2)
#define C_ICONBG        RGB(0xF4, 0xF9, 0xFE)
#define C_DIV           RGB(0xEF, 0xED, 0xF1)
#define C_GOOD          RGB(0x7F, 0xFF, 0x62)
#define C_WARN          RGB(0xFF, 0xB0, 0x20)
#define C_BAD           RGB(0xFF, 0x5A, 0x5A)
#define C_OFF           RGB(0xC9, 0xCC, 0xD2)

#define STALE_SECONDS   300      /* 超过这么久没更新，补一句「可能已关机」 */

static HINSTANCE g_inst;
static HWND      g_wnd, g_card;
static HICON     g_icon;              /* 当前托盘图标（我们自己画的） */
static NOTIFYICONDATAW g_nid;
static Cfg       g_cfg;
static WCHAR     g_exe_dir[MAX_PATH], g_exe_path[MAX_PATH], g_ini[MAX_PATH];
static int       g_card_shown;
static ULONGLONG g_last_active;
static ULONGLONG g_trim_at;
static DWORD     g_card_shown_tick;
static int       g_alerted[DM_MAX_DEV];
static int       g_icon_key = -999;   /* 图标缓存键，避免每秒重建 */
static int       g_quitting;

/* 诊断构建（-DLOWTEST）专用：读 exe 目录下的 fake_batt.txt 强制电量，
 * 用来确定性验证低电量提醒的阈值 / 去重 / 重新武装，不依赖真电池放电。
 * 正式构建里这段代码根本不存在（#ifdef 全剥掉）。 */
#ifdef LOWTEST
static int       g_fake_batt  = -1;    /* 鼠标（或第一台）强制的电量，-1 = 不注入 */
static int       g_fake_batt2 = -1;    /* 其余设备的电量，-1 = 保持真实数据 */

static void fake_batt_load(void)
{
    WCHAR  p[MAX_PATH];
    HANDLE h;
    char   buf[32];
    DWORD  got = 0;
    int    a = -1, b = -1;

    _snwprintf(p, MAX_PATH, L"%ls\\fake_batt.txt", g_exe_dir);
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        /* 一行一个值：第一个给鼠标，第二个给其余设备（用来测多台合并） */
        if (sscanf(buf, "%d %d", &a, &b) >= 1) {
            g_fake_batt  = a;
            g_fake_batt2 = b;
        }
    }
    CloseHandle(h);
}
#endif

/* 外部点击检测的状态。
 * 卡片靠 WM_ACTIVATE 判断失活是不可靠的：托盘程序不是前台进程，
 * SetForegroundWindow 常被系统拒绝，卡片从未被激活，也就永远收不到
 * WA_INACTIVE —— 表现就是「点别处不消失」。改成自己轮询按键。 */
static int       g_btn_was_down;
static int       g_esc_was_down;
static DWORD     g_click_dismiss_tick;  /* 因点击外部而收起的时刻 */
static HHOOK     g_mouse_hook;          /* 卡片显示期间的鼠标钩子 */

static HFONT g_f_title, g_f_name, g_f_pct, g_f_sign, g_f_unit, g_f_meta;
static int   g_dpi = 96;

/* 逻辑像素 → 物理像素（版式按 96 DPI 设计） */
#define DI(v) ((v) * g_dpi / 96)

/* --------------------------------------------------------------- 小工具 */

static UINT dpi_of(HWND h)
{
    HDC dc = GetDC(h);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(h, dc);
    return d ? d : 96;
}

/* px100 = 逻辑像素高 ×100（这样 11.5px 这种半像素值也能精确表达）。
 * Python 版是用负高度直接指定像素，这里保持一致，字体大小才对得上。 */
static HFONT make_font_px(int px100, int weight)
{
    int h = -(int)((double)px100 * g_dpi / 9600.0);

    if (h > -8)
        h = -8;
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static void get_paths(void)
{
    WCHAR *p;

    GetModuleFileNameW(NULL, g_exe_path, MAX_PATH);
    wcscpy(g_exe_dir, g_exe_path);
    p = wcsrchr(g_exe_dir, L'\\');
    if (p)
        *p = 0;
    _snwprintf(g_ini, MAX_PATH, L"%ls\\settings.ini", g_exe_dir);
    g_ini[MAX_PATH - 1] = 0;
}

/* 进度条颜色：断档与 Python 版 bar_color 一致（<20 红 / <45 橙 / 其余亮绿） */
static COLORREF bar_color(int pct)
{
    if (pct < 0)
        return C_OFF;
    if (pct < 20)
        return C_BAD;
    if (pct < 45)
        return C_WARN;
    return C_GOOD;
}

/* 相对时间：刚刚 / 42 秒前 / 3 分钟前（照抄 Python 版 rel_time） */
static void rel_time(WCHAR *out, int cap, DWORD age)
{
    if (age < 10)
        wcscpy(out, L"刚刚");
    else if (age < 60)
        _snwprintf(out, cap, L"%lu 秒前", age);
    else if (age < 3600)
        _snwprintf(out, cap, L"%lu 分钟前", age / 60);
    else if (age < 86400)
        _snwprintf(out, cap, L"%lu 小时前", age / 3600);
    else
        _snwprintf(out, cap, L"%lu 天前", age / 86400);
    out[cap - 1] = 0;
}

/* --------------------------------------------------------------- 托盘图标 */

/* 托盘图标直接用 art.c 里的矢量电池：几何、配色、抗锯齿都与 Python 版一致 */
static HICON render_icon(int pct, int charging)
{
    return art_battery_icon(pct, charging);
}

/* --------------------------------------------------------------- 卡片窗口 */

/* 设备行数：没有设备时也要留一行给提示文案 */
static int card_rows(int n)
{
    return n > 0 ? n : 1;
}

static int card_height(int rows)
{
    return DI(CARD_HEADER) + rows * DI(CARD_ROW_H) + DI(CARD_FOOTER);
}

static void card_layout(int *w, int *h)
{
    DmState st[DM_MAX_DEV];
    int     n = dm_copy(st, DM_MAX_DEV);

    *w = DI(CARD_W);
    *h = card_height(card_rows(n));
}

static const WCHAR *dev_display_name(int idx, const DmState *s)
{
    if (idx == 0 && g_cfg.kbd_name[0])
        return g_cfg.kbd_name;
    if (idx == 1 && g_cfg.mouse_name[0])
        return g_cfg.mouse_name;
    return s->name;
}

/* --------------------------------------------------------------- GDI 辅助
 * 名字与语义对应 Python 版的 _fill / _round_fill / _round_stroke / _text，
 * 参数一律是「逻辑像素」，进去先乘 DPI。
 */

static void fill_rect(HDC dc, COLORREF c, int x0, int y0, int x1, int y1)
{
    RECT   r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);

    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* r 是圆角半径（GDI 的 RoundRect 要的是椭圆长宽，所以传 r*2） */
static void round_fill(HDC dc, COLORREF c, int x0, int y0, int x1, int y1, int r)
{
    HBRUSH b   = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));

    RoundRect(dc, x0, y0, x1, y1, r * 2, r * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void round_stroke(HDC dc, COLORREF c, int x0, int y0, int x1, int y1, int r)
{
    HPEN    p  = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));

    RoundRect(dc, x0, y0, x1, y1, r * 2, r * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

static void draw_text(HDC dc, const WCHAR *s, HFONT f, COLORREF c,
                      int x, int y, int w, int h, UINT align)
{
    RECT    r = { x, y, x + w, y + h };
    HGDIOBJ old;

    if (w <= 0 || h <= 0 || !s || !s[0])
        return;
    old = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, align | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

static int text_w(HDC dc, const WCHAR *s, HFONT f)
{
    SIZE    sz;
    HGDIOBJ old = SelectObject(dc, f);

    sz.cx = 0;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

/* 副标题行：「充电中 · 设备休眠 · 3 分钟前更新 · 可能已关机」，对应 _meta_line */
static void meta_line(const DmState *s, WCHAR *out, int cap)
{
    WCHAR bits[5][DM_MAXSTR];
    int   nb = 0, i, used = 0;
    DWORD now = GetTickCount();

    out[0] = 0;

    if (s->charging == 1)
        wcscpy(bits[nb++], L"充电中");
    if (s->online == DM_OFFLINE)
        wcscpy(bits[nb++], L"设备休眠");

    if (s->last_frame_ms) {
        WCHAR t[32];

        rel_time(t, 32, (now - s->last_frame_ms) / 1000);
        _snwprintf(bits[nb], DM_MAXSTR, L"%ls更新", t);
        bits[nb][DM_MAXSTR - 1] = 0;
        nb++;
        if ((now - s->last_frame_ms) / 1000 > STALE_SECONDS
            && s->online != DM_OFFLINE)
            wcscpy(bits[nb++], L"可能已关机");
    } else if (s->note[0]) {
        wcsncpy(bits[nb], s->note, DM_MAXSTR - 1);
        bits[nb][DM_MAXSTR - 1] = 0;
        nb++;
    }

    if (!nb) {
        wcscpy(bits[nb++], s->kind == DM_KIND_KEYBOARD ? L"Compx 键盘" : L"迈从鼠标");
    }

    for (i = 0; i < nb; i++) {
        int len = (int)wcslen(bits[i]);

        if (used + len + 4 >= cap)
            break;
        if (used) {
            wcscat(out, L" · ");
            used += 3;
        }
        wcscat(out, bits[i]);
        used += len;
    }
}

/* 一行设备：底衬 + 图标 + 名称 + 大号百分比 + 胶囊进度条 + 副标题 */
static void paint_row(HDC dc, const DmState *s, int idx, int y, int w)
{
    int      pad   = DI(CARD_PAD);
    int      box   = DI(CHIP_SIZE);
    int      tx    = pad + box + DI(14);
    int      right = w - pad;
    int      by    = y + DI(10);
    int      isz   = DI(ICON_SIZE);
    int      py    = y + DI(27);
    int      ph    = DI(35);
    int      pct   = s->battery;
    int      nw, sw, sx, ux;
    WCHAR    num[16], meta[192];
    COLORREF color;

    /* 图标底衬（浅蓝圆角块） */
    round_fill(dc, C_ICONBG, pad, by, pad + box, by + box, DI(12));
    art_device_icon(dc, s->kind == DM_KIND_KEYBOARD ? ART_KBD : ART_MOUSE,
                    pad + (box - isz) / 2, by + (box - isz) / 2, isz, C_ICONBG);

    /* 设备名 */
    draw_text(dc, dev_display_name(idx, s), g_f_name, C_NAME,
              tx, y + DI(9), right - tx, DI(18), DT_VCENTER | DT_LEFT);

    /* 大号数字 + % + 「电量」 */
    if (pct >= 0)
        _snwprintf(num, 16, L"%d", pct);
    else
        wcscpy(num, L"--");
    num[15] = 0;
    color = pct >= 0 ? C_TITLE : C_OFF;

    nw = text_w(dc, num, g_f_pct);
    draw_text(dc, num, g_f_pct, color, tx, py, nw + DI(3), ph,
              DT_BOTTOM | DT_LEFT);

    sx = tx + nw;
    sw = text_w(dc, L"%", g_f_sign);
    draw_text(dc, L"%", g_f_sign, color, sx, py, sw + DI(4), ph,
              DT_BOTTOM | DT_LEFT);

    ux = sx + sw + DI(5);
    draw_text(dc, L"电量", g_f_unit, C_UNIT, ux, py,
              (right - ux) > DI(10) ? (right - ux) : DI(10), ph,
              DT_BOTTOM | DT_LEFT);

    /* 胶囊进度条 */
    {
        int bar_y = y + DI(66);
        int bar_h = DI(10);
        int span  = right - tx;

        round_fill(dc, C_TRACK, tx, bar_y, right, bar_y + bar_h, bar_h / 2);
        if (pct > 0 && span > 0) {
            int fw = (int)((double)span * (pct > 100 ? 100 : (pct < 2 ? 2 : pct))
                           / 100.0);

            if (fw < bar_h)
                fw = bar_h;
            if (fw > span)
                fw = span;
            round_fill(dc, bar_color(pct), tx, bar_y, tx + fw, bar_y + bar_h,
                       bar_h / 2);
        }
    }

    /* 更新时间 / 状态 */
    meta_line(s, meta, 192);
    draw_text(dc, meta, g_f_meta, C_META, tx, y + DI(81), right - tx, DI(15),
              DT_VCENTER | DT_LEFT);
}

static void paint_card(HWND hwnd, HDC target)
{
    RECT     rc;
    HDC      dc;
    HBITMAP  mem;
    DmState  st[DM_MAX_DEV];
    int      n, i;
    int      pad, header_h, row_h, footer_h;

    GetClientRect(hwnd, &rc);
    dc  = CreateCompatibleDC(target);
    mem = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    SelectObject(dc, mem);

    pad      = DI(CARD_PAD);
    header_h = DI(CARD_HEADER);
    row_h    = DI(CARD_ROW_H);
    footer_h = DI(CARD_FOOTER);

    fill_rect(dc, C_BG, 0, 0, rc.right, rc.bottom);
    round_stroke(dc, C_BORDER, 1, 1, rc.right - 1, rc.bottom - 1, DI(CARD_RADIUS));
    SetBkMode(dc, TRANSPARENT);
    n = dm_copy(st, DM_MAX_DEV);

    /* 标题行 */
    draw_text(dc, L"设备电量", g_f_title, C_TITLE, pad, 0,
              rc.right - pad * 2 - DI(64), header_h, DT_VCENTER | DT_LEFT);
    if (n > 0) {
        WCHAR cnt[32];

        _snwprintf(cnt, 32, L"%d 台设备", n);
        cnt[31] = 0;
        draw_text(dc, cnt, g_f_meta, C_META, rc.right - pad - DI(64), 0,
                  DI(64), header_h, DT_VCENTER | DT_RIGHT);
    }

    /* 设备行 */
    if (n == 0) {
        int block = DI(46);
        int ey    = header_h + (row_h - block) / 2;

        draw_text(dc, L"未检测到 2.4G 接收器", g_f_name, C_NAME, pad, ey,
                  rc.right - pad * 2, DI(24), DT_VCENTER | DT_LEFT);
        draw_text(dc, L"请确认接收器已插好，或该型号尚未适配", g_f_meta, C_META,
                  pad, ey + DI(24), rc.right - pad * 2, DI(20),
                  DT_VCENTER | DT_LEFT);
    } else {
        int y = header_h;

        for (i = 0; i < n; i++) {
            paint_row(dc, &st[i], i, y, rc.right);
            if (i < n - 1)      /* 行间分隔线，从图标右侧开始 */
                fill_rect(dc, C_DIV, pad + DI(56), y + row_h - 1,
                          rc.right - pad, y + row_h);
            y += row_h;
        }
    }

    /* 底部说明：解释刷新节奏，避免用户误以为鼠标卡住。
     * Python 版这里写「每 10 分钟」，C 版为了断连检测每 6 秒查一次，
     * 文案跟着实际行为走，不自相矛盾。 */
    draw_text(dc, L"键盘每 6 秒查询 · 鼠标实时接收", g_f_meta, C_FOOT,
              pad, rc.bottom - footer_h, rc.right - pad * 2, footer_h,
              DT_VCENTER | DT_CENTER);

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    DeleteObject(mem);
    DeleteDC(dc);
}

static void card_show(void);
static void card_hide(void);

/* --------------------------------------------------------------- 鼠标钩子
 *
 * 卡片是 WS_POPUP 浮层，靠 WM_ACTIVATE/WA_INACTIVE 判断「点了别处」并不可靠：
 * 托盘程序通常不是前台进程，SetForegroundWindow 会被系统前台锁拒绝，
 * 卡片从未获得激活，就永远收不到失活消息 —— 症状就是「点哪都不消失」。
 *
 * 一开始试的是定时器轮询 GetAsyncKeyState，但 WM_TIMER 会被系统合并、延迟，
 * 20ms 的周期实测仍有约三分之一的点击被整段错过（一次按下只有 80~150ms）。
 * 所以改成 WH_MOUSE_LL：事件驱动，一次都不会漏。
 *
 * 两个要点：
 *   1. 钩子里只投消息，不直接操作窗口 —— 钩子回调必须尽快返回，
 *      否则超过 LowLevelHooksTimeout 会被系统静默摘掉。
 *   2. 一律 CallNextHookEx，不吞任何事件，点卡片外的操作照常生效。
 */
static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_card_shown && g_card
        && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN
            || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN)) {
        const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lp;
        RECT r;

        if (GetWindowRect(g_card, &r) && !PtInRect(&r, ms->pt))
            PostMessageW(g_wnd, WM_HIDE_CARD, 0, 0);
    }
    return CallNextHookEx(g_mouse_hook, code, wp, lp);
}

static LRESULT CALLBACK card_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_card(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            /* 刚弹出时可能立刻收到失活，忽略这 300ms 内的抖动。
             * 注意必须走 card_hide()：直接 ShowWindow 不会清 g_card_shown，
             * 之后点托盘会走「已显示 → 收起」分支，表现成第一下点击没反应。
             *
             * 这条消息只在「卡片真的拿到前台」时才会来。托盘程序不是前台进程时
             * 系统会拒绝 SetForegroundWindow，卡片从未激活，这条分支永远不触发
             * —— 所以真正的收起逻辑在 poll_dismiss()，这里只是更快的一条捷径。
             * 用 -DNO_ACT_HIDE 编译可以屏蔽它，验证点击检测能否独立工作。 */
            if (GetTickCount() - g_card_shown_tick > 300) {
#ifndef NO_ACT_HIDE
                card_hide();
#else
                log_line("inactive ignored (built with NO_ACT_HIDE)");
#endif
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            card_hide();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void card_show(void)
{
    POINT     pt;
    RECT      wa;
    MONITORINFO mi;
    int       w, h, x, y;
    HWND      fg;
    DWORD     t_me, t_fg;

    card_layout(&w, &h);
    GetCursorPos(&pt);

    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi))
        wa = mi.rcWork;
    else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    }

    /* 贴着托盘图标往上弹，并保证完整落在工作区内（同 Python 版 _show_panel） */
    x = pt.x - w + DI(26);
    y = pt.y - h - DI(12);
    if (x < wa.left + DI(8))
        x = wa.left + DI(8);
    if (x + w > wa.right - DI(8))
        x = wa.right - w - DI(8);
    if (y < wa.top + DI(8))
        y = wa.top + DI(8);
    if (y + h > wa.bottom - DI(8))
        y = wa.bottom - h - DI(8);

    log_line("card_show %dx%d at %d,%d", w, h, x, y);
    SetWindowPos(g_card, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);

    /* 圆角区域必须跟着尺寸重建：设备数量变了高度就变，
     * 沿用建窗口时那份旧区域会把底下几行裁掉。 */
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, DI(13), DI(13));

        if (rgn)
            SetWindowRgn(g_card, rgn, TRUE);
    }
    g_card_shown_tick = GetTickCount();
    InvalidateRect(g_card, NULL, TRUE);

    /* 前台锁：前台窗口属于别的线程时 SetForegroundWindow 会被系统拒绝，
     * 拿不到激活就收不到 WM_ACTIVATE，卡片会「点外面也关不掉」。
     * 临时挂到前台线程的输入队列上可以绕过限制。 */
    t_me = GetCurrentThreadId();
    fg   = GetForegroundWindow();
    t_fg = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (t_fg && t_fg != t_me) {
        AttachThreadInput(t_fg, t_me, TRUE);
        SetForegroundWindow(g_card);
        AttachThreadInput(t_fg, t_me, FALSE);
    } else {
        SetForegroundWindow(g_card);
    }
    SetFocus(g_card);
    g_card_shown = 1;

    /* 弹出瞬间如果左键还按着（点击托盘那一下），先记成「已按下」，
     * 必须等松开再按一次才算「点了外面」，否则卡片会当场被自己收掉。 */
    g_btn_was_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    g_esc_was_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

    /* 主路径：鼠标钩子（事件驱动，不漏点击）。
     * 万一钩子装不上（被安全软件拦或达到钩子数量上限），退回定时器轮询：
     * WM_TIMER 会被系统合并延迟，只能算尽力而为，但总比完全没有强。 */
    if (!g_mouse_hook) {
        g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc,
                                         GetModuleHandleW(NULL), 0);
        if (!g_mouse_hook)
            log_line("mouse hook failed (err %lu), fall back to polling",
                     GetLastError());
    }
    /* 有钩子时这个定时器只用来查 Esc（150ms 一次，肉眼看不出延迟）；
     * 没钩子才靠它采样按键，那时才需要 20ms 的高频周期 */
    SetTimer(g_wnd, TIMER_DISMISS, g_mouse_hook ? 150 : DISMISS_MS, NULL);
}

static void card_hide(void)
{
    if (!g_card_shown)
        return;
    log_line("card_hide");
    if (g_card)
        ShowWindow(g_card, SW_HIDE);
    g_card_shown = 0;
    if (g_wnd)
        KillTimer(g_wnd, TIMER_DISMISS);
    if (g_mouse_hook) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = NULL;
    }
}

/* --------------------------------------------------------------- 外部点击
 *
 * 卡片是 WS_POPUP 浮层，靠 WM_ACTIVATE/WA_INACTIVE 判断「点了别处」并不可靠：
 * 托盘程序通常不是前台进程，SetForegroundWindow 会被系统前台锁拒绝，
 * 卡片从未获得激活，就永远收不到失活消息 —— 症状就是「点哪都不消失」。
 *
 * 这里改成一个按键轮询：只看按键是否刚被按下，再问光标在不在卡片矩形里。
 * 周期必须压到 20ms —— 一次点击的按下阶段只有 80~150ms，120ms 的周期会
 * 整段错过（实测过一次真漏判）。20ms 一次两次 GetAsyncKeyState 的开销
 * 可以忽略，而且只在卡片显示期间才启用这个定时器，隐藏后立刻关掉。
 */
static void poll_dismiss(void)
{
    POINT p;
    RECT  r;
    int   down;

    if (!g_card_shown || !g_card || !IsWindowVisible(g_card)) {
        /* 没显示时也要跟踪按键状态，免得「按着不放」被算成一次新按下 */
        g_btn_was_down = ((GetAsyncKeyState(VK_LBUTTON)
                           | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
        g_esc_was_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        return;
    }

    /* Esc 收起：不依赖焦点，所以没抢到前台也能用 */
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        if (!g_esc_was_down) {
            g_esc_was_down = 1;
            card_hide();
            return;
        }
        g_esc_was_down = 1;
    } else {
        g_esc_was_down = 0;
    }

    /* 有钩子时点击检测交给钩子（1 秒一次的这个轮询就只用来查 Esc），
     * 没钩子才退回按键采样，避免两套逻辑各判一次 */
    if (!g_mouse_hook) {
        down = ((GetAsyncKeyState(VK_LBUTTON)
                 | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;

        /* 只认「松开 → 按下」的那一瞬间，长按不会反复触发 */
        if (down && !g_btn_was_down) {
            GetCursorPos(&p);
            GetWindowRect(g_card, &r);
            log_line("click at %d,%d  card %d,%d-%d,%d  %ls", p.x, p.y,
                     r.left, r.top, r.right, r.bottom,
                     PtInRect(&r, p) ? L"inside" : L"outside");
            if (!PtInRect(&r, p)) {
                g_click_dismiss_tick = GetTickCount();
                card_hide();
                g_btn_was_down = down;
                return;
            }
        }
        g_btn_was_down = down;
    }
}

/* --------------------------------------------------------------- 自动启动 */

static int autostart_state(void)
{
    HKEY  k;
    WCHAR v[MAX_PATH + 4];
    DWORD sz = sizeof(v), type = 0;
    int   on = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, RUN_VALUE, NULL, &type, (BYTE *)v, &sz) == ERROR_SUCCESS)
            on = 1;
        RegCloseKey(k);
    }
    return on;
}

/* 注册表里的自启路径是不是当前这个 exe。
 *
 * 绿色版会被搬到任意目录（U 盘、D:\tools、别人电脑），如果只判断「有没有这个值」，
 * 搬家之后开机启动仍然指向旧位置 —— 旧目录一删就静默失效。所以要连路径一起核对。 */
static int autostart_points_to_self(void)
{
    HKEY  k;
    WCHAR v[MAX_PATH + 4] = {0};
    DWORD sz = (MAX_PATH + 3) * sizeof(WCHAR), type = 0;
    int   same = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, RUN_VALUE, NULL, &type, (BYTE *)v, &sz) == ERROR_SUCCESS
        && type == REG_SZ) {
        WCHAR *p = v;
        size_t n;

        while (*p == L' ' || *p == L'"')
            p++;                                   /* 值是 "\"C:\...\a.exe\"" 带引号 */
        n = wcslen(p);
        while (n > 0 && (p[n - 1] == L'"' || p[n - 1] == L' '))
            p[--n] = 0;
        same = (_wcsicmp(p, g_exe_path) == 0);
    }
    RegCloseKey(k);
    return same;
}

static void autostart_apply(int on)
{
    HKEY k;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        log_line("autostart: open Run key failed (%lu)", GetLastError());
        return;
    }
    if (on) {
        WCHAR q[MAX_PATH + 4];
        _snwprintf(q, MAX_PATH + 4, L"\"%ls\"", g_exe_path);
        q[MAX_PATH + 3] = 0;
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)q,
                       (DWORD)((wcslen(q) + 1) * sizeof(WCHAR)));
        log_line("autostart: enabled");
    } else {
        RegDeleteValueW(k, RUN_VALUE);
        log_line("autostart: disabled");
    }
    RegCloseKey(k);
}

/* --------------------------------------------------------------- 通知 */

/* 发一条系统通知，语义与 Python 版 _show_toast 一致：
 *
 *   - 走 Shell_NotifyIcon(NIM_MODIFY) + NIF_INFO，Win10/11 渲染成原生横幅并带提示音，
 *     不需要 AppUserModelID，也不需要第三方库。
 *   - uVersion 必须是 0。设成 4（NOTIFYICON_VERSION_4）后回调参数语义会变——
 *     wParam 带坐标、lParam 低字是 NIN_*——WM_TRAY_MSG 里 `LOWORD(lp)` 的解析
 *     全部错位，左键/右键/点外面收起会一起失效。
 *   - uFlags 只写 NIF_INFO。加上 NIF_ICON / NIF_TIP 会顺手覆盖托盘图标和悬停文案。
 *   - 失败时补一次图标再重发：explorer 重启过 / 图标刚被回收时第一次必然失败。
 */
static BOOL toast(const WCHAR *title, const WCHAR *info)
{
    NOTIFYICONDATAW n = g_nid;
    BOOL            ok;

    n.uFlags      = NIF_INFO;
    n.dwInfoFlags = NIIF_WARNING;      /* Python 版不区分等级，一律警告色 */
    n.uVersion    = 0;
    wcsncpy(n.szInfoTitle, title, 63);
    n.szInfoTitle[63] = 0;
    wcsncpy(n.szInfo, info, 255);
    n.szInfo[255] = 0;

    ok = Shell_NotifyIconW(NIM_MODIFY, &n);
    if (!ok) {
        NOTIFYICONDATAW fix = g_nid;

        fix.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &fix);
        ok = Shell_NotifyIconW(NIM_MODIFY, &n);
    }

#ifdef LOWTEST
    /* 返回值是「外壳是否受理这条气泡」的唯一客观证据：TRUE=已投递给通知中心。
     * 屏幕被全屏程序盖住时截屏看不出气泡（系统本来也会抑制），只能看这个值。 */
    log_linew(L"toast ok=%d flags=%lu title=%ls info=%ls err=%lu", (int)ok,
              (unsigned long)n.dwInfoFlags, n.szInfoTitle, n.szInfo,
              (unsigned long)GetLastError());
#endif
    if (!ok)
        log_linew(L"系统通知发送失败 err=%lu", (unsigned long)GetLastError());
    return ok;
}

/* 低电量提醒：多台设备合并成一条（托盘一次只能挂一条气泡，弹两条会互相顶掉）。
 * 文案照搬 Python 版 _toast_text：最危险的排前面，最多列两台，其余用「等 N 台设备」
 * 带过；szInfoTitle 63 / szInfo 255 必须按 NUL 截断。 */
static void notify_low(const WCHAR *const *names, const int *pcts, int cnt)
{
    WCHAR head[224], info[256];
    int   idx[DM_MAX_DEV], len = 0, i, j;

    if (cnt <= 0)
        return;
    if (cnt > DM_MAX_DEV)
        cnt = DM_MAX_DEV;

    for (i = 0; i < cnt; i++)
        idx[i] = i;
    for (i = 1; i < cnt; i++) {                 /* 按电量升序，插入排序够用 */
        int key = idx[i], v = pcts[key];

        for (j = i - 1; j >= 0 && pcts[idx[j]] > v; j--)
            idx[j + 1] = idx[j];
        idx[j + 1] = key;
    }

    head[0] = 0;
    for (i = 0; i < cnt && i < 2; i++) {
        int w = _snwprintf(head + len, 224 - len, L"%ls%ls 剩余 %d%%",
                           len ? L"、" : L"", names[idx[i]], pcts[idx[i]]);

        if (w < 0)
            break;
        len += (w < 224 - len) ? w : (224 - len - 1);
    }
    if (cnt > 2 && len < 200) {
        int w = _snwprintf(head + len, 224 - len, L"，等 %d 台设备", cnt);

        if (w > 0)
            len += (w < 224 - len) ? w : (224 - len - 1);
    }
    _snwprintf(info, 256, L"%ls，建议尽快充电（点击查看详情）", head);
    info[255] = 0;

    toast(L"设备电量不足", info);
}

/* --------------------------------------------------------------- 视图刷新 */

static void refresh_view(void)
{
    DmState st[DM_MAX_DEV];
    int     n, i, min_pct = -1, min_chg = 0, any = 0;
    WCHAR   tip[128];

    n = dm_copy(st, DM_MAX_DEV);
    tip[0] = 0;

#ifdef LOWTEST
    /* 诊断构建：只把鼠标伪装成在线 + 指定电量（鼠标真机 43%，没法等它掉到 20 以下）；
     * 没有鼠标时退回第一台，保证一定能触发判定路径。
     * 只伪装一台，这样「每台设备各报一次」的去重计数才好核对。 */
    fake_batt_load();
    if (g_fake_batt >= 0 && n > 0) {
        int t = -1;

        for (i = 0; i < n; i++)
            if (st[i].kind == DM_KIND_MOUSE) {
                t = i;
                break;
            }
        if (t < 0)
            t = 0;
        for (i = 0; i < n; i++) {
            if (i == t)
                continue;
            if (g_fake_batt2 < 0)
                continue;               /* 只想伪装一台时，其余保持真实数据 */
            st[i].present = 1;
            st[i].online  = DM_ONLINE;
            st[i].battery = g_fake_batt2;
        }
        st[t].present  = 1;
        st[t].online   = DM_ONLINE;
        st[t].battery  = g_fake_batt;
        st[t].charging = 0;
    }
#endif

    for (i = 0; i < n; i++) {
        const WCHAR *nm = dev_display_name(i, &st[i]);
        WCHAR        line[96];
        int          used = (int)wcslen(tip);

        /* 悬停提示只给「设备名 + 电量」，和 Python 版同一格式：
         * "狼蛛 FN75 78%  ·  迈从 G3 --"
         * 具体多久前更新留在卡片里看，气泡里塞不下。 */
        if (st[i].battery >= 0)
            _snwprintf(line, 96, L"%ls %d%%", nm, st[i].battery);
        else
            _snwprintf(line, 96, L"%ls --", nm);
        line[95] = 0;

        if (used + (int)wcslen(line) + 6 < 128) {
            if (used)
                wcscat(tip, L"  ·  ");
            wcscat(tip, line);
        }

        if (st[i].present && st[i].online != DM_OFFLINE && st[i].battery >= 0) {
            any = 1;
            if (min_pct < 0 || st[i].battery < min_pct) {
                min_pct = st[i].battery;
                min_chg = st[i].charging == 1;
            }
        }
    }

    /* 低电量提醒：跌破阈值报一次，回到恢复线以上才重新武装。
     * 先收集再统一发，多台同时跌破就合成一条气泡（Python 版同样处理）。 */
    {
        const WCHAR *low_names[DM_MAX_DEV];
        int          low_pcts[DM_MAX_DEV];
        int          low_cnt = 0;

        for (i = 0; i < n; i++) {
            if (!g_cfg.low_notify || !st[i].present || st[i].online != DM_ONLINE
                || st[i].battery < 0)
                continue;
            if (st[i].battery <= g_cfg.low_pct && !g_alerted[i]) {
                g_alerted[i] = 1;
                if (low_cnt < DM_MAX_DEV) {
                    low_names[low_cnt] = dev_display_name(i, &st[i]);
                    low_pcts[low_cnt]  = st[i].battery;
                    low_cnt++;
                }
            } else if (st[i].battery >= g_cfg.recover_pct && g_alerted[i]) {
                g_alerted[i] = 0;               /* 回到恢复线以上，重新武装 */
            }
        }
        if (low_cnt) {
            for (i = 0; i < low_cnt; i++)
                log_linew(L"低电量提醒: %ls %d%%", low_names[i], low_pcts[i]);
            notify_low(low_names, low_pcts, low_cnt);
        }
    }

    if (!any && n == 0)
        wcscpy(tip, L"未检测到支持的 2.4G 设备");

    /* 托盘提示 */
    if (wcscmp(g_nid.szTip, tip) != 0) {
        wcsncpy(g_nid.szTip, tip, 127);
        g_nid.szTip[127] = 0;
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }

    /* 图标：取所有在线设备里最低的那块电量，按 5% 一档缓存
     * （Python 版也是 bucket 到 5 的倍数，避免每秒重建 HICON） */
    {
        int bucket = min_pct < 0 ? -1 : (min_pct + 2) / 5 * 5;
        int key    = bucket * 2 + (min_chg ? 1 : 0);

        if (key != g_icon_key) {
            HICON ni = render_icon(min_pct, min_chg);
            if (ni) {
                HICON old = g_icon;
                g_icon = ni;
                g_nid.hIcon = g_icon;
                g_nid.uFlags = NIF_ICON;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                if (old)
                    DestroyIcon(old);
            }
            g_icon_key = key;
        }
    }

    if (g_card_shown)
        InvalidateRect(g_card, NULL, TRUE);
}

/* --------------------------------------------------------------- 菜单 */

static void show_menu(void)
{
    HMENU  m = CreatePopupMenu();
    POINT  pt;
    int    cmd;
    int    auto_on = autostart_state();

    /* 菜单文案与顺序照搬 Python 版 */
    AppendMenuW(m, MF_STRING | (g_cfg.autostart && auto_on ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"开机自动启动");
    {
        WCHAR low[64];

        _snwprintf(low, 64, L"低电量提醒（低于 %d%%）", g_cfg.low_pct);
        low[63] = 0;
        AppendMenuW(m, MF_STRING | (g_cfg.low_notify ? MF_CHECKED : 0),
                    IDM_NOTIFY, low);
    }
    AppendMenuW(m, MF_STRING, IDM_LOG, L"查看日志");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);          /* 必须先抢前台，否则菜单关不掉 */
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, g_wnd, NULL);
    DestroyMenu(m);
    PostMessageW(g_wnd, WM_NULL, 0, 0);

    switch (cmd) {
    case IDM_AUTOSTART:
        g_cfg.autostart = g_cfg.autostart ? 0 : 1;
        autostart_apply(g_cfg.autostart);
        cfg_save(&g_cfg, g_ini);
        break;
    case IDM_NOTIFY:
        g_cfg.low_notify = g_cfg.low_notify ? 0 : 1;
        cfg_save(&g_cfg, g_ini);
        log_linew(L"低电量提醒 → %ls", g_cfg.low_notify ? L"开" : L"关");
        if (g_cfg.low_notify) {
            /* 顺手当「通知通道通不通」的自检：省得等到真掉到阈值才发现弹不出来 */
            WCHAR s[128];

            _snwprintf(s, 128, L"电量低于 %d%% 时会提醒你一次", g_cfg.low_pct);
            s[127] = 0;
            toast(L"低电量提醒已开启", s);
        }
        break;
    case IDM_LOG: {
        WCHAR args[MAX_PATH + 32];
        _snwprintf(args, MAX_PATH + 32, L"/select,\"%ls\"", g_cfg.log_path);
        args[MAX_PATH + 31] = 0;
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        break;
    }
    case IDM_EXIT:
        g_quitting = 1;
        DestroyWindow(g_wnd);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------- 主窗口 */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY_MSG:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            /* 刚才那一下点击已经因为「点在卡片外」把卡片收起来了，
             * 说明这次点托盘只是为了关掉它，别再弹回来。
             * 时间上「按下 → 轮询收起 → 松开 → 收到托盘消息」完全可能发生。 */
            if (g_click_dismiss_tick
                && GetTickCount() - g_click_dismiss_tick < 400) {
                g_click_dismiss_tick = 0;
                return 0;
            }
            if (g_card_shown)
                card_hide();
            else
                card_show();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            if (g_card_shown)
                card_hide();
            show_menu();
        }
        return 0;

    case WM_DM_CHANGED:
        g_last_active = GetTickCount64();
        refresh_view();
        return 0;

    case WM_HIDE_CARD:
        /* 钩子只负责投这条消息，真正的收起来主线程做 */
        if (g_card_shown) {
            g_click_dismiss_tick = GetTickCount();
            card_hide();
        }
        return 0;

    case WM_TIMER: {
        ULONGLONG now;

        if (wp == TIMER_SELFTEST) {
            /* 自检：直接把卡片弹在屏幕中间，方便截图看版式 */
            KillTimer(hwnd, TIMER_SELFTEST);
            SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2,
                         GetSystemMetrics(SM_CYSCREEN) / 2);
            card_show();
            return 0;
        }
        if (wp == TIMER_DISMISS) {
            poll_dismiss();
            return 0;
        }
        if (wp != TIMER_TICK)
            break;                       /* 别把别的定时器当成自己的 */
        now = GetTickCount64();
        refresh_view();

        if (g_cfg.idle_trim_s > 0
            && now - g_last_active > (ULONGLONG)g_cfg.idle_trim_s * 1000
            && now - g_trim_at > 60000) {
            g_trim_at = now;
            if (SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1))
                log_line("idle %llus, working set returned",
                         (now - g_last_active) / 1000);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        KillTimer(hwnd, TIMER_DISMISS);
        if (g_mouse_hook) {
            UnhookWindowsHookEx(g_mouse_hook);
            g_mouse_hook = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG         msg;
    HANDLE      mutex;

    (void)prev;
    (void)cmd;
    (void)show;
    g_inst = inst;

    /* 必须最先做：不做 DPI 感知的话系统会虚拟化所有坐标，
     * SetWindowPos 的位置和实际位置会差一个缩放比，文字也会被拉糊。 */
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
        PFN_SetCtx pfn = u32
            ? (PFN_SetCtx)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext")
            : NULL;
        if (!pfn || !pfn((HANDLE)-4))       /* -4 = PER_MONITOR_AWARE_V2 */
            SetProcessDPIAware();
    }
    g_dpi = (int)dpi_of(NULL);

    mutex = CreateMutexW(NULL, TRUE, L"2.4GBatteryTray.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;                        /* 已经在跑了 */
    }

    get_paths();
    cfg_defaults(&g_cfg, g_exe_dir);
    {
        BOOL existed = cfg_load(&g_cfg, g_ini);
        if (!existed)
            cfg_save(&g_cfg, g_ini);     /* 首次运行落一份带注释的配置 */
    }
    if (!g_cfg.log_path[0])
        _snwprintf(g_cfg.log_path, MAX_PATH, L"%ls\\tray.log", g_exe_dir);
    log_open(g_cfg.log_path);
    log_line("---- start ----");

    /* 以 ini 为准对齐注册表；开启时还要核对路径 —— 绿色版被搬到别的目录后，
     * 必须把自启指向现在这个 exe，否则开机启动的仍是旧位置那一份。 */
    if (g_cfg.autostart) {
        if (!autostart_points_to_self()) {
            autostart_apply(1);
            log_linew(L"autostart: path synced to %ls", g_exe_path);
        }
    } else if (autostart_state()) {
        autostart_apply(0);
    }

    /* 字号照搬 Python 版 _create_resources 的 specs（像素高，不是磅） */
    g_f_title = make_font_px(1300, FW_SEMIBOLD);
    g_f_name  = make_font_px(1300, FW_SEMIBOLD);
    g_f_pct   = make_font_px(2600, FW_BOLD);
    g_f_sign  = make_font_px(1300, FW_BOLD);
    g_f_unit  = make_font_px(1150, FW_NORMAL);
    g_f_meta  = make_font_px(1050, FW_NORMAL);

    /* 主窗口（隐藏，只为收消息） */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"BattTrayMain";
    RegisterClassExW(&wc);

    g_wnd = CreateWindowExW(0, L"BattTrayMain", L"2.4G 设备电量", 0,
                            0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_wnd) {
        log_line("main window create failed");
        return 1;
    }

    /* 卡片窗口 */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = card_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.style         = CS_DROPSHADOW;     /* 和 Python 版一样带投影 */
    wc.lpszClassName = L"BattTrayCard";
    RegisterClassExW(&wc);

    {
        int w, h;
        card_layout(&w, &h);
        g_card = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                 L"BattTrayCard", L"", WS_POPUP,
                                 0, 0, w, h, g_wnd, NULL, inst, NULL);
        if (g_card) {
            HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, DI(13), DI(13));
            SetWindowRgn(g_card, rgn, TRUE);
        }
    }

    /* 托盘图标 */
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_wnd;
    g_nid.uID              = IDI_TRAY;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_MSG;
    g_icon                 = render_icon(-1, 1);
    g_nid.hIcon            = g_icon;
    wcscpy(g_nid.szTip, L"2.4G 设备电量：正在读取");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_last_active = GetTickCount64();
    g_trim_at     = g_last_active;
    dm_start(g_wnd, WM_DM_CHANGED);
    SetTimer(g_wnd, TIMER_TICK, TICK_MS, NULL);
    if (cmd && wcsstr(cmd, L"--card"))
        SetTimer(g_wnd, TIMER_SELFTEST, 2500, NULL);   /* 自检用 */
    refresh_view();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    dm_stop();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_icon)
        DestroyIcon(g_icon);
    if (g_f_title) DeleteObject(g_f_title);
    if (g_f_name)  DeleteObject(g_f_name);
    if (g_f_pct)   DeleteObject(g_f_pct);
    if (g_f_sign)  DeleteObject(g_f_sign);
    if (g_f_unit)  DeleteObject(g_f_unit);
    if (g_f_meta)  DeleteObject(g_f_meta);
    art_free();
    log_line("---- stop ----");
    log_close();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
