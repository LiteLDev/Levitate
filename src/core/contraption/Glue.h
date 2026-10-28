#pragma once

// Super glue regions (pure C++): boxes of blocks glued together, Create 0.5+ style.

#include <functional>
#include <optional>
#include <vector>

#include "core/contraption/Geometry.h"

namespace lv::core::contraption {

/// An inclusive box of block positions.
struct GlueBox {
    Pos min, max;

    friend bool operator==(GlueBox const&, GlueBox const&) = default;
    [[nodiscard]] static GlueBox of(Pos const& a, Pos const& b);
    [[nodiscard]] bool           contains(Pos const& p) const;
    [[nodiscard]] bool           intersects(GlueBox const& o) const;
    [[nodiscard]] Pos            size() const { return {max.x - min.x + 1, max.y - min.y + 1, max.z - min.z + 1}; }
    [[nodiscard]] GlueBox        moved(Pos const& d) const { return {min + d, max + d}; }
};

/// True when `a` and its neighbour `a + offset(d)` lie in one of `boxes`.
[[nodiscard]] bool isGlued(std::vector<GlueBox> const& boxes, Pos const& a, Dir d);

enum class GlueCheck { Ok, TooFar, CannotReach };

/// Validates a two-click selection (Create's SuperGlueSelectionHandler): every side at most
/// `maxRange` blocks, and the second corner reachable from the first through solid blocks
/// inside the box (`solid(p)` = a block that would be moved).
[[nodiscard]] GlueCheck
checkSelection(Pos const& first, Pos const& second, int maxRange, std::function<bool(Pos const&)> const& solid);

/// A box rotated by quarter turns and moved: the image of `box` under `place` (grid placement).
[[nodiscard]] GlueBox transformBox(GlueBox const& box, std::function<Pos(Pos const&)> const& place);

} // namespace lv::core::contraption
