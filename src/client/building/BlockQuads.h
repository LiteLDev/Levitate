#pragma once

#ifdef LV_CLIENT

// Client side of the building unit: block meshes drawn by the client mod in the world (copycats,
// brackets, sliding doors, connected-texture corners).
//
// - `quadsOf(block)`: the block's quads as the chunk mesh draws them, in block units (the block's
//   bottom-north-west corner at the origin) with atlas UVs. Levitate blocks come from the pack's own
//   block files and geometry (core/ponder/BlockModel.h, as Ponder), other blocks are tessellated by
//   the game into our tessellator and read back.
// - `cropAndMove`: Create's BakedModelHelper.cropAndMove (the part of a quad inside a box, its UVs
//   following, then moved).
// - `Drawer`: one frame of drawing in the block entity pass (camera relative, lit at a position as
//   block entities are).

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "glm/glm.hpp"

class BaseActorRenderContext;
class Block;
class BlockPos;
class BlockSource;
class Tessellator;
namespace mce {
class Mesh;
}

namespace lv::client::building {

struct Quad {
    std::array<glm::vec3, 4> pos;
    std::array<glm::vec2, 4> uv; ///< atlas coordinates
    glm::vec4                color{1.f, 1.f, 1.f, 1.f};
};

/// Face direction of an axis-aligned quad lying on a face of the unit cube (0..5, Java order), -1
/// for a quad inside the block.
[[nodiscard]] int cubeFace(Quad const& q);

/// The quads of `block` (cached per block state for the current level). Empty when unknown.
[[nodiscard]] std::vector<Quad> const& quadsOf(Tessellator& tess, Block const& block);

/// The part of `q` inside [lo, hi] (block units), UVs following, then moved by `move`; nullopt
/// when nothing is left.
[[nodiscard]] std::optional<Quad> cropAndMove(Quad const& q, glm::vec3 lo, glm::vec3 hi, glm::vec3 move);

/// A GPU mesh of quads (both windings when `twoSided`).
[[nodiscard]] std::unique_ptr<mce::Mesh> buildMesh(Tessellator& tess, std::vector<Quad> const& quads, bool twoSided = false);

/// The atlas UVs of a terrain texture (u0, v0, u1, v1).
[[nodiscard]] glm::vec4 atlasUv(std::string const& texture);
/// The atlas UVs of the texture a block shows on `face` (0..5), from the game's block graphics;
/// nullopt when the block has none (data-driven blocks: use `atlasUv` with their texture name).
[[nodiscard]] std::optional<glm::vec4> faceUv(Block const& block, int face);

/// Drops the caches (new level: block states and the atlas are registered again).
void resetQuadCache();

class Drawer {
public:
    explicit Drawer(BaseActorRenderContext& ctx);
    [[nodiscard]] bool ok() const { return mOk; }
    [[nodiscard]] Tessellator& tessellator();
    /// Lights the next draws as a block entity at `pos` (its light level, day / night).
    void light(BlockSource& region, BlockPos const& pos);
    /// Draws `mesh` with `model` (block units, world position) relative to the camera.
    void draw(mce::Mesh const& mesh, glm::mat4 const& model);
    /// The same, blended by the vertex alpha (ghost blocks).
    void drawBlended(mce::Mesh const& mesh, glm::mat4 const& model);

private:
    void drawWith(void const* material, mce::Mesh const& mesh, glm::mat4 const& model);

    BaseActorRenderContext& mCtx;
    bool                    mOk = false;
    void const*             mMaterial = nullptr;
    void const*             mBlend    = nullptr;
    glm::vec3               mCam{0.f};
};

} // namespace lv::client::building

#endif // LV_CLIENT
