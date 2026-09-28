#pragma once

#include <windows.h>

// Shared by the real control and the offscreen bitmap test. Every supplied
// track resource is neutral gray; this renderer has no access to LowCast's
// brown checked/pressed/focus resources.
inline RECT center_lowcast_volume_thumb(const RECT& bounds, const RECT& native_thumb) {
    RECT centered = native_thumb;
    const int width = native_thumb.right - native_thumb.left;
    const int cx = (bounds.left + bounds.right) / 2;
    centered.left = cx - width / 2;
    centered.right = centered.left + width; // preserve native width exactly
    return centered;                        // preserve native top/bottom exactly
}

inline RECT lowcast_volume_channel_rect(const RECT& bounds) {
    const int cx = (bounds.left + bounds.right) / 2;
    return RECT{ cx - 2, bounds.top + 5, cx + 2, bounds.bottom - 5 };
}

// TBS_VERT reports its long channel axis in RECT::left/right. Map a click to
// the native 0..100 track position, centered on the thumb and clamped.
inline int lowcast_vertical_volume_pos(const RECT& channel, const RECT& thumb,
                                       int mouse_y) {
    const int thumb_height = thumb.bottom - thumb.top;
    const int span = (channel.right - channel.left) - thumb_height;
    if (span <= 0) return -1;
    const int y = mouse_y - channel.left - thumb_height / 2;
    int pos = MulDiv(y, 100, span);
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    return pos;
}

inline void paint_lowcast_volume_track(HDC dc, const RECT& bounds,
                                       const RECT& native_thumb, HBRUSH background,
                                       HBRUSH neutral_gray,
                                       HBRUSH thumb_fill,
                                       HBRUSH neutral_border,
                                       HPEN neutral_focus_pen,
                                       bool focused) {
    FillRect(dc, &bounds, background);
    const RECT channel = lowcast_volume_channel_rect(bounds); // 4 px, raster-centered
    FillRect(dc, &channel, neutral_gray);
    const RECT thumb = center_lowcast_volume_thumb(bounds, native_thumb);
    FillRect(dc, &thumb, thumb_fill);
    FrameRect(dc, &thumb, neutral_border);
    if (focused) {
        RECT ring = bounds;
        InflateRect(&ring, -2, -2);
        HGDIOBJ old_pen = SelectObject(dc, neutral_focus_pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, ring.left, ring.top, ring.right, ring.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
    }
}
