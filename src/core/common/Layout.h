#pragma once

// Screen geometry for the native screen framework (U3, SPEC §3.3), independent of the game.
// Units are GUI pixels (the game's scaled UI coordinates).

namespace lv::core::common {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    [[nodiscard]] constexpr bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    [[nodiscard]] constexpr Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    [[nodiscard]] constexpr Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    friend constexpr bool        operator==(Rect const&, Rect const&) = default;
};

/// A panel of size (w, h) centred in a screen of size (sw, sh), snapped to whole pixels.
[[nodiscard]] constexpr Rect centred(float sw, float sh, float w, float h) {
    return {static_cast<float>(static_cast<int>((sw - w) / 2)), static_cast<float>(static_cast<int>((sh - h) / 2)), w, h};
}

/// Slot `index` of a grid starting at (x, y) with `columns` columns of `size`-pixel cells.
[[nodiscard]] constexpr Rect gridCell(float x, float y, int columns, float size, int index) {
    return {x + static_cast<float>(index % columns) * size, y + static_cast<float>(index / columns) * size, size, size};
}

/// The grid cell under (px, py), or -1.
[[nodiscard]] constexpr int gridIndexAt(float x, float y, int columns, int count, float size, float px, float py) {
    if (px < x || py < y) return -1;
    int c = static_cast<int>((px - x) / size), r = static_cast<int>((py - y) / size);
    if (c >= columns) return -1;
    int i = r * columns + c;
    return i < count ? i : -1;
}

} // namespace lv::core::common
