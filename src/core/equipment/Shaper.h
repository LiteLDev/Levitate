#pragma once

// Handheld worldshaper brushes, tools, placement options and patterns (U31, SPEC §3.6.31),
// independent of the game. Create's terrainzapper package.

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::equipment {

enum class Brush : int { Cuboid = 0, Sphere = 1, Cylinder = 2, Surface = 3, Cluster = 4 };
enum class ShaperTool : int { Fill = 0, Place = 1, Replace = 2, Clear = 3, Overlay = 4, Flatten = 5 };
enum class Placement : int { Merged = 0, Attached = 1, Inserted = 2 };
enum class Pattern : int { Solid = 0, Checkered = 1, InverseCheckered = 2, Chance25 = 3, Chance50 = 4, Chance75 = 5 };

struct IPos {
    int x = 0, y = 0, z = 0;
    friend bool operator==(IPos const&, IPos const&) = default;
    friend auto operator<=>(IPos const&, IPos const&) = default;
};

/// Brush parameters as Create's: cuboid w/h/l (1..32), sphere radius (0..10), cylinder radius
/// (0..8) + height (1..8), surface / cluster range (1..10) + Follow Diagonals + Ignore Material
/// Borders.
struct ShaperSettings {
    Brush                brush     = Brush::Cuboid;
    std::array<int, 3>   params    = {1, 1, 1};
    ShaperTool           tool      = ShaperTool::Fill;
    Placement            placement = Placement::Merged;
    Pattern              pattern   = Pattern::Solid;
    bool                 configured = false; ///< the GUI was confirmed once (Create: BRUSH component present)
    std::string          material;           ///< serialised block (game layer), "" = none

    [[nodiscard]] nlohmann::json toJson() const;
    static ShaperSettings        fromJson(nlohmann::json const& j);
};

struct ParamSpec {
    std::string key; ///< translation key of the label
    std::string fallback;
    int         min = 0, max = 0, def = 0;
    bool        toggle = false;
};
/// Parameter rows of a brush (Create's Brush.getParamLabel / min / max).
[[nodiscard]] std::vector<ParamSpec> brushParams(Brush b);
/// Tools a brush offers (Create: Surface: Overlay, Replace, Clear; Cluster: Replace, Clear;
/// shaped brushes: every tool).
[[nodiscard]] std::vector<ShaperTool> brushTools(Brush b);
/// Clamps params into range and the tool into the brush's tools.
void sanitize(ShaperSettings& s);
/// Whether the brush uses placement options (shaped brushes).
[[nodiscard]] bool hasPlacement(Brush b);

/// Offsets of a shaped brush (cuboid, sphere, cylinder) around (0, 0, 0).
[[nodiscard]] std::vector<IPos> brushShape(ShaperSettings const& s);
/// Placement shift along the clicked face (Bedrock face index 0 down .. 5 east).
[[nodiscard]] IPos placementOffset(ShaperSettings const& s, int face);

/// Flood fill of a dynamic brush (Create's DynamicBrush): from `start`, through neighbours
/// (6, or 26 with Follow Diagonals; surface brushes stay in the plane of `face`), within
/// Euclidean distance < range of the start, visiting positions for which `include(p)` holds.
/// The caller's `include` applies Create's replace / place criteria.
[[nodiscard]] std::vector<IPos> floodFill(ShaperSettings const& s, IPos start, int face, std::function<bool(IPos const&)> const& include);
/// Follow Diagonals / Ignore Material Borders toggles of a dynamic brush (Create: param == 0 is on).
[[nodiscard]] inline bool followDiagonals(ShaperSettings const& s) { return s.params[1] == 0; }
[[nodiscard]] inline bool ignoreMaterialBorders(ShaperSettings const& s) { return s.params[2] == 0; }

/// Whether a position keeps its block under a pattern (`rnd()` returns a fair random bit).
[[nodiscard]] bool patternKeeps(Pattern p, IPos const& pos, std::function<bool()> const& rnd);

/// What a tool does at a position (Fill / Place / Replace / Clear): 0 nothing, 1 set the
/// material, 2 set air. `replaceable`: the world block there is air / replaceable.
[[nodiscard]] int toolAction(ShaperTool t, bool replaceable);

/// Height map value for a column that has no surface (Create: Integer.MIN_VALUE).
inline constexpr int kNoHeight = -2147483647 - 1;
/// Create's FlattenTool kernel: 5x5 Gaussian, edge cells and kNoHeight read as the cell itself,
/// floor(v + 0.5). `rows` x `cols`, row-major.
[[nodiscard]] std::vector<int> smoothHeights(std::vector<int> const& heights, int rows, int cols);

/// One unit edge of a cluster outline, from `a` to `b` (block corner coordinates).
struct Edge {
    IPos a, b;
    friend bool operator==(Edge const&, Edge const&) = default;
    friend auto operator<=>(Edge const&, Edge const&) = default;
};
/// Create's Outliner#showCluster: the edges of the outline of a set of blocks (edges of faces
/// between a block of the set and one outside it, except edges inside a flat side of the set).
[[nodiscard]] std::vector<Edge> clusterEdges(std::vector<IPos> const& blocks);

/// Translation keys (Create's `create.gui.terrainzapper.*`).
[[nodiscard]] std::string brushKey(Brush b);
[[nodiscard]] std::string toolKey(ShaperTool t);
[[nodiscard]] std::string placementKey(Placement p);
[[nodiscard]] std::string patternKey(Pattern p);
[[nodiscard]] std::string brushFallback(Brush b);
[[nodiscard]] std::string toolFallback(ShaperTool t);
[[nodiscard]] std::string placementFallback(Placement p);
[[nodiscard]] std::string patternFallback(Pattern p);

} // namespace lv::core::equipment
