/* art.h — 图标绘制（托盘电池图标 / 设备图标）
 *
 * 版式与配色与 Python 版 wsgui.py 完全一致：
 *   托盘图标 16×16 设计稿，设备图标 24×24 设计稿，两者都用 4× 超采样抗锯齿。
 * 这样两版 exe 出来的图标肉眼无法区分。
 */
#ifndef ART_H
#define ART_H

#include <windows.h>

/* 设备图标种类 */
#define ART_KBD     0
#define ART_MOUSE   1
#define ART_DONGLE  2

/* 电池托盘图标。pct<0 表示未知（画空电池），charging 非 0 时画闪电。
 * 返回的 HICON 由调用方 DestroyIcon。 */
HICON art_battery_icon(int pct, int charging);

/* 把设备图标画到 dc 的 (x,y)，边长 size 像素。
 * bg 是图标位置的真实底色：抗锯齿边缘要和底色混合，否则有一圈杂边。 */
void art_device_icon(HDC dc, int kind, int x, int y, int size, COLORREF bg);

/* 释放内部缓存的位图 */
void art_free(void);

#endif /* ART_H */
