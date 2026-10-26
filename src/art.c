/* art.c — 逐像素对齐 Python 版 wsgui.py 的图标栅格化
 *
 * Python 版的做法是：先在 4 倍分辨率上把每个子像素按「形状覆盖」打上颜色标签，
 * 再下采样成目标尺寸，得到带抗锯齿的 32bpp 位图。这里照搬同一套算法与坐标，
 * 所以两版的图标形状、圆角、灰阶边缘完全一致。
 */
#include "art.h"

#include <stdlib.h>
#include <string.h>

#define SS 4                     /* 超采样倍数 */

typedef struct { double r, g, b; } Col;
typedef struct { double x0, y0, x1, y1, r; } Rr;

static Col col(int r, int g, int b)
{
    Col c;
    c.r = r; c.g = g; c.b = b;
    return c;
}

/* --------------------------------------------------------------- 形状基元 */

static Rr rr(double x0, double y0, double x1, double y1, double r)
{
    Rr q;
    q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1; q.r = r;
    return q;
}

/* 圆角矩形覆盖判定（浮点边界，和 Python 的 _round_rect_mask 等价） */
static int in_rr(const Rr *q, double px, double py)
{
    double cx, cy, dx, dy;

    if (px < q->x0 || px > q->x1 || py < q->y0 || py > q->y1)
        return 0;
    cx = px < q->x0 + q->r ? q->x0 + q->r : (px > q->x1 - q->r ? q->x1 - q->r : px);
    cy = py < q->y0 + q->r ? q->y0 + q->r : (py > q->y1 - q->r ? q->y1 - q->r : py);
    dx = px - cx;
    dy = py - cy;
    return dx * dx + dy * dy <= q->r * q->r;
}

static void put_mask(unsigned char *t, int W, const Rr *q, unsigned char tag)
{
    int x, y;

    for (y = 0; y < W; y++)
        for (x = 0; x < W; x++)
            if (in_rr(q, x + 0.5, y + 0.5))
                t[y * W + x] = tag;
}

/* 环形（外框挖掉内框）：Python 里是 rr_outer(px,py) and not rr_inner(px,py) */
static void put_ring(unsigned char *t, int W, const Rr *o, const Rr *i,
                     unsigned char tag)
{
    int x, y;

    for (y = 0; y < W; y++)
        for (x = 0; x < W; x++)
            if (in_rr(o, x + 0.5, y + 0.5) && !in_rr(i, x + 0.5, y + 0.5))
                t[y * W + x] = tag;
}

/* 多边形覆盖判定（奇偶规则），给充电闪电用 */
static int in_poly(const double *v, int n, double px, double py)
{
    int    i, j, inside = 0;

    for (i = 0, j = n - 1; i < n; j = i++) {
        double xi = v[i * 2], yi = v[i * 2 + 1];
        double xj = v[j * 2], yj = v[j * 2 + 1];

        if (((yi > py) != (yj > py))
            && (px < (xj - xi) * (py - yi) / (yj - yi + 1e-9) + xi))
            inside = !inside;
    }
    return inside;
}

/* --------------------------------------------------------------- 位图封装 */

static HBITMAP make_dib(HDC ref, int w, int h, void **bits)
{
    BITMAPINFO bi;
    HBITMAP    bm;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          /* 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bm = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    return bm;
}

/* --------------------------------------------------------------- 托盘图标 */

/* 16×16 设计稿上的电池。几何与配色照搬 Python 版 render_battery_icon。 */
HICON art_battery_icon(int pct, int charging)
{
    int            size = GetSystemMetrics(SM_CXSMICON);
    int            W, i;
    unsigned char *tag;
    Col            pal[5];
    Col            outline, track, fill;
    Rr             body, inner, nub;
    double         u, stroke, pad, bx0, bx1, by0, by1, frac, fx1;
    HDC            dc, mdc;
    HBITMAP        dib, mask;
    void          *bits = NULL;
    BYTE          *px;
    ICONINFO       ii;
    ICONINFO       zero;
    HICON          ic = NULL;

    (void)zero;

    if (size < 16)
        size = 16;
    W   = size * SS;
    u   = (double)W / 16.0;
    tag = (unsigned char *)calloc((size_t)W * (size_t)W, 1);
    if (!tag)
        return NULL;

    outline = col(0x6B, 0x72, 0x80);
    track   = col(0xD1, 0xD5, 0xDB);
    if (pct < 0)
        pct = 0;
    if (pct < 20)
        fill = col(0xDC, 0x26, 0x26);
    else if (pct < 45)
        fill = col(0xF5, 0x9E, 0x0B);
    else
        fill = col(0x16, 0xA3, 0x4A);
    if (charging)
        fill = col(0x25, 0x63, 0xEB);

    pal[1] = outline;
    pal[2] = track;
    pal[3] = fill;
    pal[4] = col(0xFF, 0xFF, 0xFF);          /* 闪电 */

    /* 外壳：外框挖出内框，形成一圈等宽描边 */
    body   = rr(0.9 * u, 3.9 * u, 12.9 * u, 12.1 * u, 2.2 * u);
    stroke = 1.15 * u;
    inner  = rr(body.x0 + stroke, body.y0 + stroke,
                body.x1 - stroke, body.y1 - stroke, 1.6 * u);
    put_ring(tag, W, &body, &inner, 1);

    /* 正极触点 */
    nub = rr(13.1 * u, 6.3 * u, 15.2 * u, 9.7 * u, 0.6 * u);
    put_mask(tag, W, &nub, 1);

    /* 电量槽 + 填充 */
    pad = 1.9 * u;
    bx0 = body.x0 + pad;
    bx1 = body.x1 - pad;
    by0 = body.y0 + pad;
    by1 = body.y1 - pad;
    {
        Rr tr = rr(bx0, by0, bx1, by1, 1.0 * u);
        put_mask(tag, W, &tr, 2);
    }

    frac = (double)pct / 100.0;
    if (frac > 1.0)
        frac = 1.0;
    if (frac > 0.02) {
        Rr fr;

        fx1 = bx0 + (bx1 - bx0) * frac;
        if (fx1 < bx0 + 1.3 * u)
            fx1 = bx0 + 1.3 * u;
        fr = rr(bx0, by0, fx1, by1, 1.0 * u);
        put_mask(tag, W, &fr, 3);
    }

    /* 充电闪电 */
    if (charging) {
        double cx = (bx0 + bx1) / 2.0, cy = (by0 + by1) / 2.0;
        double v[12];
        int    x, y;

        v[0]  = cx + 0.55 * u; v[1]  = cy - 1.9 * u;
        v[2]  = cx - 0.75 * u; v[3]  = cy + 0.35 * u;
        v[4]  = cx - 0.05 * u; v[5]  = cy + 0.35 * u;
        v[6]  = cx - 0.5 * u;  v[7]  = cy + 2.0 * u;
        v[8]  = cx + 0.8 * u;  v[9]  = cy - 0.25 * u;
        v[10] = cx + 0.1 * u;  v[11] = cy - 0.25 * u;

        for (y = 0; y < W; y++)
            for (x = 0; x < W; x++)
                if (in_poly(v, 6, x + 0.5, y + 0.5))
                    tag[y * W + x] = 4;
    }

    /* 下采样：颜色取平均，alpha 取覆盖比例（图标用直通 alpha） */
    dc  = CreateCompatibleDC(NULL);
    dib = make_dib(dc, size, size, &bits);
    if (!dib) {
        DeleteDC(dc);
        free(tag);
        return NULL;
    }
    px = (BYTE *)bits;
    for (i = 0; i < size * size; i++) {
        int    ox = (i % size) * SS, oy = (i / size) * SS;
        int    cnt = 0, sx, sy;
        double sr = 0, sg = 0, sb = 0;

        for (sy = 0; sy < SS; sy++)
            for (sx = 0; sx < SS; sx++) {
                unsigned char t = tag[(oy + sy) * W + ox + sx];

                if (!t)
                    continue;
                sr += pal[t].r;
                sg += pal[t].g;
                sb += pal[t].b;
                cnt++;
            }
        if (!cnt)
            continue;
        /* 取整方式也要和 Python 一致：那边是 // 整除（向下取整），
         * 差一点点就能让边缘像素差 1，逐像素比对时会被算作差异 */
        px[i * 4 + 0] = (BYTE)((int)(sb / cnt));
        px[i * 4 + 1] = (BYTE)((int)(sg / cnt));
        px[i * 4 + 2] = (BYTE)((int)(sr / cnt));
        px[i * 4 + 3] = (BYTE)(255 * cnt / (SS * SS));
    }
    free(tag);

    /* 掩码全 0（不遮挡），实际透明度由 32bpp 的 alpha 通道决定 */
    mdc  = CreateCompatibleDC(NULL);
    mask = CreateBitmap(size, size, 1, 1, NULL);
    if (mask && mdc) {
        RECT r = { 0, 0, size, size };
        HBRUSH bk = (HBRUSH)GetStockObject(BLACK_BRUSH);

        SelectObject(mdc, mask);
        FillRect(mdc, &r, bk);

        ZeroMemory(&ii, sizeof(ii));
        ii.fIcon    = TRUE;
        ii.hbmColor = dib;
        ii.hbmMask  = mask;
        ic = CreateIconIndirect(&ii);
    }
    if (mask)
        DeleteObject(mask);
    if (mdc)
        DeleteDC(mdc);
    DeleteObject(dib);
    DeleteDC(dc);
    return ic;
}

/* --------------------------------------------------------------- 设备图标 */

/* 24×24 设计稿，灰色描边 + 浅蓝填充（对齐 UI/设备图标.png） */
static void draw_kbd(unsigned char *t, int W)
{
    double u = (double)W / 24.0;
    int    i;

    put_ring(t, W, &(Rr){ 1.6 * u, 6.2 * u, 22.4 * u, 17.8 * u, 2.6 * u },
                     &(Rr){ 3.1 * u, 7.7 * u, 20.9 * u, 16.3 * u, 1.6 * u }, 1);
    for (i = 0; i < 2; i++) {
        double row = i ? 12.1 : 9.2;
        int    k;

        for (k = 0; k < 4; k++) {
            double x0 = 4.7 + k * 4.05;

            put_mask(t, W, &(Rr){ x0 * u, row * u, (x0 + 2.95) * u,
                                  (row + 1.6) * u, 0.45 * u }, 2);
        }
    }
    put_mask(t, W, &(Rr){ 8.4 * u, 14.8 * u, 15.6 * u, 16.15 * u, 0.45 * u }, 2);
}

static void draw_mouse(unsigned char *t, int W)
{
    double u = (double)W / 24.0;
    Rr     outer = { 7.2 * u, 2.6 * u, 16.8 * u, 21.4 * u, 4.4 * u };
    Rr     inner = { 8.7 * u, 4.1 * u, 15.3 * u, 19.9 * u, 2.9 * u };
    Rr     line  = { 8.2 * u, 9.85 * u, 15.8 * u, 10.75 * u, 0.45 * u };
    int    x, y;

    put_ring(t, W, &outer, &inner, 1);

    /* 中键分界线：只画在机身内部 */
    for (y = 0; y < W; y++)
        for (x = 0; x < W; x++)
            if (in_rr(&line, x + 0.5, y + 0.5) && in_rr(&inner, x + 0.5, y + 0.5))
                t[y * W + x] = 1;

    put_mask(t, W, &(Rr){ 10.95 * u, 5.3 * u, 13.05 * u, 8.6 * u, 1.0 * u }, 2);
}

static void draw_dongle(unsigned char *t, int W)
{
    double u = (double)W / 24.0;

    put_ring(t, W, &(Rr){ 7.4 * u, 2.4 * u, 16.6 * u, 21.6 * u, 3.0 * u },
                     &(Rr){ 8.9 * u, 3.9 * u, 15.1 * u, 20.1 * u, 2.0 * u }, 1);
    put_mask(t, W, &(Rr){ 9.6 * u, 4.6 * u, 14.4 * u, 9.4 * u, 0.8 * u }, 2);
    put_mask(t, W, &(Rr){ 10.9 * u, 13.4 * u, 13.1 * u, 15.6 * u, 1.1 * u }, 1);
}

typedef struct {
    int     kind, size, bg;
    HDC     dc;
    HBITMAP bm;
    HBITMAP old;
} DevIcon;

static DevIcon g_dev[8];

void art_device_icon(HDC dc, int kind, int x, int y, int size, COLORREF bg)
{
    static const Col ICON_BLUE = { 0x7C, 0xC8, 0xF2 };
    static const Col ICON_GRAY = { 0x7E, 0x80, 0x86 };
    Col            pal[3];
    DevIcon       *hit = NULL;
    unsigned char *tag;
    int            W = size * SS, i;
    void          *bits = NULL;

    size = (int)size;
    if (size < 8 || !dc)
        return;

    for (i = 0; i < (int)(sizeof(g_dev) / sizeof(g_dev[0])); i++) {
        if (g_dev[i].dc && g_dev[i].kind == kind && g_dev[i].size == size
            && g_dev[i].bg == (int)bg) {
            hit = &g_dev[i];
            break;
        }
        if (!g_dev[i].dc && !hit)
            hit = &g_dev[i];       /* 记下空槽，渲染完再填 */
    }
    if (!hit)
        return;
    if (hit->dc) {                 /* 命中缓存 */
        BitBlt(dc, x, y, size, size, hit->dc, 0, 0, SRCCOPY);
        return;
    }

    tag = (unsigned char *)calloc((size_t)W * (size_t)W, 1);
    if (!tag)
        return;
    pal[1] = ICON_GRAY;
    pal[2] = ICON_BLUE;

    if (kind == ART_KBD)
        draw_kbd(tag, W);
    else if (kind == ART_MOUSE)
        draw_mouse(tag, W);
    else
        draw_dongle(tag, W);

    hit->dc = CreateCompatibleDC(dc);
    hit->bm = make_dib(dc, size, size, &bits);
    if (!hit->dc || !hit->bm) {
        if (hit->dc)
            DeleteDC(hit->dc);
        if (hit->bm)
            DeleteObject(hit->bm);
        ZeroMemory(hit, sizeof(*hit));
        free(tag);
        return;
    }
    hit->old = (HBITMAP)SelectObject(hit->dc, hit->bm);
    hit->kind = kind;
    hit->size = size;
    hit->bg   = (int)bg;

    /* 下采样并直接合成到底色上：图标位置底色固定，不必运行时 AlphaBlend
     *（本机该 API 对 DIB 源位图会返回参数错误，Python 版也踩过） */
    {
        BYTE *px = (BYTE *)bits;
        int   br = GetRValue(bg), bgc = GetGValue(bg), bb = GetBValue(bg);

        for (i = 0; i < size * size; i++) {
            int    ox = (i % size) * SS, oy = (i / size) * SS;
            int    cnt = 0, sx, sy;
            double sr = 0, sg = 0, sb = 0, cov;

            for (sy = 0; sy < SS; sy++)
                for (sx = 0; sx < SS; sx++) {
                    unsigned char t = tag[(oy + sy) * W + ox + sx];

                    if (!t)
                        continue;
                    sr += pal[t].r;
                    sg += pal[t].g;
                    sb += pal[t].b;
                    cnt++;
                }
            if (!cnt) {
                px[i * 4 + 0] = (BYTE)bb;
                px[i * 4 + 1] = (BYTE)bgc;
                px[i * 4 + 2] = (BYTE)br;
                px[i * 4 + 3] = 255;
                continue;
            }
            cov = (double)cnt / (SS * SS);
            px[i * 4 + 0] = (BYTE)(int)((sb / cnt) * cov + bb * (1.0 - cov) + 0.5);
            px[i * 4 + 1] = (BYTE)(int)((sg / cnt) * cov + bgc * (1.0 - cov) + 0.5);
            px[i * 4 + 2] = (BYTE)(int)((sr / cnt) * cov + br * (1.0 - cov) + 0.5);
            px[i * 4 + 3] = 255;
        }
    }
    free(tag);
    BitBlt(dc, x, y, size, size, hit->dc, 0, 0, SRCCOPY);
}

void art_free(void)
{
    int i;

    for (i = 0; i < (int)(sizeof(g_dev) / sizeof(g_dev[0])); i++) {
        if (!g_dev[i].dc)
            continue;
        SelectObject(g_dev[i].dc, g_dev[i].old);
        DeleteObject(g_dev[i].bm);
        DeleteDC(g_dev[i].dc);
        ZeroMemory(&g_dev[i], sizeof(g_dev[i]));
    }
}
