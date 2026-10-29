#pragma once

// Display items (U33): items the client mod draws in the world for a block (items on belts, in
// chutes, filter slots, placards, ...). Pure data, wire codec and interpolation; the server
// registry is feature/itemrender/DisplayItems.h, the renderer client/render/WorldItems.h.
//
// Wire format (companion channel "itemrender.batch", server -> client):
//     {"dim": 0, "set": [entry, ...], "move": [entry, ...], "del": [[x, y, z, slot], ...]}
//     entry = {"o": [x, y, z], "s": slot, "i": item json (set only), "p": [x, y, z],
//              "r": [yaw, pitch, roll], "c": scale, "st": style, "sp": spin, "pl": pile, "t": lerp ticks,
//              "h": actor runtime id to hide}
// Client -> server: "itemrender.query" {"dim": 0, "pos": [[x, y, z], ...]} asks for the display items of
// blocks the client sees that a provider draws (filter slots, placards, ...); "itemrender.reset"
// {"dim": 0} says the client dropped every item (new level / dimension): the server sends them again.

#include <array>
#include <cstdint>
#include <optional>

#include "nlohmann/json.hpp"

namespace lv::core::itemrender {

using Json = nlohmann::json;

/// How the canonical item is laid out before the transform (see WorldItems.h).
enum class Style : std::uint8_t {
    /// The item faces the transform's +Z (an item frame / filter slot / placard): flat items stand
    /// in the XY plane centred on `pos`, block items stand in front of that plane.
    Facing = 0,
    /// The item lies on a surface whose normal is the transform's +Y (Create's belt / depot /
    /// chute look): flat items lie face up, block items stand on it; `pos` is where it rests.
    Lying = 1,
};

/// Where and how an item is drawn. Angles in degrees: the item is turned by `roll` about Z,
/// then `pitch` about X, then `yaw` about Y (all about its own centre / resting point).
struct Transform {
    double x = 0, y = 0, z = 0;
    float  yaw = 0, pitch = 0, roll = 0;
    float  scale = 1; ///< size in blocks of the canonical (one block) item

    friend bool operator==(Transform const&, Transform const&) = default;
};

struct Entry {
    std::array<int, 3> owner{0, 0, 0}; ///< block the item belongs to
    std::int64_t       slot  = 0;      ///< distinct per owner
    Json               item;           ///< items::toJson of the stack (null in a "move")
    Transform          t;
    Style              style = Style::Facing;
    float              spin  = 0;     ///< degrees per tick about the item's own up axis
    bool               pile  = false; ///< extra copies for bigger stacks (Create's belt look)
    int                lerp  = 0;     ///< ticks the client takes to reach `t` from where it is
    std::int64_t       hide  = 0;     ///< runtime id of a fallback entity the client should not draw
};

/// Same within a thousandth of a block / degree.
[[nodiscard]] bool nearlySame(Transform const& a, Transform const& b);
/// The angle between `a` and `b` at `f` (0..1) going the short way round.
[[nodiscard]] float lerpAngle(float a, float b, float f);
/// The transform between `a` and `b` at `f` (clamped to 0..1).
[[nodiscard]] Transform lerp(Transform const& a, Transform const& b, float f);
/// How far a move that started at `start` (ticks) and lasts `ticks` is at `now` (0..1).
[[nodiscard]] float progress(double now, double start, int ticks);

/// Extra copies drawn for a stack of `count` items on a pile (Create: log2(count) / 2).
[[nodiscard]] int pileCopies(int count);
/// Offset (in canonical item units) and yaw (degrees) of pile copy `i` (1..), stable per copy.
[[nodiscard]] std::array<float, 4> pileOffset(int i);

[[nodiscard]] Json                 encode(Entry const& e, bool withItem);
[[nodiscard]] std::optional<Entry> decode(Json const& j);
/// [x, y, z, slot] of a deletion.
[[nodiscard]] Json encodeKey(std::array<int, 3> const& owner, std::int64_t slot);
[[nodiscard]] std::optional<std::pair<std::array<int, 3>, std::int64_t>> decodeKey(Json const& j);

} // namespace lv::core::itemrender
