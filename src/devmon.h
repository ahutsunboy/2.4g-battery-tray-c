/* devmon.h — 设备监控层（线程 + 状态机），UI 无关
 *
 * 职责：后台线程常驻，负责
 *   1. 枚举并打开 0xFF02 通道上的键盘/鼠标接收器
 *   2. 按协议轮询/被动收帧，解析电量与链路状态
 *   3. 判定「设备掉线」（关机 / 断连 / 接收器被拔）
 *   4. 状态有变化时 PostMessage 通知 UI 线程
 *
 * 所有共享数据由内部临界区保护，UI 只通过 dm_copy() 取快照。
 */
#ifndef DEVMON_H
#define DEVMON_H

#include <windows.h>

#define DM_MAX_DEV 4

/* note 字段容量 */
#define DM_MAXSTR 48

/* 设备种类 */
enum { DM_KIND_NONE = 0, DM_KIND_KEYBOARD = 1, DM_KIND_MOUSE = 2 };

/* online 字段取值 */
#define DM_UNKNOWN  (-1)
#define DM_OFFLINE  (0)
#define DM_ONLINE   (1)

typedef struct {
    int   kind;                 /* DM_KIND_* */
    WCHAR key[24];              /* "kbd" / "mouse"，用于配置与排序 */
    WCHAR name[64];             /* 显示名 */
    int   battery;              /* 0..100，-1 未知 */
    int   charging;             /* 1 充电中，0 未充电，-1 未知 */
    int   online;               /* DM_ONLINE / DM_OFFLINE / DM_UNKNOWN */
    int   fresh;                /* 1 表示本轮拿到的是真实帧，0 表示缓存/推断 */
    int   present;              /* 接收器是否可访问（拔掉 = 0） */
    DWORD last_frame_ms;        /* 最近一次收到帧的时刻（GetTickCount 毫秒） */
    WCHAR note[DM_MAXSTR];      /* 状态说明，如「设备已关闭」「等待状态帧」 */
} DmState;

/* 启动 / 停止监控线程。notify_wnd 收到 PostMessage(msg) 表示状态可能变化。 */
BOOL dm_start(HWND notify_wnd, UINT msg);
void dm_stop(void);

/* 取快照，返回设备数量 */
int  dm_copy(DmState *out, int max);

/* 打开状态：0=还没初始化 1=已初始化 -1=主动停止 */
int  dm_ready(void);

#endif /* DEVMON_H */
