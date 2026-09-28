#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "volume_track_renderer.h"

static COLORREF pixel_color(const unsigned char* pixels, int stride, int x, int y) {
    const unsigned char* p = pixels + y * stride + x * 4;
    return RGB(p[2], p[1], p[0]);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    constexpr int W = 192, H = 128, STRIDE = W * 4;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H; // top-down pixels
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);

    const COLORREF bg = RGB(22, 24, 27);
    const COLORREF neutral = RGB(98, 105, 117);
    const COLORREF border = RGB(74, 80, 90);
    const COLORREF hot = RGB(116, 123, 136);
    HBRUSH bg_brush = CreateSolidBrush(bg);
    HBRUSH neutral_brush = CreateSolidBrush(neutral);
    HBRUSH border_brush = CreateSolidBrush(border);
    HBRUSH hot_brush = CreateSolidBrush(hot);
    HPEN focus_pen = CreatePen(PS_SOLID, 1, neutral);

    // Normal/focused, hot, and pressed states share one full-height gray
    // channel. Native thumb input is deliberately 3 px left of center,
    // matching the screenshot: thumb center 31 vs channel center 34.
    const HBRUSH fills[3] = { neutral_brush, hot_brush, border_brush };
    bool geometry_centered = true;
    for (int panel = 0; panel < 3; ++panel) {
        int x0 = panel * 64;
        RECT bounds{ x0, 0, x0 + 64, H };
        RECT native_thumb{ x0 + 18, 54, x0 + 40, 70 };
        RECT centered = center_lowcast_volume_thumb(bounds, native_thumb);
        int native_width = native_thumb.right - native_thumb.left;
        int centered_width = centered.right - centered.left;
        int center_error2 = std::abs((centered.left + centered.right) -
                                     (bounds.left + bounds.right));
        geometry_centered = geometry_centered && centered_width == native_width &&
                            centered.top == native_thumb.top &&
                            centered.bottom == native_thumb.bottom &&
                            center_error2 == 0;
        paint_lowcast_volume_track(dc, bounds, native_thumb, bg_brush, neutral_brush,
                                   fills[panel], border_brush, focus_pen,
                                   panel == 0);
    }

    unsigned char* pixels = static_cast<unsigned char*>(bits);
    const COLORREF brown[] = { RGB(47, 41, 35), RGB(61, 49, 39),
                               RGB(132, 99, 65), RGB(115, 86, 50) };
    int brown_pixels = 0, neutral_pixels = 0;
    int hot_min_x = W, hot_max_x = -1;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        COLORREF c = pixel_color(pixels, STRIDE, x, y);
        if (c == neutral) ++neutral_pixels;
        if (c == hot) { if (x < hot_min_x) hot_min_x = x; if (x > hot_max_x) hot_max_x = x; }
        for (COLORREF banned : brown) if (c == banned) ++brown_pixels;
    }
    // Check actual raster centers using doubled pixel coordinates. For the
    // middle 64 px panel, control pixels [64,127] center at 191/2.
    int channel_min_x = W, channel_max_x = -1;
    for (int x = 64; x < 128; ++x) {
        if (pixel_color(pixels, STRIDE, x, 20) == neutral) {
            if (x < channel_min_x) channel_min_x = x;
            if (x > channel_max_x) channel_max_x = x;
        }
    }
    const int control_center2 = 64 + 128 - 1;
    const int channel_center2 = channel_min_x + channel_max_x;
    const int thumb_center2 = hot_min_x + hot_max_x;
    bool rendered_centered = channel_min_x <= channel_max_x &&
        hot_min_x <= hot_max_x && channel_max_x - channel_min_x + 1 == 4 &&
        channel_center2 == control_center2 && thumb_center2 == control_center2;
    bool continuous = true;
    for (int panel = 0; panel < 3; ++panel) {
        int cx = panel * 64 + 32;
        for (int y = 5; y < 54; ++y)
            continuous = continuous && pixel_color(pixels, STRIDE, cx, y) == neutral;
        for (int y = 70; y < H - 5; ++y)
            continuous = continuous && pixel_color(pixels, STRIDE, cx, y) == neutral;
    }

    BITMAPFILEHEADER bf{};
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bf.bfSize = bf.bfOffBits + STRIDE * H;
    FILE* file = nullptr;
    _wfopen_s(&file, argv[1], L"wb");
    bool wrote = file != nullptr;
    if (file) {
        wrote = fwrite(&bf, sizeof(bf), 1, file) == 1 &&
                fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, file) == 1 &&
                fwrite(pixels, STRIDE * H, 1, file) == 1;
        fclose(file);
    }

    SelectObject(dc, old_bitmap);
    DeleteObject(focus_pen);
    DeleteObject(hot_brush);
    DeleteObject(border_brush);
    DeleteObject(neutral_brush);
    DeleteObject(bg_brush);
    DeleteObject(bitmap);
    DeleteDC(dc);

    bool passed = continuous && geometry_centered && rendered_centered &&
                  brown_pixels == 0 && wrote;
    wprintf(L"%ls: native-like thumb offset -3 px corrected; exact raster center2 control=%d channel=%d thumb=%d; channel width=4 px; channel continuity=%ls; neutral-gray pixels=%d; brown-palette pixels=%d; bitmap=%dx%d\n",
            passed ? L"PASS" : L"FAIL",
            control_center2, channel_center2, thumb_center2,
            continuous ? L"full above/below thumb" : L"broken",
            neutral_pixels, brown_pixels, W, H);
    return passed ? 0 : 1;
}
