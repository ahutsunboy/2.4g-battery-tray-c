/* feat2.c — 特性报告探测：报告 ID × 长度 暴力枚举（排查用）
 *
 * HidD_GetFeature 对所有长度都返回 87（参数错误）时，常见原因是
 * 缓冲区首字节必须是「已存在的特性报告 ID」，填 0 无效。
 * 这里把 rid 0..31 × 若干长度全试一遍，只报成功和非 87 的失败。
 *
 * 只有 GET，没有任何 SET。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hidsdi.h>

#include "hid.h"

static void hexdump(const unsigned char *b, int n)
{
    int i;
    printf("        data:");
    for (i = 0; i < n && i < 32; i++)
        printf(" %02X", b[i]);
    printf("%s\n", n > 32 ? " ..." : "");
}

int wmain(void)
{
    HidDev        devs[HID_MAX_DEVICES];
    int           n = hid_enum(devs, HID_MAX_DEVICES);
    int           i, rid, hits = 0;
    unsigned char buf[1100];

    printf("=== feat2: report-id x length brute force ===\n");

    for (i = 0; i < n; i++) {
        HidDev *d = &devs[i];
        HANDLE  h;
        int     li;

        if ((d->vid != 0x258A && d->vid != 0x3554) || d->feat_len == 0)
            continue;

        h = CreateFileW(d->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        printf("\n  %04X:%04X page=0x%04X feat_len=%u\n",
               d->vid, d->pid, d->usage_page, d->feat_len);

        for (rid = 0; rid < 32; rid++) {
            for (li = 0; li < 64; li++) {
                int len = li < 60 ? li + 2 : (int)d->feat_len;   /* 2..61 + caps 长度 */
                DWORD err;

                if (len > (int)sizeof(buf))
                    continue;
                ZeroMemory(buf, sizeof(buf));
                buf[0] = (unsigned char)rid;
                SetLastError(0);
                if (HidD_GetFeature(h, buf, (ULONG)len)) {
                    printf("      OK  rid=%d len=%d\n", rid, len);
                    hexdump(buf, len);
                    hits++;
                    li = 99;             /* 这个 rid 已经成功，换下一个 rid */
                } else {
                    err = GetLastError();
                    if (err != 87)
                        printf("      rid=%d len=%d err=%lu\n", rid, len, err);
                }
            }
        }
        CloseHandle(h);
    }

    printf("\n=== %s (%d hits) ===\n", hits ? "SOME REPORT WORKS" : "no feature report readable", hits);
    printf("Press ENTER to close...");
    (void)getchar();
    return 0;
}
