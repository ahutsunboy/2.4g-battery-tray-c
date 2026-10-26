/* settings.h — 极简 UTF-8 INI 配置（不依赖 json）
 *
 * 文件位置：exe 同目录 settings.ini
 * 格式：
 *     autostart = 1
 *     low_notify = 1
 *     low_pct = 20
 *     recover_pct = 25
 *     idle_trim_seconds = 120
 *     kbd_name = 狼蛛 FN75 键盘
 *     mouse_name = 迈从 G3 鼠标
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <windows.h>

typedef struct {
    int   autostart;        /* 开机自启（默认 1） */
    int   low_notify;       /* 低电量提醒（默认 1） */
    int   low_pct;          /* 跌破多少 % 提醒（默认 20） */
    int   recover_pct;      /* 回到多少 % 重新武装提醒（默认 25） */
    int   idle_trim_s;      /* 空闲多少秒归还工作集（默认 120，0 = 关闭） */
    WCHAR kbd_name[64];
    WCHAR mouse_name[64];
    WCHAR log_path[MAX_PATH];
} Cfg;

void cfg_defaults(Cfg *c, const WCHAR *exe_dir);
BOOL cfg_load(Cfg *c, const WCHAR *ini_path);   /* 返回 TRUE 表示文件存在且读过 */
BOOL cfg_save(const Cfg *c, const WCHAR *ini_path);

#endif /* SETTINGS_H */
