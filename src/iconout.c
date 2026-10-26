/* iconout.c — 开发工具：把托盘图标导出成 BMP，用于和 Python 版逐像素对比
 *
 * 用法：iconout.exe out.bmp [bg=255]
 * 输出：一行 5 个图标（0% / 15% / 50% / 100% / 充电），衬在浅色底上
 */
#include <windows.h>
#include <stdio.h>

#include "art.h"

#define NPCT 5

int main(int argc, char **argv)
{
    static const int pcts[NPCT] = { 0, 15, 50, 100, -1 };
    static const int chg[NPCT]  = { 0, 0, 0, 0, 1 };
    int      size = GetSystemMetrics(SM_CXSMICON);
    int      W, H, i, x, y;
    int      bg = 245;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    unsigned char   *px;
    FILE            *f;

    if (argc > 2)
        bg = atoi(argv[2]);
    if (size < 16)
        size = 16;

    W = size * NPCT;
    H = size;
    px = (unsigned char *)calloc((size_t)W * H * 3, 1);
    if (!px)
        return 1;
    for (i = 0; i < W * H; i++) {
        px[i * 3 + 0] = (unsigned char)bg;
        px[i * 3 + 1] = (unsigned char)bg;
        px[i * 3 + 2] = (unsigned char)bg;
    }

    for (i = 0; i < NPCT; i++) {
        HICON  ic = art_battery_icon(pcts[i], chg[i]);
        HDC    dc;
        BITMAPINFO bi;
        void  *bits = NULL;
        HBITMAP dib;
        unsigned char *src;

        if (!ic)
            continue;
        dc = CreateCompatibleDC(NULL);
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = size;
        bi.bmiHeader.biHeight      = -size;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(dc, dib);
        /* 白底填充后再画图标，方便直接看抗锯齿边缘 */
        {
            RECT r = { 0, 0, size, size };
            HBRUSH b = CreateSolidBrush(RGB(bg, bg, bg));

            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        DrawIconEx(dc, 0, 0, ic, size, size, 0, NULL, DI_NORMAL);

        src = (unsigned char *)bits;
        for (y = 0; y < size; y++)
            for (x = 0; x < size; x++) {
                unsigned char *d = px + ((size_t)y * W + i * size + x) * 3;
                unsigned char *s = src + ((size_t)y * size + x) * 4;

                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }

        DeleteObject(dib);
        DeleteDC(dc);
        DestroyIcon(ic);
    }

    if (!argv[1]) {
        free(px);
        return 1;
    }
    f = fopen(argv[1], "wb");
    if (!f) {
        free(px);
        return 1;
    }

    ZeroMemory(&fh, sizeof(fh));
    ZeroMemory(&ih, sizeof(ih));
    ih.biSize        = sizeof(ih);
    ih.biWidth       = W;
    ih.biHeight      = H;
    ih.biPlanes      = 1;
    ih.biBitCount    = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = (DWORD)(((W * 3 + 3) & ~3) * H);
    fh.bfType        = 0x4D42;
    fh.bfOffBits     = sizeof(fh) + sizeof(ih);
    fh.bfSize        = fh.bfOffBits + ih.biSizeImage;

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (y = H - 1; y >= 0; y--) {                 /* BMP 自下而上 */
        int pad = ((W * 3 + 3) & ~3) - W * 3;

        fwrite(px + (size_t)y * W * 3, (size_t)W * 3, 1, f);
        if (pad) {
            unsigned char z[3] = { 0, 0, 0 };
            fwrite(z, (size_t)pad, 1, f);
        }
    }
    fclose(f);
    printf("%s  %dx%d  icon=%dpx\n", argv[1], W, H, size);
    free(px);
    return 0;
}
