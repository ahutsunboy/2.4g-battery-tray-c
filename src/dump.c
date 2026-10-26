/* dump.c — 原始帧记录器（排查/取证用，不是最终程序）
 *
 * 用途一：验证 C 版 HID 层能否正确读到两台设备的电量。
 * 用途二：**抓「设备关机」时接收器到底发什么** —— 这是判断掉线的唯一依据。
 *         Python 版无法识别断电，就是因为它只认电量帧，把其它帧一律丢弃。
 *
 * 构建见 build.bat，用法：
 *     dump.exe [秒数]        默认 60 秒
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include "hid.h"

#define MAX_TARGET 8
#define QUERY_MS   2000      /* 键盘查询间隔 */
#define SILENCE_S  5.0       /* 多久没帧算「静默」，打印一次 */

typedef struct {
    HidDev dev;
    HANDLE h;
    int    is_kb;            /* Compx 键盘：需要主动发查询 */
    int    is_ms;            /* SinoWealth 鼠标：只能被动收 */
    double last_rx;          /* 最近一次收到帧的时刻 */
    int    silence_reported;
    unsigned long frames;
} Target;

static Target g_t[MAX_TARGET];
static int    g_nt;

static double now_s(void)
{
    return (double)GetTickCount64() / 1000.0;
}

static void hex(const unsigned char *b, int n, wchar_t *out, int cap)
{
    int i, pos = 0;
    out[0] = 0;
    for (i = 0; i < n && pos < cap - 4; i++)
        pos += swprintf(out + pos, cap - pos, L"%02X ", b[i]);
    if (pos > 0)
        out[pos - 1] = 0;    /* 去掉末尾空格 */
}

int wmain(int argc, wchar_t **argv)
{
    HidDev devs[HID_MAX_DEVICES];
    int len = 60, n, i;
    double t0, last_query = -1000.0;

    if (argc > 1)
        len = _wtoi(argv[1]);
    if (len <= 0)
        len = 60;

    n = hid_enum(devs, HID_MAX_DEVICES);
    wprintf(L"=== 枚举到 %d 个 HID 接口，其中厂商自定义页(>=0xFF00) 或已知 VID 的：\n", n);
    for (i = 0; i < n; i++) {
        HidDev *d = &devs[i];
        if (d->usage_page < 0xFF00 && d->vid != 0x258A && d->vid != 0x3554)
            continue;
        wprintf(L"  %04X:%04X v%04X  page=0x%04X usage=0x%04X  in=%u out=%u feat=%u  \"%ls\"\n",
                d->vid, d->pid, d->version, d->usage_page, d->usage,
                d->in_len, d->out_len, d->feat_len, d->product);
    }

    /* 挑选要抓的目标：只认 0xFF02 通道 */
    for (i = 0; i < n && g_nt < MAX_TARGET; i++) {
        HidDev *d = &devs[i];
        int kb = (d->vid == 0x3554 && d->pid == 0xFA09);
        int ms = (d->vid == 0x258A && d->pid == 0x0138);
        if (!kb && !ms)
            continue;
        if (d->usage_page != 0xFF02)
            continue;
        g_t[g_nt].dev = *d;
        g_t[g_nt].is_kb = kb;
        g_t[g_nt].is_ms = ms;
        g_t[g_nt].h = hid_open(d->path, kb);
        g_t[g_nt].last_rx = 0;
        g_t[g_nt].frames = 0;
        if (g_t[g_nt].h == INVALID_HANDLE_VALUE) {
            wprintf(L"  [!] 打开失败: %ls\n", d->path);
            continue;
        }
        wprintf(L"  [+] 已打开 %ls (%04X:%04X page 0xFF02)\n",
                kb ? L"键盘" : L"鼠标", d->vid, d->pid);
        g_nt++;
    }

    if (!g_nt) {
        wprintf(L"\n没找到可抓的 0xFF02 通道。请确认接收器已插好。\n");
        return 1;
    }

    wprintf(L"\n=== 开始捕获 %d 秒（每 2 秒给键盘发一次查询）===\n", len);
    wprintf(L"提示：捕获期间把鼠标/键盘关机，观察接收器还会不会发帧、发什么。\n\n");

    t0 = now_s();
    for (;;) {
        double el = now_s() - t0;
        int k;

        if (el >= len)
            break;

        if (el - last_query >= (double)QUERY_MS / 1000.0) {
            last_query = el;
            for (k = 0; k < g_nt; k++) {
                if (!g_t[k].is_kb)
                    continue;
                static const unsigned char q[20] = {
                    0x13, 0x05, 0x01, 0, 0, 0, 0, 0, 0, 0,
                    0,    0,    0,    0, 0, 0, 0, 0, 0, 0x19
                };
                if (hid_write(g_t[k].h, q, sizeof(q)) < 0)
                    wprintf(L"%7.2fs  KB  查询写入失败\n", el);
            }
        }

        for (k = 0; k < g_nt; k++) {
            unsigned char buf[64];
            wchar_t hx[256];
            int rc = hid_read_timeout(g_t[k].h, buf, sizeof(buf), 60);
            const wchar_t *tag = g_t[k].is_kb ? L"KB" : L"MS";

            if (rc > 0) {
                g_t[k].last_rx = el;
                g_t[k].silence_reported = 0;
                g_t[k].frames++;
                hex(buf, rc > 16 ? 16 : rc, hx, 256);

                if (buf[0] == 0x13 && rc >= 8 && buf[1] == 0x0A) {
                    wprintf(L"%7.2fs  %ls  %-47ls -> 电量 %u%%  链路 %ls\n",
                            el, tag, hx, buf[6],
                            buf[7] == 1 ? L"在线" : L"**离线**");
                } else if (buf[0] == 0x09 && rc >= 4 && buf[1] == 0x0A) {
                    if (buf[2] == 0x05)
                        wprintf(L"%7.2fs  %ls  %-47ls -> 电量 %u%%\n", el, tag, hx, buf[3]);
                    else
                        wprintf(L"%7.2fs  %ls  %-47ls -> 非电量状态帧 sub=0x%02X\n",
                                el, tag, hx, buf[2]);
                } else {
                    wprintf(L"%7.2fs  %ls  %-47ls -> 未知帧（%d 字节）\n", el, tag, hx, rc);
                }
            } else if (!g_t[k].silence_reported
                       && el - g_t[k].last_rx > SILENCE_S) {
                g_t[k].silence_reported = 1;
                wprintf(L"%7.2fs  %ls  （已静默 %.0f 秒，一个字节都没收到）\n",
                        el, tag, el - g_t[k].last_rx);
            }
        }
        Sleep(20);
    }

    wprintf(L"\n=== 统计 ===\n");
    for (i = 0; i < g_nt; i++)
        wprintf(L"  %ls: 收到 %lu 帧，最后一帧在 %7.2fs\n",
                g_t[i].is_kb ? L"键盘" : L"鼠标", g_t[i].frames, g_t[i].last_rx);

    for (i = 0; i < g_nt; i++)
        if (g_t[i].h != INVALID_HANDLE_VALUE)
            CloseHandle(g_t[i].h);
    return 0;
}
