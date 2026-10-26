/* log.c — 见 log.h */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include "log.h"

#define LOG_MAX_BYTES (256 * 1024)

static HANDLE          g_f = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static BOOL            g_cs_ok = FALSE;

static void ensure_cs(void)
{
    if (!g_cs_ok) {
        InitializeCriticalSection(&g_cs);
        g_cs_ok = TRUE;
    }
}

BOOL log_open(const WCHAR *path)
{
    ensure_cs();
    EnterCriticalSection(&g_cs);

    if (g_f != INVALID_HANDLE_VALUE) {
        CloseHandle(g_f);
        g_f = INVALID_HANDLE_VALUE;
    }
    g_f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_f != INVALID_HANDLE_VALUE) {
        /* 超限则轮转：<path> → <path>.old */
        LARGE_INTEGER sz;
        if (GetFileSizeEx(g_f, &sz) && sz.QuadPart > LOG_MAX_BYTES) {
            WCHAR old[MAX_PATH + 8];
            CloseHandle(g_f);
            g_f = INVALID_HANDLE_VALUE;
            _snwprintf(old, MAX_PATH + 8, L"%ls.old", path);
            old[MAX_PATH + 7] = 0;
            DeleteFileW(old);
            MoveFileW(path, old);
            g_f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }

    LeaveCriticalSection(&g_cs);
    return g_f != INVALID_HANDLE_VALUE;
}

void log_close(void)
{
    if (!g_cs_ok)
        return;
    EnterCriticalSection(&g_cs);
    if (g_f != INVALID_HANDLE_VALUE) {
        CloseHandle(g_f);
        g_f = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_cs);
}

static void write_raw(const char *body)
{
    SYSTEMTIME st;
    char       line[1200];
    int        n;
    DWORD      wrote;

    if (g_f == INVALID_HANDLE_VALUE)
        return;

    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d  %s\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, body);
    if (n < 0)
        n = (int)sizeof(line) - 1;
    if (n > (int)sizeof(line) - 1)
        n = (int)sizeof(line) - 1;

    ensure_cs();
    EnterCriticalSection(&g_cs);
    if (g_f != INVALID_HANDLE_VALUE)
        WriteFile(g_f, line, (DWORD)n, &wrote, NULL);
    LeaveCriticalSection(&g_cs);
}

void log_line(const char *fmt, ...)
{
    char    body[1024];
    va_list ap;

    if (g_f == INVALID_HANDLE_VALUE)
        return;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = 0;
    write_raw(body);
}

void log_linew(const WCHAR *fmt, ...)
{
    WCHAR wbuf[1024];
    char  body[1024];
    va_list ap;

    if (g_f == INVALID_HANDLE_VALUE)
        return;

    va_start(ap, fmt);
    _vsnwprintf(wbuf, 1024, fmt, ap);
    va_end(ap);
    wbuf[1023] = 0;

    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, body, sizeof(body), NULL, NULL);
    body[sizeof(body) - 1] = 0;
    write_raw(body);
}
