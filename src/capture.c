/* capture.c — 分阶段抓帧实验（排查用，不是最终程序）
 *
 * 目的：搞清楚「设备关机」时接收器到底发什么，以此决定断连检测依据。
 *
 * 它会把实验步骤直接打印在控制台里，跟着做就行，全程约 2 分钟：
 *     STAGE 2  鼠标静止不动（确认空闲时接收器是否发保活帧）
 *     STAGE 3  关掉鼠标电源  ← 关键：此时接收器发不发 09 0A 02 之类的事件帧？
 *     STAGE 4  重新打开鼠标
 *     STAGE 6  关掉键盘电源  ← 关键：此时链路字节 data[7] 还等于 1 吗？
 *
 * 输出同时打到屏幕和同目录下的 capture.log（纯 ASCII，方便回传分析）。
 *
 * 构建见 build.bat，用法：
 *     capture.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "hid.h"

#define MAX_T 4

typedef struct {
    HidDev dev;
    HANDLE h;
    int    is_kb;
    int    is_ms;
    long   frames;        /* 本阶段收到帧数 */
    long   total;
    double last_rx;       /* 相对 t0 的秒数 */
} T;

static T      g_t[MAX_T];
static int    g_n;
static double g_t0;
static FILE  *g_log;

static double now_s(void) { return (double)GetTickCount64() / 1000.0; }
static double rel_s(void) { return now_s() - g_t0; }

static void logf_(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

static void hexs(const unsigned char *b, int n, char *out, int cap)
{
    int i, p = 0;
    out[0] = 0;
    for (i = 0; i < n && p < cap - 4; i++)
        p += snprintf(out + p, cap - p, "%02X ", b[i]);
    if (p > 0)
        out[p - 1] = 0;
}

/* ---------------------------------------------------------------- 实验表 */
typedef struct {
    double      secs;
    const char *title;
    const char *action;
} Stage;

static const Stage STAGES[] = {
    {  6, "S0 baseline          ", "Leave EVERYTHING alone. Do not touch mouse or keyboard." },
    { 12, "S1 mouse ACTIVE      ", "Keep MOVING the mouse continuously." },
    { 15, "S2 mouse IDLE        ", "Leave the mouse completely STILL. Still powered on." },
    { 26, "S3 mouse POWER OFF   ", ">>> TURN OFF the mouse switch NOW. Do not touch it again." },
    { 18, "S4 mouse POWER ON    ", ">>> TURN THE MOUSE BACK ON, then wiggle it." },
    { 12, "S5 keyboard ACTIVE   ", "Press some keys on the keyboard." },
    { 22, "S6 keyboard POWER OFF", ">>> TURN OFF the keyboard switch NOW." },
    { 16, "S7 keyboard POWER ON ", ">>> TURN THE KEYBOARD BACK ON, then press a key." },
};
#define N_STAGE ((int)(sizeof(STAGES) / sizeof(STAGES[0])))

#define QUERY_MS 1000.0

static void send_query(void)
{
    static const unsigned char q[20] = {
        0x13, 0x05, 0x01, 0, 0, 0, 0, 0, 0, 0,
        0,    0,    0,    0, 0, 0, 0, 0, 0, 0x19
    };
    int k;
    for (k = 0; k < g_n; k++) {
        if (!g_t[k].is_kb)
            continue;
        if (hid_write(g_t[k].h, q, sizeof(q)) < 0)
            logf_("      [w] keyboard query write FAILED (handle/pipe problem)\n");
    }
}

/* 跑一个阶段：持续读所有句柄，打印每一帧 */
static void run_stage(int idx)
{
    double t_end = rel_s() + STAGES[idx].secs;
    double next_query = rel_s();
    int    k;

    for (k = 0; k < g_n; k++)
        g_t[k].frames = 0;

    logf_("\n---- %s  (%.0fs) ----\n", STAGES[idx].title, STAGES[idx].secs);
    logf_("     %s\n", STAGES[idx].action);

    while (rel_s() < t_end) {
        if (rel_s() >= next_query) {
            next_query = rel_s() + QUERY_MS / 1000.0;
            send_query();
        }
        for (k = 0; k < g_n; k++) {
            unsigned char buf[64];
            char          hx[256];
            int           rc = hid_read_timeout(g_t[k].h, buf, sizeof(buf), 40);
            const char   *tag = g_t[k].is_kb ? "KB" : "MS";

            if (rc <= 0)
                continue;

            g_t[k].last_rx = rel_s();
            g_t[k].frames++;
            g_t[k].total++;
            hexs(buf, rc > 16 ? 16 : rc, hx, sizeof(hx));

            if (buf[0] == 0x13 && rc >= 8 && buf[1] == 0x0A)
                logf_("  %7.2fs %s  %-47s | batt=%u%% link=%u %s\n",
                      g_t[k].last_rx, tag, hx, buf[6], buf[7],
                      buf[7] == 1 ? "ONLINE" : "**OFFLINE**");
            else if (buf[0] == 0x09 && rc >= 4 && buf[1] == 0x0A)
                logf_("  %7.2fs %s  %-47s | %s\n", g_t[k].last_rx, tag, hx,
                      buf[2] == 0x05 ? "BATTERY frame" : "STATUS frame");
            else if (buf[0] == 0x13 && rc >= 2 && buf[1] == 0x05)
                logf_("  %7.2fs %s  %-47s | echo of our query\n", g_t[k].last_rx, tag, hx);
            else
                logf_("  %7.2fs %s  %-47s | unknown (%d bytes)\n", g_t[k].last_rx, tag, hx, rc);
        }
        Sleep(10);
    }

    for (k = 0; k < g_n; k++)
        logf_("    == %s frames this stage: %ld\n",
              g_t[k].is_kb ? "KB" : "MS", g_t[k].frames);
}

int wmain(void)
{
    HidDev devs[HID_MAX_DEVICES];
    int    n, i, s;

    g_t0  = now_s();
    g_log = fopen("capture.log", "w");

    logf_("================ 2.4G power-off capture ================\n");
    logf_("Follow the prompts. Do not close this window.\n");
    logf_("Total ~2 minutes. Dongle must stay plugged in.\n");

    n = hid_enum(devs, HID_MAX_DEVICES);
    for (i = 0; i < n && g_n < MAX_T; i++) {
        HidDev *d = &devs[i];
        int kb = (d->vid == 0x3554 && d->pid == 0xFA09);
        int ms = (d->vid == 0x258A && d->pid == 0x0138);
        if (!kb && !ms)
            continue;
        if (d->usage_page != 0xFF02)
            continue;
        g_t[g_n].dev    = *d;
        g_t[g_n].is_kb  = kb;
        g_t[g_n].is_ms  = ms;
        g_t[g_n].h      = hid_open(d->path, kb);
        g_t[g_n].last_rx = 0;
        if (g_t[g_n].h == INVALID_HANDLE_VALUE) {
            logf_("[!] open FAILED: %ls\n", d->path);
            continue;
        }
        logf_("[+] opened %s %04X:%04X on page 0xFF02\n",
              kb ? "KEYBOARD" : "MOUSE", d->vid, d->pid);
        g_n++;
    }

    if (!g_n) {
        logf_("\nNo target found. Is the dongle plugged in?\n");
        if (g_log) fclose(g_log);
        return 1;
    }

    for (s = 0; s < N_STAGE; s++)
        run_stage(s);

    logf_("\n================ SUMMARY ================\n");
    for (i = 0; i < g_n; i++)
        logf_("  %-8s total frames %5ld, last frame at %7.2fs (%s)\n",
              g_t[i].is_kb ? "KEYBOARD" : "MOUSE", g_t[i].total, g_t[i].last_rx,
              g_t[i].total ? "seen" : "NEVER");

    logf_("\nLog saved to capture.log\n");
    if (g_log) fclose(g_log);

    for (i = 0; i < g_n; i++)
        if (g_t[i].h != INVALID_HANDLE_VALUE)
            CloseHandle(g_t[i].h);

    printf("\nPress ENTER to close...");
    (void)getchar();
    return 0;
}
