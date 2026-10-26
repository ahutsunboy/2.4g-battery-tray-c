/* log.h — 极简文件日志（UTF-8，带时间戳，超 256 KB 自动轮转为 .old）
 *
 * 只在需要时打开，避免常驻内存与 IO。
 */
#ifndef LOG_H
#define LOG_H

#include <windows.h>

BOOL log_open(const WCHAR *path);
void log_close(void);

/* 把一行写进日志（自动加 "YYYY-MM-DD HH:MM:SS " 前缀和换行） */
void log_line(const char *fmt, ...);

/* 宽字符版本（内部转 UTF-8） */
void log_linew(const WCHAR *fmt, ...);

#endif /* LOG_H */
