#pragma once

// Rotating-part meshes from Bedrock block geometry (pure C++).
//
// The client mod draws the rotating parts of kinetic blocks itself. Their shape comes from the same
// `models/blocks/*.geo.json` the static block uses: each bone becomes a list of textured quads.
// Coordinates: block units with the block's bottom-north-west corner at (0, 0, 0) and the model's
// X mirrored as Bedrock does (model x = -world x). UVs are 0..1 over the model's texture, to be
// mapped onto the texture's place in the terrain atlas by the caller.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"

namespace lv::core::kinetics {

struct ModelQuad {
    std::array<std::array<float, 3>, 4> pos; ///< corners in order (outside view: TL, TR, BR, BL)
    std::array<std::array<float, 2>, 4> uv;
};

struct RotorModel {
    std::map<std::string, std::vector<ModelQuad>> bones;
};

/// Parses a geometry file ("minecraft:geometry": [...]) into models by identifier. Supports cubes
/// with per-face or box UVs, cube rotation about a pivot and bone-to-bone parents are ignored.
/// Returns nullopt (with `error`) if the file is not geometry.
[[nodiscard]] std::optional<std::map<std::string, RotorModel>> parseGeometry(nlohmann::json const& file, std::string* error = nullptr);

} // namespace lv::core::kinetics
