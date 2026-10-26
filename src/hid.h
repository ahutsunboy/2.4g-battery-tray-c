/* hid.h — HID 枚举 + 带超时的异步读写（Zero-dependency，仅 setupapi/hid）
 *
 * 两种接口：
 *   1. HidIo：常驻事件对象，适合长跑轮询（托盘程序用这个）
 *   2. hid_read_timeout / hid_write：一次性调用，适合小工具（dump/capture）
 *
 * 返回值约定（read）：
 *   > 0  读到这么多字节
 *   = 0  超时，没有数据（正常，不是错误）
 *   < 0  句柄失效 / 设备被拔掉（需要重新枚举）
 */
#ifndef HID_H
#define HID_H

#include <windows.h>

#define HID_MAX_DEVICES  128
#define HID_PATH_CHARS   512
#define HID_NAME_CHARS   128

typedef struct {
    WCHAR    path[HID_PATH_CHARS];
    USHORT   vid, pid, version;
    USHORT   usage_page, usage;
    USHORT   in_len, out_len, feat_len;   /* 已含 Report ID 字节 */
    WCHAR    product[HID_NAME_CHARS];
} HidDev;

/* 常驻 IO 上下文：handle + 复用的事件对象 */
typedef struct {
    HANDLE h;
    HANDLE ev;
} HidIo;

int  hid_enum(HidDev *out, int max);
HANDLE hid_open(const WCHAR *path, int want_write);

int  hid_io_init(HidIo *io, HANDLE h);   /* 只建事件，不接管 h 的所有权 */
void hid_io_close(HidIo *io);            /* 关事件（不关 h） */
int  hid_io_read(HidIo *io, unsigned char *buf, DWORD len, int timeout_ms);
int  hid_io_write(HidIo *io, const unsigned char *buf, DWORD len);

/* 一次性封装（内部临时建事件） */
int  hid_read_timeout(HANDLE h, unsigned char *buf, DWORD len, int timeout_ms);
int  hid_write(HANDLE h, const unsigned char *buf, DWORD len);

#endif /* HID_H */
