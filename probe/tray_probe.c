/* 轻量级可行性探针：最小可用的 Win32 托盘程序
 *
 * 目的只有一个 —— 量出「纯 C + Win32」在这个项目上的真实下限：
 *   exe 体积、私有内存、工作集、GDI/USER 句柄、线程数。
 * 不做 HID、不做自绘，只验证运行时底座有多轻。
 *
 * 构建（w64devkit）：
 *   gcc -O2 -s -mwindows -static -o tray_probe.exe tray_probe.c -lshell32
 */
#define WIN32_LEAN_AND_MEAN
/* 不加这两个宏，IDI_APPLICATION 会展开成 MAKEINTRESOURCEA（CHAR*），
   传给 LoadIconW 会报 incompatible pointer type。
   -municode 也会定义它们，所以要先判断，否则编译会出 redefined 警告 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>

#define WM_TRAY (WM_APP + 1)
#define IDM_EXIT 1001

static NOTIFYICONDATAW g_nid;
static int g_ticks = 0;

static void tray_add(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"\u7535\u91cf\u63a2\u9488 100%", 127);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_balloon(HWND hwnd, LPCWSTR title, LPCWSTR text)
{
    NOTIFYICONDATAW n;
    ZeroMemory(&n, sizeof(n));
    n.cbSize = sizeof(n);
    n.hWnd = hwnd;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_USER;
    lstrcpynW(n.szInfoTitle, title, 63);   /* 63 / 255：填满就没有 NUL 结尾 */
    lstrcpynW(n.szInfo, text, 255);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"\u9000\u51fa");
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                           pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
        } else if (LOWORD(lp) == WM_LBUTTONUP) {
            tray_balloon(hwnd, L"\u7535\u91cf\u63a2\u9488",
                         L"\u9f20\u6807 45%  \u00b7  \u952e\u76d8 100%");
        }
        return 0;

    case WM_TIMER:
        /* 800ms 空转，模拟真实程序的定时器负载 */
        if (++g_ticks % 100 == 0)
            tray_balloon(hwnd, L"\u5fc3\u8df3", L"\u4ecd\u5728\u8fd0\u884c");
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_EXIT) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;

    (void)prev; (void)cmd; (void)show;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"TrayProbeHost";
    RegisterClassExW(&wc);

    /* 隐藏的 message-only 窗口，托盘只需要一个收消息的 HWND */
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, inst, NULL);
    if (!hwnd)
        return 1;

    tray_add(hwnd);
    SetTimer(hwnd, 1, 800, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
