#pragma once

// Game rules of the schematic tools (pure C++, unit-tested): the schematic-and-quill selection,
// the schematicannon's print order, replace modes, fuel and flight times, the material checklist
// and the crafting blueprint's layout (Create 6.0.10 behaviour).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/schematics/Schematic.h"

namespace lv::core::schematics {

// ---- schematic and quill ---------------------------------------------------------------------------

/// Create's SchematicAndQuillHandler selection: two corners, resized by face scrolling.
struct Selection {
    std::optional<Pos> first, second;
    int                range = 10; ///< distance of mid-air selection (Ctrl-scroll, 1..100)

    [[nodiscard]] bool complete() const { return first && second; }
    /// {min, max} corners (inclusive) of the complete selection.
    [[nodiscard]] std::array<Pos, 2> box() const;
    [[nodiscard]] Pos                size() const;
    /// Ctrl-scroll before the second corner: changes the range (clamped 1..100).
    void scrollRange(int delta);
    /// Ctrl-scroll on face `face` (0..5, down up north south west east) of the complete selection:
    /// pushes / pulls that face (inverted when the camera is inside the box). Never shrinks
    /// below one block.
    void resize(int face, int delta, bool cameraInside);
};

/// Create's RaycastHelper.rayTraceRange: the first block along a ray for which `hit(pos)` is true,
/// with the face it was entered through (0..5, down up north south west east), up to `range`.
template <class F>
std::optional<std::pair<Pos, int>> raycast(double ox, double oy, double oz, double dx, double dy, double dz, double range, F&& hit);

// ---- schematicannon ----------------------------------------------------------------------------------

/// The four replace modes (Create's ConfigureSchematicannonPacket options, same order).
enum class ReplaceMode : int { DontReplaceSolid = 0, ReplaceWithSolid = 1, ReplaceWithAny = 2, ReplaceWithEmpty = 3 };

/// What the cannon knows about a target position (SchematicannonBlockEntity.shouldPlace).
struct PlaceQuery {
    ReplaceMode mode              = ReplaceMode::ReplaceWithAny;
    bool        placingAir        = false; ///< the schematic has air there
    bool        schematicSolid    = false; ///< the schematic block is a full solid cube
    bool        worldSolid        = false; ///< the world block is a full solid cube (redstone conductor)
    bool        worldOtherSolid   = false; ///< the other half (doors, beds) is a solid cube
    bool        worldHasData      = false; ///< the world block holds data (chest, ...)
    bool        protectData       = true;  ///< "Protect Block Entities" option
    bool        nearCannon        = false; ///< within 2 blocks of the cannon
    bool        ignored           = false; ///< upper halves of doors / beds, piston heads, ...
};
[[nodiscard]] bool shouldPlace(PlaceQuery const& q);

/// Ticks a launched block flies (Create's LaunchedItem.ticksForDistance).
[[nodiscard]] int flightTicks(Pos from, Pos to);

/// Print order: layer by layer from the bottom, rows along z, blocks along x (Create's printer),
/// brittle blocks (torches, rails, ...) deferred until all others are printed.
class PrintCursor {
public:
    PrintCursor() = default;
    PrintCursor(Pos min, Pos max) : mMin(min), mMax(max), mCur{min.x - 1, min.y, min.z} {}

    /// Next position (offset coordinates) or nullopt when done. `deferred(pos)` is asked once per
    /// position of the first pass.
    template <class F>
    std::optional<Pos> next(F&& deferred) {
        while (mStage == 0) {
            if (!advance()) {
                mStage = 1;
                break;
            }
            if (deferred(mCur)) {
                mDeferred.push_back(mCur);
                continue;
            }
            return mCur;
        }
        if (mStage == 1) {
            if (mDeferredIndex < mDeferred.size()) return mCur = mDeferred[mDeferredIndex++];
            mStage = 2;
        }
        return std::nullopt;
    }
    [[nodiscard]] Pos  current() const { return mCur; }
    [[nodiscard]] int  stage() const { return mStage; }
    [[nodiscard]] bool done() const { return mStage == 2; }

    /// State for saving: {stage, current, deferred..., deferredIndex}.
    [[nodiscard]] std::vector<int> save() const;
    [[nodiscard]] static PrintCursor load(Pos min, Pos max, std::vector<int> const& data);

private:
    bool advance();

    Pos              mMin{}, mMax{-1, -1, -1};
    Pos              mCur{};
    int              mStage = 0;
    std::vector<Pos> mDeferred;
    std::size_t      mDeferredIndex = 0;
};

/// Fuel: `remaining` shots, refilled with one gunpowder giving `shotsPerGunpowder` shots
/// (Create: only when empty; clamped when the config shrinks).
struct Fuel {
    int remaining = 0;
    /// True if a gunpowder should be consumed now (remaining <= 0).
    [[nodiscard]] bool needsRefill() const { return remaining <= 0; }
    void               refill(int shotsPerGunpowder) { remaining += shotsPerGunpowder; }
    void               clamp(int shotsPerGunpowder) {
        if (remaining > shotsPerGunpowder) remaining = shotsPerGunpowder;
    }
};

/// Material checklist (Create's MaterialChecklist): required vs. gathered per item.
struct Checklist {
    std::map<std::string, int> required; ///< item id -> count
    std::map<std::string, int> gathered;
    bool                       blocksNotLoaded = false;

    void require(std::string const& item, int count = 1);
    /// Counts an item found in the attached inventories (only items that are required).
    void collect(std::string const& item, int count);
    /// Missing count of an item (<= 0: complete).
    [[nodiscard]] int missing(std::string const& item) const;

    struct Entry {
        std::string item;
        int         amount   = 0; ///< still missing (or the total for completed entries)
        bool        complete = false;
    };
    /// Entries sorted by `nameOf(item)` (lower case), missing ones first, then completed ones.
    template <class NameOf>
    [[nodiscard]] std::vector<Entry> entries(NameOf&& nameOf) const;
};

/// " x{amount} | {stacks}▤ +{remainder}" as Create writes checklist amounts.
[[nodiscard]] std::string checklistAmount(int amount);
/// Book pages (5 entries per page, " >>>" at the end of full pages).
[[nodiscard]] std::vector<std::string> checklistPages(
    std::vector<Checklist::Entry> const& entries,
    std::vector<std::string> const&      names,
    bool                                 blocksNotLoaded,
    std::string const&                   notLoadedText
);

// ---- crafting blueprint ----------------------------------------------------------------------------

/// The block positions of a blueprint of `size` (1..3) placed against `face` (0..5, the side
/// of the supporting block it was placed on, i.e. the direction it faces), with `horizontal`
/// (2..5) the player's horizontal facing for floor / ceiling blueprints. Index = col + row * size
/// (columns left to right, rows bottom to top, as seen by a viewer looking at it).
[[nodiscard]] std::vector<Pos> blueprintCells(Pos clicked, int face, int horizontal, int size);

template <class F>
std::optional<std::pair<Pos, int>> raycast(double ox, double oy, double oz, double dx, double dy, double dz, double range, F&& hit) {
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9) return std::nullopt;
    dx /= len, dy /= len, dz /= len;
    Pos    p{static_cast<int>(std::floor(ox)), static_cast<int>(std::floor(oy)), static_cast<int>(std::floor(oz))};
    int    sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    auto   next = [](double o, int c, double d) {
        if (std::abs(d) < 1e-12) return 1e30;
        double b = d > 0 ? c + 1.0 : static_cast<double>(c);
        return (b - o) / d;
    };
    double tx = next(ox, p.x, dx), ty = next(oy, p.y, dy), tz = next(oz, p.z, dz);
    double ddx = std::abs(dx) < 1e-12 ? 1e30 : 1.0 / std::abs(dx), ddy = std::abs(dy) < 1e-12 ? 1e30 : 1.0 / std::abs(dy),
           ddz = std::abs(dz) < 1e-12 ? 1e30 : 1.0 / std::abs(dz);
    int    face = -1;
    for (int guard = 0; guard < 4096; ++guard) {
        if (face >= 0 && hit(p)) return std::pair{p, face};
        if (tx <= ty && tx <= tz) {
            if (tx > range) break;
            p.x += sx;
            tx += ddx;
            face = sx > 0 ? 4 : 5; // entered through the west / east face
        } else if (ty <= tz) {
            if (ty > range) break;
            p.y += sy;
            ty += ddy;
            face = sy > 0 ? 0 : 1;
        } else {
            if (tz > range) break;
            p.z += sz;
            tz += ddz;
            face = sz > 0 ? 2 : 3;
        }
    }
    return std::nullopt;
}

template <class NameOf>
std::vector<Checklist::Entry> Checklist::entries(NameOf&& nameOf) const {
    std::vector<std::pair<std::string, std::string>> keyed; // (lower-case name, item)
    for (auto const& [item, _] : required) {
        std::string n = nameOf(item);
        for (auto& c : n) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        keyed.emplace_back(std::move(n), item);
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<Entry> missingEntries, done;
    for (auto const& [_, item] : keyed) {
        int m = missing(item);
        if (m > 0) missingEntries.push_back({item, m, false});
        else done.push_back({item, required.at(item), true});
    }
    missingEntries.insert(missingEntries.end(), done.begin(), done.end());
    return missingEntries;
}

} // namespace lv::core::schematics
