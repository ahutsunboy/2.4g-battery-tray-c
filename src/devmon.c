/* devmon.c — 见 devmon.h
 *
 * 判定「设备掉线」的三条依据（按可靠性排序）：
 *   A. 键盘状态帧 13 0A 里的链路字节 data[7]：1 在线、非 1 离线 —— 直接可信
 *   B. 接收器在设备关机时推送的状态帧 09 0A xx（实测确认后启用）
 *   C. 兜底：键盘查询长期无任何回包 → 判定离线
 *
 * 为什么鼠标不能像键盘那样靠「静默」判定：
 *   实测该接收器没有保活帧，鼠标静止时一帧都不发。静默只能说明「没在动」，
 *   不能说明「关机了」。所以鼠标必须有 A/B 那类显式信号才判离线。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "devmon.h"
#include "hid.h"
#include "log.h"

#define KB_QUERY_MS     6000      /* 键盘主动查询间隔 */
#define KB_SILENCE_MS   20000     /* 键盘连续这么久没有任何回包 → 离线 */
#define KB_OFF_DEBOUNCE_MS 1000   /* 看到 link=0 后等这么久再提交，滤掉上电过渡帧 */
#define READ_SLICE_MS   25        /* 单次读超时（两个设备相加仍 < 60ms） */
#define RESCAN_MS       5000      /* 掉线后多久重扫一次接收器 */
#define FRESH_MS        3000      /* 收到帧后多少毫秒内算「新鲜数据」 */
#define MS_STALE_MS     600000    /* 鼠标 10 分钟没帧且光标没动 → 标「长时间无数据」 */

typedef struct {
    int     kind;
    HidDev  dev;
    HidIo   io;
    int     opened;
    double  next_query;           /* 下次发查询的时刻（秒） */
    double  last_ok;              /* 最近一次收到「有效业务帧」的时刻（秒） */
    double  retry_at;             /* 下次尝试打开的时刻（秒） */
    int     off_pending;          /* 已看到「离线」帧，等去抖窗口结束再提交 */
    double  off_since;
    int     status_sub;           /* 鼠标最近一次非电量状态帧的 sub code */
    DWORD   status_ms;
    DmState st;
} Mon;

static Mon              g_m[DM_MAX_DEV];
static int              g_n;
static CRITICAL_SECTION g_cs;
static BOOL             g_cs_ok;
static HANDLE           g_thread;
static HANDLE           g_stop;
static HWND             g_wnd;
static UINT             g_msg;
static volatile LONG    g_ready;
static DWORD            g_m_cursor_ms;   /* 光标最近一次移动的时刻 */

static double now_s(void) { return (double)GetTickCount64() / 1000.0; }

static void set_note(DmState *s, const WCHAR *note)
{
    if (!note) {
        s->note[0] = 0;
        return;
    }
    wcsncpy(s->note, note, DM_MAXSTR - 1);
    s->note[DM_MAXSTR - 1] = 0;
}

/* ----------------------------------------------------------- 状态变化通知 */

static DmState g_posted[DM_MAX_DEV];
static int     g_posted_n;

static unsigned sig(const DmState *s, WCHAR *note_out, int cap)
{
    unsigned h = 2166136261u;
    const unsigned char *p = (const unsigned char *)s;
    size_t i;

    /* 跳过 last_frame_ms（每次收帧都变，不该触发重绘）和 note 原文 */
    for (i = 0; i < offsetof(DmState, last_frame_ms); i++)
        h = (h ^ p[i]) * 16777619u;

    if (note_out && cap > 0) {
        wcsncpy(note_out, s->note, cap - 1);
        note_out[cap - 1] = 0;
    }
    for (i = 0; s->note[i] && i < DM_MAXSTR; i++)
        h = (h ^ (unsigned char)s->note[i]) * 16777619u;

    return h;
}

static void maybe_notify(void)
{
    DmState  snap[DM_MAX_DEV];
    int      n = 0, i;
    unsigned hs[DM_MAX_DEV];

    EnterCriticalSection(&g_cs);
    n = g_n;
    for (i = 0; i < n; i++) {
        snap[i] = g_m[i].st;
        hs[i]   = sig(&snap[i], NULL, 0);
    }
    LeaveCriticalSection(&g_cs);

    if (n != g_posted_n) {
        g_posted_n = n;
        for (i = 0; i < n; i++)
            g_posted[i] = snap[i];
        if (g_wnd)
            PostMessageW(g_wnd, g_msg, 0, 0);
        return;
    }

    for (i = 0; i < n; i++) {
        if (sig(&g_posted[i], NULL, 0) != hs[i]) {
            g_posted[i] = snap[i];
            if (g_wnd)
                PostMessageW(g_wnd, g_msg, 0, 0);
            return;                       /* 一条消息足够，UI 会整体重读 */
        }
    }
}

/* --------------------------------------------------------------- 帧解析 */

static void on_kb_frame(Mon *m, const unsigned char *b, int rc, double t)
{
    if (b[0] == 0x13 && b[1] == 0x0A && rc >= 8) {
        m->last_ok            = t;
        m->st.last_frame_ms   = GetTickCount();

        if (b[7] == 1) {                  /* 链路在线 */
            m->off_pending = 0;
            if (m->st.online != DM_ONLINE)
                log_line("KB link=1 -> ONLINE (batt=%u)", b[6]);
            m->st.online  = DM_ONLINE;
            m->st.present = 1;
            /* 只有 sub=0x05（查询回显）才带电量；sub=0x02 的链路事件里 b[6]
             * 不是电量，照着读会把电量刷成 0 */
            if (b[5] == 0x05 && b[6] <= 100)
                m->st.battery = b[6];
            set_note(&m->st, NULL);
        } else if (b[5] == 0x02) {        /* 权威的「链路断开」事件 */
            /* 不立刻下结论：上电瞬间会连发两帧 link=0 的过渡帧
             * （实测 113.83s / 114.00s），立刻提交会闪一下「已关闭」。 */
            if (!m->off_pending) {
                m->off_pending = 1;
                m->off_since   = t;
            }
            log_line("KB link=%u (pending offline) raw %02X %02X %02X %02X %02X %02X %02X %02X",
                     b[7], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        } else {
            /* sub=0x07 实测会在键盘明明在线时报 link=0（休眠通告），
             * 拿它当断开依据会平白闪一下「已关闭」，所以只记录不采信。 */
            log_line("KB sub=0x%02X link=0 (ignored, not a link event)", b[5]);
        }
        return;
    }

    if (b[0] == 0x13 && b[1] == 0x05) {   /* 查询回显：证明接收器活着 */
        m->last_ok = t;
        return;
    }

    log_line("KB unknown frame (%d bytes): %02X %02X %02X %02X %02X %02X",
             rc, b[0], b[1], b[2], b[3], b[4], b[5]);
}

static void on_ms_frame(Mon *m, const unsigned char *b, int rc, double t)
{
    if (b[0] != 0x09 || b[1] != 0x0A || rc < 4)
        return;

    m->last_ok          = t;
    m->st.last_frame_ms = GetTickCount();
    m->st.present       = 1;

    if (b[2] == 0x05) {                   /* 电量帧 */
        if (b[3] <= 100)
            m->st.battery = b[3];
        if (m->st.online != DM_ONLINE)
            log_line("MS battery frame batt=%u -> ONLINE", b[3]);
        m->st.online = DM_ONLINE;
        set_note(&m->st, NULL);
        return;
    }

    /* 非电量状态帧。实测：sub=0x02 出现在「鼠标重新上电」的瞬间，
     * data[3]=1，随后 1 秒内补一帧电量。据此认定它是链路事件帧。
     * 注意：关机那一刻接收器什么都不发（两分钟抓帧里 0 帧），
     * 所以关机只能靠别的途径判断，这一帧只解决「回来了」。 */
    m->status_sub = b[2];
    m->status_ms  = GetTickCount();
    log_line("MS status frame sub=0x%02X link=%u (%d bytes): %02X %02X %02X %02X %02X %02X %02X %02X",
             b[2], b[3], rc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

    if (b[2] == 0x02) {
        if (b[3] == 1) {
            m->st.online = DM_ONLINE;
            set_note(&m->st, NULL);
        } else {
            m->st.online = DM_OFFLINE;
            set_note(&m->st, L"设备已关闭");
        }
    }
}

/* --------------------------------------------------------------- 打开/关闭 */

static BOOL match_kind(const HidDev *d, int kind)
{
    if (d->usage_page != 0xFF02)
        return FALSE;
    if (kind == DM_KIND_KEYBOARD)
        return d->vid == 0x3554 && d->pid == 0xFA09;
    return d->vid == 0x258A && d->pid == 0x0138;
}

static void close_one(Mon *m)
{
    if (m->opened) {
        HANDLE h = m->io.h;
        hid_io_close(&m->io);          /* 只关事件，句柄还得自己关 */
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        m->opened = 0;
    }
    m->st.present = 0;
}

static void open_one(Mon *m, double t)
{
    HidDev devs[HID_MAX_DEVICES];
    int    n = hid_enum(devs, HID_MAX_DEVICES);
    int    i;

    for (i = 0; i < n; i++) {
        HANDLE   h;
        wchar_t  path[HID_PATH_CHARS];

        if (!match_kind(&devs[i], m->kind))
            continue;

        h = hid_open(devs[i].path, m->kind == DM_KIND_KEYBOARD);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        if (hid_io_init(&m->io, h) != 0) {
            CloseHandle(h);
            continue;
        }
        m->dev        = devs[i];
        m->opened     = 1;
        m->last_ok    = t;
        m->next_query = t;                 /* 立刻查一次 */
        m->st.present = 1;
        m->st.online  = DM_UNKNOWN;
        set_note(&m->st, L"等待数据");
        wcsncpy(path, devs[i].path, HID_PATH_CHARS - 1);
        path[HID_PATH_CHARS - 1] = 0;
        log_linew(L"opened %ls (%04X:%04X) in=%u out=%u",
                  m->kind == DM_KIND_KEYBOARD ? L"keyboard" : L"mouse",
                  devs[i].vid, devs[i].pid, devs[i].in_len, devs[i].out_len);
        return;
    }
    m->st.present = 0;
    set_note(&m->st, L"接收器未连接");
}

/* --------------------------------------------------------------- 轮询 */

static void pump_one(Mon *m, double t, int *need_rescan)
{
    unsigned char buf[65];
    int           rc, k;

    if (!m->opened) {
        if (t >= m->retry_at) {
            m->retry_at = t + RESCAN_MS / 1000.0;
            open_one(m, t);
        }
        return;
    }

    if (m->kind == DM_KIND_KEYBOARD && t >= m->next_query) {
        static const unsigned char q[20] = {
            0x13, 0x05, 0x01, 0, 0, 0, 0, 0, 0, 0,
            0,    0,    0,    0, 0, 0, 0, 0, 0, 0x19
        };
        /* 还没拿到过有效帧时快速连问，拿到后回到常规间隔 */
        m->next_query = t + (m->st.online == DM_UNKNOWN ? 0.7 : KB_QUERY_MS / 1000.0);
        if (hid_io_write(&m->io, q, sizeof(q)) < 0) {
            log_line("KB query write failed -> device may be gone");
            *need_rescan = 1;
        }
    }

    for (k = 0; k < 4; k++) {
        rc = hid_io_read(&m->io, buf, sizeof(buf), READ_SLICE_MS);
        if (rc == 0)
            break;                         /* 这一轮没数据，正常 */
        if (rc < 0) {                      /* 句柄失效：接收器被拔 */
            log_line("read failed -> closing (device removed?)");
            close_one(m);
            m->retry_at  = t + 1.0;
            *need_rescan = 1;
            return;
        }
        if (m->kind == DM_KIND_KEYBOARD)
            on_kb_frame(m, buf, rc, t);
        else
            on_ms_frame(m, buf, rc, t);
    }

    /* 去抖窗口到期 → 提交离线判定 */
    if (m->kind == DM_KIND_KEYBOARD && m->off_pending
        && m->st.online != DM_OFFLINE
        && (t - m->off_since) * 1000.0 > KB_OFF_DEBOUNCE_MS) {
        m->off_pending = 1;
        m->st.online   = DM_OFFLINE;
        m->st.present  = 1;
        set_note(&m->st, L"设备已关闭");
        log_line("KB -> OFFLINE (link byte was 0, debounce %.0fms passed)",
                 (double)KB_OFF_DEBOUNCE_MS);
    }

    /* 失去响应的兜底判定：仅对键盘（它有主动查询，静默即异常） */
    if (m->kind == DM_KIND_KEYBOARD
        && (t - m->last_ok) * 1000.0 > KB_SILENCE_MS
        && m->st.online != DM_OFFLINE) {
        m->st.online = DM_OFFLINE;
        set_note(&m->st, L"无响应");
        log_line("KB no response for %.0fs -> OFFLINE", t - m->last_ok);
    }
}

/* 光标是否在动：鼠标关机时它不可能动，动了就证明设备活着。
 * 每秒看一次即可，别把 GetCursorPos 塞进 10ms 的循环里。 */
static void poll_cursor(void)
{
    static DWORD last_ms;
    static POINT last_pt = { -1, -1 };
    DWORD now = GetTickCount();
    POINT p;

    if (now - last_ms < 1000)
        return;
    last_ms = now;

    if (!GetCursorPos(&p))
        return;
    if (p.x != last_pt.x || p.y != last_pt.y) {
        last_pt = p;
        EnterCriticalSection(&g_cs);
        if (g_n > 1) {
            g_m_cursor_ms = now;
            if (g_m[1].st.note[0] && !wcscmp(g_m[1].st.note, L"长时间无数据"))
                set_note(&g_m[1].st, NULL);
        }
        LeaveCriticalSection(&g_cs);
    }
}

/* 鼠标长时间无帧 → 打一个「未证实」的标记，不冒充「已关闭」 */
static void check_mouse_stale(void)
{
    DWORD now = GetTickCount();
    Mon  *m;

    if (g_n < 2)
        return;
    m = &g_m[1];
    if (m->kind != DM_KIND_MOUSE || m->st.online != DM_ONLINE)
        return;

    if (m->st.last_frame_ms != 0
        && now - m->st.last_frame_ms > MS_STALE_MS
        && now - g_m_cursor_ms > MS_STALE_MS
        && m->st.note[0] == 0) {
        set_note(&m->st, L"长时间无数据");
        log_line("MS no frame for 10min and cursor idle -> marked stale");
    }
}

static DWORD WINAPI mon_proc(LPVOID param)
{
    int i;

    (void)param;
    InterlockedExchange(&g_ready, 1);

    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        double t = now_s();
        int    k, need_rescan = 0;

        for (k = 0; k < g_n; k++)
            pump_one(&g_m[k], t, &need_rescan);

        poll_cursor();
        check_mouse_stale();
        maybe_notify();
        Sleep(10);
    }

    for (i = 0; i < g_n; i++)
        close_one(&g_m[i]);
    return 0;
}

/* --------------------------------------------------------------- 对外接口 */

static void add_dev(int kind, const WCHAR *key, const WCHAR *name)
{
    Mon *m;
    if (g_n >= DM_MAX_DEV)
        return;
    m = &g_m[g_n++];
    ZeroMemory(m, sizeof(*m));
    m->kind      = kind;
    m->st.kind   = kind;              /* 快照里也要带上，UI 靠它区分文案 */
    wcsncpy(m->st.key, key, sizeof(m->st.key) / sizeof(WCHAR) - 1);
    wcsncpy(m->st.name, name, sizeof(m->st.name) / sizeof(WCHAR) - 1);
    m->st.battery  = -1;
    m->st.charging = -1;
    m->st.online   = DM_UNKNOWN;
    m->st.present  = 0;
}

BOOL dm_start(HWND notify_wnd, UINT msg)
{
    if (g_thread)
        return TRUE;

    if (!g_cs_ok) {
        InitializeCriticalSection(&g_cs);
        g_cs_ok = TRUE;
    }

    g_wnd = notify_wnd;
    g_msg = msg;
    g_n   = 0;
    add_dev(DM_KIND_KEYBOARD, L"kbd",   L"狼蛛 FN75 键盘");
    add_dev(DM_KIND_MOUSE,    L"mouse", L"迈从 G3 鼠标");
    g_posted_n = 0;

    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop)
        return FALSE;

    g_thread = CreateThread(NULL, 0, mon_proc, NULL, 0, NULL);
    if (!g_thread) {
        CloseHandle(g_stop);
        g_stop = NULL;
        return FALSE;
    }
    return TRUE;
}

void dm_stop(void)
{
    if (!g_thread)
        return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    g_thread = NULL;
    g_stop   = NULL;
    InterlockedExchange(&g_ready, -1);
}

int dm_copy(DmState *out, int max)
{
    int i, n;
    DWORD now;

    if (!out || max <= 0 || !g_cs_ok)
        return 0;

    EnterCriticalSection(&g_cs);
    n = g_n;
    if (n > max)
        n = max;
    now = GetTickCount();
    for (i = 0; i < n; i++) {
        out[i] = g_m[i].st;
        out[i].fresh = (g_m[i].st.last_frame_ms != 0
                        && (DWORD)(now - g_m[i].st.last_frame_ms) < FRESH_MS);
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

int dm_ready(void)
{
    return (int)g_ready;
}
