/* hid.c — 见 hid.h */
#include "hid.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <stdlib.h>
#include <string.h>

/* GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030} */
static const GUID kHidGuid =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

int hid_enum(HidDev *out, int max)
{
    HDEVINFO set;
    int n = 0;
    DWORD i;

    if (!out || max <= 0)
        return 0;

    set = SetupDiGetClassDevsW(&kHidGuid, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return 0;

    for (i = 0; ; i++) {
        SP_DEVICE_INTERFACE_DATA did;
        DWORD need = 0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        HANDLE h;

        ZeroMemory(&did, sizeof(did));
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &kHidGuid, i, &did))
            break;

        /* 第一次只问需要多大缓冲区 */
        SetupDiGetDeviceInterfaceDetailW(set, &did, NULL, 0, &need, NULL);
        if (need == 0)
            continue;

        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!detail)
            continue;
        /* x64 下这个结构体是 8 字节对齐的，cbSize 必须写真实大小（8），
           写 sizeof 会得到 12，SetupAPI 直接返回参数错误 */
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, &need, NULL)) {
            free(detail);
            continue;
        }

        /* 先要读写权限；拿不到再退成 0 权限（只查询信息用） */
        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileW(detail->DevicePath, 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            HIDD_ATTRIBUTES attr;
            attr.Size = sizeof(attr);
            if (HidD_GetAttributes(h, &attr) && n < max) {
                HidDev *d = &out[n];
                PHIDP_PREPARSED_DATA pp = NULL;

                ZeroMemory(d, sizeof(*d));
                wcsncpy(d->path, detail->DevicePath, HID_PATH_CHARS - 1);
                d->vid = attr.VendorID;
                d->pid = attr.ProductID;
                d->version = attr.VersionNumber;

                if (HidD_GetPreparsedData(h, &pp)) {
                    HIDP_CAPS caps;
                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                        d->usage_page = caps.UsagePage;
                        d->usage = caps.Usage;
                        d->in_len = caps.InputReportByteLength;
                        d->out_len = caps.OutputReportByteLength;
                        d->feat_len = caps.FeatureReportByteLength;
                    }
                    HidD_FreePreparsedData(pp);
                }
                /* 取不到产品名时留空，上层自己给默认名 */
                if (!HidD_GetProductString(h, d->product, sizeof(d->product)))
                    d->product[0] = L'\0';
                n++;
            }
            CloseHandle(h);
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(set);
    return n;
}

HANDLE hid_open(const wchar_t *path, int want_write)
{
    DWORD access = GENERIC_READ | (want_write ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE && want_write) {
        /* 有些通道只允许读：退回只读再试一次 */
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    }
    return h;
}

/* ------------------------------------------------------------ 常驻 IO 上下文 */

int hid_io_init(HidIo *io, HANDLE h)
{
    if (!io)
        return -1;
    io->h  = h;
    io->ev = (h && h != INVALID_HANDLE_VALUE)
                 ? CreateEventW(NULL, TRUE, FALSE, NULL)
                 : NULL;
    return (io->ev && h && h != INVALID_HANDLE_VALUE) ? 0 : -1;
}

void hid_io_close(HidIo *io)
{
    if (!io)
        return;
    if (io->ev) {
        CloseHandle(io->ev);
        io->ev = NULL;
    }
    io->h = NULL;
}

int hid_io_read(HidIo *io, unsigned char *buf, DWORD len, int timeout_ms)
{
    OVERLAPPED ov;
    DWORD      got = 0;

    if (!io || !io->h || io->h == INVALID_HANDLE_VALUE || !io->ev || !buf || len == 0)
        return -1;

    /* 复用的 OVERLAPPED：每次清零，事件先复位 */
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = io->ev;
    ResetEvent(io->ev);

    if (ReadFile(io->h, buf, len, &got, &ov))
        return (int)got;

    if (GetLastError() != ERROR_IO_PENDING)
        return -1;                      /* 句柄已失效：接收器被拔了 */

    if (WaitForSingleObject(io->ev, (DWORD)timeout_ms) == WAIT_OBJECT_0) {
        if (GetOverlappedResult(io->h, &ov, &got, FALSE))
            return (int)got;
        return -1;                      /* 设备拔掉时这里会失败 */
    }

    /* 超时：必须取消并等它真正退出内核，否则 OVERLAPPED 会被踩坏 */
    CancelIoEx(io->h, &ov);
    WaitForSingleObject(io->ev, 50);
    GetOverlappedResult(io->h, &ov, &got, TRUE);   /* 排空完成状态 */
    return 0;                                      /* 0 = 没数据，不是错误 */
}

int hid_io_write(HidIo *io, const unsigned char *buf, DWORD len)
{
    OVERLAPPED ov;
    DWORD      done = 0;

    if (!io || !io->h || io->h == INVALID_HANDLE_VALUE || !io->ev || !buf || len == 0)
        return -1;

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = io->ev;
    ResetEvent(io->ev);

    if (WriteFile(io->h, buf, len, &done, &ov))
        return (int)done;

    if (GetLastError() != ERROR_IO_PENDING)
        return -1;

    if (WaitForSingleObject(io->ev, 500) == WAIT_OBJECT_0
        && GetOverlappedResult(io->h, &ov, &done, FALSE))
        return (int)done;

    CancelIoEx(io->h, &ov);
    WaitForSingleObject(io->ev, 50);
    GetOverlappedResult(io->h, &ov, &done, TRUE);
    return -1;
}

/* ------------------------------------------------------------ 一次性封装 */

int hid_read_timeout(HANDLE h, unsigned char *buf, DWORD len, int timeout_ms)
{
    HidIo io;
    int   rc;

    if (hid_io_init(&io, h) != 0)
        return -1;
    rc = hid_io_read(&io, buf, len, timeout_ms);
    hid_io_close(&io);
    return rc;
}

int hid_write(HANDLE h, const unsigned char *buf, DWORD len)
{
    HidIo io;
    int   rc;

    if (hid_io_init(&io, h) != 0)
        return -1;
    rc = hid_io_write(&io, buf, len);
    hid_io_close(&io);
    return rc;
}
