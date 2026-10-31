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

/// The legacy `direction` (0..3) order of a block: S W N E for most blocks, E S W N for doors,
/// E W S N for trapdoors (Bedrock's own orders).
std::array<Dir, 4> legacyOrder(std::string_view id) {
    bool trapdoor = id.find("trapdoor") != std::string_view::npos;
    bool door     = !trapdoor && (id.ends_with("_door") || id == "minecraft:wooden_door");
    if (trapdoor) return {Dir::East, Dir::West, Dir::South, Dir::North};
    if (door) return {Dir::East, Dir::South, Dir::West, Dir::North};
    return {Dir::South, Dir::West, Dir::North, Dir::East};
}

/// A lever (`lever_direction`): the direction away from the block it is on and the axis it
/// swings along (wall levers swing vertically).
struct Lever {
    Dir  facing;
    Axis swing;
};
std::optional<Lever> parseLever(std::string_view v) {
    auto axisOfSuffix = [&](std::string_view rest) -> std::optional<Axis> {
        if (rest == "east_west") return Axis::X;
        if (rest == "north_south") return Axis::Z;
        return std::nullopt;
    };
    if (v.starts_with("up_")) {
        if (auto a = axisOfSuffix(v.substr(3))) return Lever{Dir::Up, *a};
        return std::nullopt;
    }
    if (v.starts_with("down_")) {
        if (auto a = axisOfSuffix(v.substr(5))) return Lever{Dir::Down, *a};
        return std::nullopt;
    }
    if (auto d = dirFromName(std::string(v)); d && axisOf(*d) != Axis::Y) return Lever{*d, Axis::Y};
    return std::nullopt;
}
std::optional<std::string> leverName(Lever const& l) {
    if (axisOf(l.facing) != Axis::Y) return std::string(dirName(l.facing));
    if (l.swing == Axis::Y) return std::nullopt;
    return std::string(l.facing == Dir::Up ? "up_" : "down_") + (l.swing == Axis::X ? "east_west" : "north_south");
}

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

bool rotateStates(States& s, Axis axis, int quarterTurns, std::string_view blockId) {
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
        // legacy horizontal index in the block's own order; a turn that would make it vertical
        // keeps it (Create's StructureTransform: half turns about X or Z flip it unless it lies
        // along the axis)
        auto order = legacyOrder(blockId);
        if (auto r = rotateHorizontal(order[static_cast<std::size_t>(*v)], axis, q)) {
            for (int i = 0; i < 4; ++i)
                if (order[static_cast<std::size_t>(i)] == *r) setInt(v, i);
        }
    }
    // stairs turned about X or Z (Create's transformStairs): a stair is the half block on its
    // vertical side plus the half towards its facing; after the turn the horizontal one of the two
    // is the new facing and the vertical one the new half
    auto* weirdo = intOf(s, "weirdo_direction");
    auto* upside = intOf(s, "upside_down_bit");
    bool  stairs = weirdo && upside && *weirdo >= 0 && *weirdo <= 3;
    if (stairs && axis != Axis::Y) {
        Dir f  = kWeirdo[static_cast<std::size_t>(*weirdo)];
        Dir h  = *upside ? Dir::Up : Dir::Down;
        Dir rf = rotate(f, axis, q), rh = rotate(h, axis, q);
        bool rfHorizontal = axisOf(rf) != Axis::Y, rhHorizontal = axisOf(rh) != Axis::Y;
        if (rfHorizontal != rhHorizontal) { // both horizontal: facing along the axis, a quarter turn keeps it
            Dir facing = rfHorizontal ? rf : rh;
            Dir half   = rfHorizontal ? rh : rf;
            for (int i = 0; i < 4; ++i)
                if (kWeirdo[static_cast<std::size_t>(i)] == facing) setInt(weirdo, i);
            setInt(upside, half == Dir::Up ? 1 : 0);
        } else if (q == 2) {
            setInt(upside, *upside ? 0 : 1);
        }
    } else if (weirdo && *weirdo >= 0 && *weirdo <= 3) {
        if (auto r = rotateHorizontal(kWeirdo[static_cast<std::size_t>(*weirdo)], axis, q)) {
            for (int i = 0; i < 4; ++i)
                if (kWeirdo[i] == *r) setInt(weirdo, i);
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
    if (auto* v = strOf(s, "lever_direction")) {
        // Create turns FaceAttachedHorizontalDirectionalBlocks (levers) about every axis
        if (auto lever = parseLever(*v)) {
            Lever turned{rotate(lever->facing, axis, q), rotate(lever->swing, axis, q)};
            if (auto name = leverName(turned)) setStr(v, *name);
        }
    }
    if (axis != Axis::Y && q == 2) {
        if (auto* v = strOf(s, "minecraft:vertical_half")) setStr(v, *v == "top" ? "bottom" : "top");
        if (auto* v = intOf(s, "upside_down_bit"); v && !stairs) setInt(v, *v ? 0 : 1);
    }
    return changed;
}

} // namespace lv::core::contraption
