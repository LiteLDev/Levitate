#pragma once

// Curves of a world, indexed by their end blocks and by the blocks they pass through
// (Bedrock blocks have no block entities, so this replaces Create's TrackBlockEntity connections).

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/tracks/Curve.h"

namespace lv::core::tracks {

class CurveStore {
public:
    using CurvePtr = std::shared_ptr<Curve const>;

    /// Adds `curve` (its primary orientation). Replaces a curve between the same two blocks.
    /// Returns the curve that was replaced, if any.
    std::optional<Curve> add(int dim, Curve const& curve);
    /// Removes the curve between `a` and `b` (either order); returns it (primary orientation).
    std::optional<Curve> remove(int dim, BlockPos3 const& a, BlockPos3 const& b);
    /// Removes every curve ending at `pos`; returns them (primary orientation).
    std::vector<Curve> removeAllAt(int dim, BlockPos3 const& pos);
    void               clear();

    /// Curves ending at `pos`, each oriented from `pos` (block(0) == pos).
    [[nodiscard]] std::vector<CurvePtr> curvesAt(int dim, BlockPos3 const& pos) const;
    /// The curve between `a` and `b`, oriented from `a`.
    [[nodiscard]] CurvePtr between(int dim, BlockPos3 const& a, BlockPos3 const& b) const;
    [[nodiscard]] bool     hasCurvesAt(int dim, BlockPos3 const& pos) const;
    /// Curves passing through `pos` (fake track blocks), primary orientation.
    [[nodiscard]] std::vector<CurvePtr> curvesThrough(int dim, BlockPos3 const& pos) const;
    /// Every curve (primary orientation) with its dimension.
    [[nodiscard]] std::vector<std::pair<int, CurvePtr>> all() const;
    [[nodiscard]] size_t                                size() const;

    /// Chunk (16×16 column) that stores a curve: the chunk of its primary block(0).
    [[nodiscard]] static std::pair<int, int> chunkOf(BlockPos3 const& p) {
        return {p.x >> 4, p.z >> 4};
    }
    /// Curves stored in one chunk (primary orientation).
    [[nodiscard]] std::vector<CurvePtr> inChunk(int dim, int cx, int cz) const;

private:
    struct Key {
        BlockPos3 a, b; ///< primary block(0), block(1)
        friend auto operator<=>(Key const&, Key const&) = default;
        friend bool operator==(Key const&, Key const&)  = default;
    };
    struct DimData {
        std::map<Key, CurvePtr>                                 primaries;
        std::map<BlockPos3, std::map<BlockPos3, CurvePtr>>      byEnd;     ///< pos → other end → oriented from pos
        std::map<BlockPos3, std::set<Key>>                      byBlock;   ///< fake track block → curves
        std::map<std::pair<int, int>, std::set<Key>>            byChunk;   ///< chunk of the primary block → curves
    };
    void index(DimData& d, Key const& key, CurvePtr const& c);
    void unindex(DimData& d, Key const& key, CurvePtr const& c);

    std::map<int, DimData> mDims;
};

} // namespace lv::core::tracks
