/* settings.c — 见 settings.h */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

#define INI_MAX_BYTES (16 * 1024)

static WCHAR *read_utf8_file(const WCHAR *path)
{
    HANDLE h;
    DWORD  sz = 0, got = 0;
    char  *raw;
    WCHAR *out;
    int    wlen;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > INI_MAX_BYTES) {
        CloseHandle(h);
        return NULL;
    }

    raw = (char *)malloc(sz + 1);
    if (!raw) {
        CloseHandle(h);
        return NULL;
    }
    if (!ReadFile(h, raw, sz, &got, NULL))
        got = 0;
    raw[got] = 0;
    CloseHandle(h);

    /* 跳过 UTF-8 BOM */
    {
        char *p = raw;
        if (got >= 3 && (unsigned char)p[0] == 0xEF
            && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;
        wlen = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
        if (wlen <= 0) {
            free(raw);
            return NULL;
        }
        out = (WCHAR *)malloc(sizeof(WCHAR) * wlen);
        if (out)
            MultiByteToWideChar(CP_UTF8, 0, p, -1, out, wlen);
    }
    free(raw);
    return out;
}

static WCHAR *trim(WCHAR *s)
{
    WCHAR *e;
    while (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n')
        s++;
    e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' || e[-1] == L'\n'))
        *--e = 0;
    return s;
}

static void set_str(WCHAR *dst, int cap, const WCHAR *src)
{
    wcsncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

void cfg_defaults(Cfg *c, const WCHAR *exe_dir)
{
    ZeroMemory(c, sizeof(*c));
    c->autostart    = 0;              /* 默认不写注册表，用户右键菜单里自己开 */
    c->low_notify   = 1;
    c->low_pct      = 20;
    c->recover_pct  = 25;
    c->idle_trim_s  = 120;
    wcscpy(c->kbd_name,    L"狼蛛 FN75 键盘");
    wcscpy(c->mouse_name,  L"迈从 G3 鼠标");
    _snwprintf(c->log_path, MAX_PATH, L"%ls\\tray.log", exe_dir);
    c->log_path[MAX_PATH - 1] = 0;
}

BOOL cfg_load(Cfg *c, const WCHAR *ini_path)
{
    WCHAR *text = read_utf8_file(ini_path);
    WCHAR *p;

    if (!text)
        return FALSE;

    p = text;
    while (p && *p) {
        WCHAR *line = p, *eq;

        p = wcschr(line, L'\n');
        if (p)
            *p++ = 0;

        line = trim(line);
        if (!*line || *line == L';' || *line == L'#' || *line == L'[')
            continue;

        eq = wcschr(line, L'=');
        if (!eq)
            continue;
        *eq = 0;
        {
            WCHAR *key = trim(line);
            WCHAR *val = trim(eq + 1);

            if (!_wcsicmp(key, L"autostart"))
                c->autostart = _wtoi(val) ? 1 : 0;
            else if (!_wcsicmp(key, L"low_notify"))
                c->low_notify = _wtoi(val) ? 1 : 0;
            else if (!_wcsicmp(key, L"low_pct"))
                c->low_pct = _wtoi(val);
            else if (!_wcsicmp(key, L"recover_pct"))
                c->recover_pct = _wtoi(val);
            else if (!_wcsicmp(key, L"idle_trim_seconds"))
                c->idle_trim_s = _wtoi(val);
            else if (!_wcsicmp(key, L"kbd_name"))
                set_str(c->kbd_name, 64, val);
            else if (!_wcsicmp(key, L"mouse_name"))
                set_str(c->mouse_name, 64, val);
        }
    }
    free(text);

    /* 数值兜底 */
    if (c->low_pct < 1 || c->low_pct > 99)
        c->low_pct = 20;
    if (c->recover_pct <= c->low_pct)
        c->recover_pct = c->low_pct + 5;
    if (c->idle_trim_s < 0)
        c->idle_trim_s = 0;
    return TRUE;
}

BOOL cfg_save(const Cfg *c, const WCHAR *ini_path)
{
    WCHAR  buf[2048];
    char   utf8[4096];
    WCHAR *exe_dir = NULL;
    HANDLE h;
    DWORD  wrote = 0;
    int    n;

    _snwprintf(buf, 2048,
               L"; 2.4G 设备电量托盘 —— 配置\n"
               L"; 改完保存，下次启动生效（菜单项会自动回写这两行）\n"
               L"autostart = %d\n"
               L"low_notify = %d\n"
               L"low_pct = %d\n"
               L"recover_pct = %d\n"
               L"idle_trim_seconds = %d\n"
               L"kbd_name = %ls\n"
               L"mouse_name = %ls\n",
               c->autostart, c->low_notify, c->low_pct,
               c->recover_pct, c->idle_trim_s, c->kbd_name, c->mouse_name);
    buf[2047] = 0;
    (void)exe_dir;

    n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 0)
        return FALSE;

    h = CreateFileW(ini_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    WriteFile(h, utf8, (DWORD)(n - 1), &wrote, NULL);
    CloseHandle(h);
    return TRUE;
}
