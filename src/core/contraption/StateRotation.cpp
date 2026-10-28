#include "core/contraption/StateRotation.h"

#include <optional>

namespace lv::core::contraption {

namespace {

std::int64_t* intOf(States& s, std::string const& key) {
    auto it = s.find(key);
    return it == s.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
}
std::string* strOf(States& s, std::string const& key) {
    auto it = s.find(key);
    return it == s.end() ? nullptr : std::get_if<std::string>(&it->second);
}

/// Rotates a direction that may only stay horizontal; nullopt if it would turn vertical.
std::optional<Dir> rotateHorizontal(Dir d, Axis axis, int q) {
    Dir r = rotate(d, axis, q);
    if (axisOf(r) == Axis::Y) return std::nullopt;
    return r;
}

// stairs: 0 east, 1 west, 2 south, 3 north
constexpr std::array<Dir, 4> kWeirdo{Dir::East, Dir::West, Dir::South, Dir::North};

} // namespace

std::array<Dir, 4> radialSides(Axis axis) {
    switch (axis) {
    case Axis::X:
        return {Dir::North, Dir::South, Dir::Up, Dir::Down};
    case Axis::Z:
        return {Dir::Up, Dir::Down, Dir::East, Dir::West};
    default:
        return {Dir::North, Dir::South, Dir::East, Dir::West};
    }
}

bool rotateStates(States& s, Axis axis, int quarterTurns) {
    int q = normQuarter(quarterTurns);
    if (q == 0) return false;
    bool changed = false;
    auto setInt  = [&](std::int64_t* v, std::int64_t nv) {
        if (*v != nv) changed = true;
        *v = nv;
    };
    auto setStr = [&](std::string* v, std::string_view nv) {
        if (*v != nv) changed = true;
        *v = std::string(nv);
    };

    // chassis sticky sides first: they depend on the axis before rotation
    if (auto* ax = intOf(s, "levitate:axis"); ax && *ax >= 0 && *ax <= 2) {
        Axis oldAxis = static_cast<Axis>(*ax);
        Axis newAxis = rotate(oldAxis, axis, q);
        auto* top    = intOf(s, "levitate:sticky_top");
        auto* bottom = intOf(s, "levitate:sticky_bottom");
        if (top && bottom) {
            // top = the positive end of the axis
            Dir  newTopDir  = rotate(dirOf(oldAxis, true), axis, q);
            if (!positive(newTopDir)) {
                auto t = *top;
                setInt(top, *bottom);
                setInt(bottom, t);
            }
        }
        static constexpr std::array<char const*, 4> kRadialKeys{
            "levitate:sticky_north",
            "levitate:sticky_south",
            "levitate:sticky_east",
            "levitate:sticky_west"
        };
        std::array<std::int64_t*, 4> radial{};
        bool                         isRadial = true;
        for (int i = 0; i < 4; ++i) isRadial = isRadial && (radial[i] = intOf(s, kRadialKeys[i])) != nullptr;
        if (isRadial) {
            auto                         oldSides = radialSides(oldAxis);
            auto                         newSides = radialSides(newAxis);
            std::array<std::int64_t, 4>  values{};
            for (int i = 0; i < 4; ++i) {
                Dir world = rotate(oldSides[i], axis, q);
                for (int j = 0; j < 4; ++j)
                    if (newSides[j] == world) values[j] = *radial[i];
            }
            for (int j = 0; j < 4; ++j) setInt(radial[j], values[j]);
        }
        setInt(ax, static_cast<std::int64_t>(newAxis));
    }

    for (auto const* key : {"facing_direction", "levitate:facing"}) {
        if (auto* v = intOf(s, key); v && *v >= 0 && *v <= 5) {
            setInt(v, static_cast<std::int64_t>(rotate(static_cast<Dir>(*v), axis, q)));
        }
    }
    for (auto const* key : {"minecraft:facing_direction", "minecraft:block_face"}) {
        if (auto* v = strOf(s, key)) {
            if (auto d = dirFromName(*v)) setStr(v, dirName(rotate(*d, axis, q)));
        }
    }
    if (auto* v = strOf(s, "minecraft:cardinal_direction")) {
        if (auto d = dirFromName(*v)) {
            if (auto r = rotateHorizontal(*d, axis, q)) setStr(v, dirName(*r));
        }
    }
    if (auto* v = strOf(s, "pillar_axis")) {
        if (auto a = axisFromName(*v)) setStr(v, axisName(rotate(*a, axis, q)));
    }
    if (auto* v = intOf(s, "direction"); v && *v >= 0 && *v <= 3) {
        // legacy horizontal index: S W N E for most blocks, E S W N for doors; both run clockwise
        // seen from above, so a turn about Y is a shift. About X or Z the two orders disagree:
        // the value is kept (the block's own rotation handler can do better).
        if (axis == Axis::Y) setInt(v, normQuarter(static_cast<int>(*v) - q));
    }
    if (auto* v = intOf(s, "weirdo_direction"); v && *v >= 0 && *v <= 3) {
        if (auto r = rotateHorizontal(kWeirdo[*v], axis, q)) {
            for (int i = 0; i < 4; ++i)
                if (kWeirdo[i] == *r) setInt(v, i);
        }
    }
    if (auto* v = intOf(s, "ground_sign_direction"); v && *v >= 0 && *v <= 15 && axis == Axis::Y) {
        setInt(v, ((*v - 4 * q) % 16 + 16) % 16);
    }
    if (auto* v = strOf(s, "torch_facing_direction")) {
        std::optional<Dir> d = *v == "top" ? std::optional<Dir>(Dir::Down) : dirFromName(*v);
        if (d) {
            Dir r = rotate(*d, axis, q);
            if (r == Dir::Down) setStr(v, "top");
            else if (r != Dir::Up) setStr(v, dirName(r));
        }
    }
    if (auto* v = intOf(s, "rail_direction"); v && *v >= 0 && *v <= 9 && axis == Axis::Y) {
        // 0 N-S, 1 E-W, 2..5 ascending E W N S, 6..9 curves S-E, S-W, N-W, N-E
        static constexpr std::array<Dir, 4> kAscending{Dir::East, Dir::West, Dir::North, Dir::South};
        static constexpr std::array<std::pair<Dir, Dir>, 4> kCurves{
            {{Dir::South, Dir::East}, {Dir::South, Dir::West}, {Dir::North, Dir::West}, {Dir::North, Dir::East}}
        };
        if (*v <= 1) {
            if (q % 2) setInt(v, 1 - *v);
        } else if (*v <= 5) {
            Dir r = rotate(kAscending[*v - 2], axis, q);
            for (int i = 0; i < 4; ++i)
                if (kAscending[i] == r) setInt(v, 2 + i);
        } else {
            auto [a, b] = kCurves[*v - 6];
            Dir ra = rotate(a, axis, q), rb = rotate(b, axis, q);
            for (int i = 0; i < 4; ++i) {
                auto [ca, cb] = kCurves[i];
                if ((ca == ra && cb == rb) || (ca == rb && cb == ra)) setInt(v, 6 + i);
            }
        }
    }
    if (axis != Axis::Y && q == 2) {
        if (auto* v = strOf(s, "minecraft:vertical_half")) setStr(v, *v == "top" ? "bottom" : "top");
        if (auto* v = intOf(s, "upside_down_bit")) setInt(v, *v ? 0 : 1);
    }
    return changed;
}

} // namespace lv::core::contraption
