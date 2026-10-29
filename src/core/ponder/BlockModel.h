#pragma once

// Block models of data-driven blocks for the Ponder renderer (U30), pure C++.
//
// The Ponder scene draws Levitate blocks from the same data the game uses: the behavior pack
// block (`minecraft:geometry` + `bone_visibility`, `minecraft:material_instances`,
// `minecraft:transformation`, permutations with Molang conditions on block states) and the resource
// pack geometry (`models/blocks/*.geo.json`). `resolve` picks the components for a set of state
// values; `buildQuads` turns the geometry into textured quads in block units (0..1, the block's
// bottom-north-west corner at the origin), each tagged with its texture name.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::ponder {

using V3 = std::array<float, 3>;
using V2 = std::array<float, 2>;

// ---- Molang (the subset block files use) -------------------------------------------------------

/// Evaluates a block-file Molang expression: numbers, `true` / `false`, `q.block_state('name')` /
/// `query.block_state(...)`, `!`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, parentheses and
/// string literals. Unknown states read as 0; string states compare by `strings`. Returns nullopt
/// on a syntax error.
[[nodiscard]] std::optional<float> evalMolang(
    std::string const&                        expr,
    std::map<std::string, int> const&         states,
    std::map<std::string, std::string> const& strings = {}
);

// ---- geometry ------------------------------------------------------------------------------------

struct ModelFace {
    std::string             bone;
    std::string             material; ///< material instance name (face name or the face's own)
    std::string             face;     ///< north / south / east / west / up / down (model space)
    std::array<V3, 4>       pos;      ///< block units, outside view TL, TR, BR, BL
    std::array<V2, 4>       uv;       ///< 0..1 over the texture
};

struct Geometry {
    std::vector<ModelFace> faces;
};

/// Parses a `.geo.json` file into geometries by identifier (cubes with box or per-face UVs, cube
/// and bone rotations about their pivots, bone parents). Model x is mirrored as Bedrock does.
[[nodiscard]] std::map<std::string, Geometry> parseGeometries(nlohmann::json const& file);

/// The geometry of `minecraft:geometry.full_block`.
[[nodiscard]] Geometry const& fullBlock();

// ---- block definitions --------------------------------------------------------------------------

struct BlockFile {
    std::string                                       id;
    nlohmann::json                                    components;   ///< base components
    std::vector<std::pair<std::string, nlohmann::json>> permutations; ///< (condition, components)
    std::map<std::string, int>                        defaults;     ///< first value of each integer state
};

/// Reads a behavior pack block file; nullopt when it is not one.
[[nodiscard]] std::optional<BlockFile> parseBlockFile(nlohmann::json const& file);

struct ResolvedBlock {
    std::string                        geometry;       ///< geometry identifier
    std::map<std::string, std::string> boneVisibility; ///< bone -> Molang
    std::map<std::string, std::string> materials;      ///< instance -> texture (aliases resolved)
    V3                                 rotation{0, 0, 0};
    V3                                 translation{0, 0, 0};
    V3                                 scale{1, 1, 1};
};

/// The components in effect for `states` / `strings` (base components, then every matching permutation in
/// order). Block traits not given use Bedrock's defaults (block_face "down", cardinal_direction "south",
/// facing_direction "down").
[[nodiscard]] ResolvedBlock resolve(
    BlockFile const&                          file,
    std::map<std::string, int> const&         states,
    std::map<std::string, std::string> const& strings = {}
);

/// `strings` with the trait defaults filled in.
[[nodiscard]] std::map<std::string, std::string> withTraitDefaults(std::map<std::string, std::string> strings);

/// Texture of a face: its own material instance, then its direction, "side" for the four sides, "*".
[[nodiscard]] std::string textureOf(ResolvedBlock const& block, ModelFace const& face);

struct Quad {
    std::array<V3, 4> pos;
    std::array<V2, 4> uv;
    std::string       texture;
    float             shade = 1; ///< Minecraft's directional face shading
};

/// Visible, transformed quads of a block (`bone_visibility` evaluated with `states`).
[[nodiscard]] std::vector<Quad> buildQuads(
    ResolvedBlock const&              block,
    Geometry const&                   geometry,
    std::map<std::string, int> const& states,
    std::vector<std::string> const&   hideBones = {},
    std::map<std::string, std::string> const& strings = {}
);

} // namespace lv::core::ponder
