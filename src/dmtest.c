/* dmtest.c — 监控层控制台测试台（开发用）
 *
 * 不需要托盘 UI，直接看 devmon 的实时判定结果。用来验证：
 *   - 电量是否读对
 *   - 关掉设备后 online 是否变 0、note 是否给出原因
 *
 * 用法：dmtest.exe [秒数]     默认 30 秒
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "devmon.h"
#include "log.h"

static const char *online_txt(int v)
{
    return v == DM_ONLINE ? "ONLINE " : v == DM_OFFLINE ? "OFFLINE" : "unknown";
}

int wmain(int argc, WCHAR **argv)
{
    DmState st[DM_MAX_DEV];
    double  end_s;
    int     secs = 30, last_n = -1;

    if (argc > 1)
        secs = _wtoi(argv[1]);
    if (secs <= 0)
        secs = 30;

    if (!log_open(L"frames.log"))
        printf("[warn] cannot open frames.log\n");

    if (!dm_start(NULL, 0)) {
        printf("[error] dm_start failed\n");
        return 1;
    }

    end_s = (double)GetTickCount64() / 1000.0 + secs;
    printf("monitoring for %d s ...\n\n", secs);

    while ((double)GetTickCount64() / 1000.0 < end_s) {
        int n = dm_copy(st, DM_MAX_DEV), i;

        if (n != last_n) {
            printf("devices: %d\n", n);
            last_n = n;
        }
        for (i = 0; i < n; i++)
            printf("  %-6ls batt=%3d%% %s present=%d fresh=%d  %ls\n",
                   st[i].key, st[i].battery, online_txt(st[i].online),
                   st[i].present, st[i].fresh,
                   st[i].note[0] ? st[i].note : L"");
        printf("\n");
        Sleep(1000);
    }

    dm_stop();
    printf("done. see frames.log\n");
    return 0;
}
