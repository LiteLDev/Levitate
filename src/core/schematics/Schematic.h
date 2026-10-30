#pragma once

// Schematic files (pure C++): the in-memory structure, Bedrock `.mcstructure` reading / writing
// and Create / Java `.nbt` structure reading (block states mapped to Bedrock by JavaMapping.h).
// Placement transforms (Create's StructurePlaceSettings: mirror, then rotation about the anchor)
// for positions and block states.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/schematics/Nbt.h"

namespace lv::core::schematics {

struct Pos {
    int x = 0, y = 0, z = 0;
    friend bool operator==(Pos const&, Pos const&) = default;
    friend auto operator<=>(Pos const&, Pos const&) = default;
    Pos         operator+(Pos const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Pos         operator-(Pos const& o) const { return {x - o.x, y - o.y, z - o.z}; }
};

/// A Bedrock block state: name + states (+ alternative names to try when `name` doesn't exist).
struct BlockState {
    std::string              name;
    nbt::Compound            states;
    std::int32_t             version = 0;
    std::vector<std::string> alternatives;

    [[nodiscard]] bool isAir() const { return name == "minecraft:air"; }
    /// "minecraft:oak_stairs[upside_down_bit=1,weirdo_direction=2]"
    [[nodiscard]] std::string describe() const;
    friend bool               operator==(BlockState const& a, BlockState const& b);
};

/// Positions without a block (structure void) have index -1: they are never printed.
inline constexpr std::int32_t kVoid = -1;

struct Schematic {
    Pos                        size;
    std::vector<BlockState>    palette;
    std::vector<std::int32_t>  blocks;  ///< x-major, then y, then z (the .mcstructure order)
    std::vector<std::int32_t>  liquids; ///< second layer (waterlogging), empty or same size
    /// Levitate BlockData documents (JSON text) per block index.
    std::map<std::size_t, std::string> levitateData;
    /// Block entity data of the source file per block index (kept, not printed).
    std::map<std::size_t, nbt::Tag> blockEntities;
    int                             entities = 0; ///< entities in the file
    /// Bedrock entity NBT of a `.mcstructure` (absolute "Pos" in the world the structure was saved
    /// from, see `worldOrigin`); Java entities are only counted.
    std::vector<nbt::Tag> entityTags;
    /// `structure_world_origin` of a `.mcstructure`: the world position of local (0, 0, 0) when saved.
    Pos worldOrigin;
    std::string                     format;       ///< "mcstructure" or "nbt"

    [[nodiscard]] std::size_t index(Pos p) const {
        return (static_cast<std::size_t>(p.x) * size.y + static_cast<std::size_t>(p.y)) * size.z + static_cast<std::size_t>(p.z);
    }
    [[nodiscard]] Pos  posOf(std::size_t i) const;
    [[nodiscard]] bool contains(Pos p) const { return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < size.x && p.y < size.y && p.z < size.z; }
    [[nodiscard]] std::size_t volume() const { return static_cast<std::size_t>(size.x) * size.y * size.z; }
    [[nodiscard]] std::int32_t at(Pos p) const { return contains(p) ? blocks[index(p)] : kVoid; }
    [[nodiscard]] BlockState const* stateAt(Pos p) const;
    /// Index of `state` in the palette (added when new).
    std::int32_t paletteIndex(BlockState const& state);
    void         resize(Pos size);
    /// Positions with a non-air block.
    [[nodiscard]] std::size_t solidCount() const;
};

/// Hard limits for files we accept (a file this big is refused, not truncated).
inline constexpr int         kMaxDimension = 512;
inline constexpr std::size_t kMaxVolume    = 8u << 20;

[[nodiscard]] std::optional<Schematic> readMcstructure(std::string_view bytes, std::string* error = nullptr);
[[nodiscard]] std::string              writeMcstructure(Schematic const& s, Pos worldOrigin = {});
/// A decompressed Java structure NBT (Create's `.nbt` after GZIP).
[[nodiscard]] std::optional<Schematic> readJavaStructure(std::string_view nbtBytes, std::string* error = nullptr);
/// Loads by file extension: `.mcstructure` or `.nbt` (GZIP, or raw NBT).
[[nodiscard]] std::optional<Schematic> load(std::string_view fileName, std::string_view bytes, std::string* error = nullptr);
/// File name accepted for schematics ("x.nbt", "x.mcstructure"), no paths.
[[nodiscard]] bool isSchematicFileName(std::string_view name);

// ---- placement transforms ------------------------------------------------------------------------

/// Create's Mirror order: NONE, LEFT_RIGHT (flips z), FRONT_BACK (flips x).
enum class Mirror : int { None = 0, LeftRight = 1, FrontBack = 2 };

struct Transform {
    int    rotation = 0; ///< clockwise quarter turns seen from above (Create's Rotation ordinal)
    Mirror mirror   = Mirror::None;
};

/// Local schematic position -> offset from the anchor (mirror, then rotate about the origin).
[[nodiscard]] Pos apply(Transform const& t, Pos local);
/// Offset from the anchor -> local position (inverse of `apply`).
[[nodiscard]] Pos unapply(Transform const& t, Pos offset);
/// The transformed bounds relative to the anchor: {min, max} (inclusive).
[[nodiscard]] std::array<Pos, 2> bounds(Transform const& t, Pos size);
/// Mirrors and rotates the direction-like states of a block (facing, stairs, doors, signs, rails,
/// axes, torches, vines, walls, Levitate facings). Returns true if anything changed.
bool transformStates(BlockState& state, Transform const& t);

/// Create's StructureTransform for the anchor of the client placement tool: the anchor that
/// keeps the transformed structure's footprint where it was before a rotation / mirror change.
[[nodiscard]] Pos recenterAnchor(Pos anchor, Pos size, Transform const& from, Transform const& to);

} // namespace lv::core::schematics
