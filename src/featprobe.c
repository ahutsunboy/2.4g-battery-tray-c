/* featprobe.c — 最后一轮：关机时这些可读状态块会不会变（排查用）
 *
 * 前面的扫描（feat2.c）确认：整台机器上只有 3 个特性报告读得到，
 * 鼠标的数据通道 0xFF02 一个都没有 —— 没法像键盘那样主动发查询。
 *
 * 这里把能读的那几个逐轮抓一遍，中间让你关一次鼠标、开一次鼠标，
 * 只要有一个字节跟着变，鼠标「关机检测」就还有救。
 *
 * 只读，不写。
 * 用法：双击 feat.bat，或 featprobe.exe，按提示操作。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hidsdi.h>

#include "hid.h"

#define N_REPORT 8

typedef struct {
    USHORT vid, pid, page;
    BYTE   rid;
    int    len;
    const char *what;
} Target;

static const Target TARGETS[] = {
    { 0x258A, 0x0138, 0xFF01, 10, 8, "mouse 0xFF01 rid=10 (status?)" },
    { 0x258A, 0x0138, 0xFF00,  5, 8, "mouse 0xFF00 rid=5  (string?)" },
    { 0x3554, 0xFA09, 0xFF04,  6, 8, "kbd   0xFF04 rid=6  (status?)" }
};
#define N_TARGET ((int)(sizeof(TARGETS) / sizeof(TARGETS[0])))

static BYTE  g_snap[4][N_REPORT][16];
static int   g_ok[4][N_REPORT];
static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    char    buf[600];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
}

/* 找接口 → 读报告。找不到就记 fail。 */
static void read_one(int round, int idx)
{
    const Target *t = &TARGETS[idx];
    HidDev        devs[HID_MAX_DEVICES];
    int           n = hid_enum(devs, HID_MAX_DEVICES);
    int           i;
    HANDLE        h = INVALID_HANDLE_VALUE;
    BYTE          buf[16];
    int           j;

    g_ok[round - 1][idx] = 0;

    for (i = 0; i < n; i++) {
        if (devs[i].vid != t->vid || devs[i].pid != t->pid)
            continue;
        if (devs[i].usage_page != t->page)
            continue;
        h = CreateFileW(devs[i].path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        logf_("    %-34s : <cannot open>\n", t->what);
        return;
    }

    ZeroMemory(buf, sizeof(buf));
    buf[0] = t->rid;
    if (HidD_GetFeature(h, buf, (ULONG)t->len)) {
        g_ok[round - 1][idx] = 1;
        memcpy(g_snap[round - 1][idx], buf, t->len);
        logf_("    %-34s :", t->what);
        for (j = 0; j < t->len; j++)
            logf_(" %02X", buf[j]);
        logf_("\n");
    } else {
        logf_("    %-34s : <read failed err=%lu>\n", t->what, GetLastError());
    }
    CloseHandle(h);
}

static void read_round(int round)
{
    int i;
    logf_("\n-------- ROUND %d --------\n", round);
    for (i = 0; i < N_TARGET; i++)
        read_one(round, i);
}

static void diff_round(int a, int b)
{
    int i, j, total = 0;

    logf_("\n  diff ROUND %d -> ROUND %d:\n", a, b);
    for (i = 0; i < N_TARGET; i++) {
        int d = 0;

        if (!g_ok[a - 1][i] || !g_ok[b - 1][i]) {
            logf_("    %-34s : <not comparable>\n", TARGETS[i].what);
            continue;
        }
        for (j = 0; j < TARGETS[i].len; j++) {
            if (g_snap[a - 1][i][j] != g_snap[b - 1][i][j]) {
                logf_("    * %-32s [%d] %02X -> %02X\n", TARGETS[i].what, j,
                      g_snap[a - 1][i][j], g_snap[b - 1][i][j]);
                d++;
            }
        }
        if (!d)
            logf_("    = %-34s unchanged\n", TARGETS[i].what);
        total += d;
    }
    logf_("    total changed bytes: %d %s\n", total,
          total ? ">>> a link flag exists!" : "(no usable flag)");
}

static void pause_wait(const char *msg)
{
    char line[16];
    logf_("\n>>> %s\n    then press ENTER...", msg);
    fflush(stdout);
    fflush(stdin);
    if (!fgets(line, sizeof(line), stdin))
        logf_("\n    (stdin closed, continuing)\n");
}

int wmain(void)
{
    g_log = fopen("feat.log", "w");
    logf_("=== power-state comparison of readable feature reports ===\n");
    logf_("(read-only; nothing is written to the devices)\n");

    read_round(1);
    pause_wait("TURN THE MOUSE OFF now (switch at the bottom).");
    read_round(2);
    pause_wait("TURN THE MOUSE BACK ON now.");
    read_round(3);

    diff_round(1, 2);
    diff_round(2, 3);

    logf_("\n=== done. see feat.log ===\n");
    logf_("Press ENTER to close...");
    (void)getchar();
    if (g_log) fclose(g_log);
    return 0;
}
