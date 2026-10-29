#pragma once

// World item renderer of the client mod (U33). Client build only.
//
// Draws ItemStacks anywhere in the world with the game's own item models (3D block items, flat
// item sprites, special block items such as torches), lit and coloured as the game draws held /
// dropped items. Two uses:
//
// - Display items published by the server (feature/itemrender/DisplayItems.h): this module
//   receives them ("itemrender.batch"), interpolates them every frame and draws them, and hides
//   the fallback item entities they name. Units need no client code for these.
// - Client code that already draws something every frame (a deployer hand, an arm claw) draws an
//   item in its own block entity pass hook:
//
//       glm::mat4 m = glm::translate(glm::mat4{1.f}, render::relative(ctx, {x, y, z}));
//       m = glm::scale(m, glm::vec3{.5f});
//       render::drawItem(ctx, stack, m, render::Style::Facing);
//
// The canonical item is one block big. `Style::Facing` (item frames, filter slots, placards): flat
// items stand in the XY plane centred on the origin and read correctly from +Z; block items are a
// half size upright cube standing in front of that plane (centre at +Z 0.25). `Style::Lying` (belts, depots):
// flat items lie face up (+Y) centred on the origin; block items stand on it (centre at +Y 0.5).

#ifdef LV_CLIENT

#include <array>

#include "glm/mat4x4.hpp"

#include "core/itemrender/DisplayItem.h"

class BaseActorRenderContext;
class ItemStack;

namespace lv::client::render {

using core::itemrender::Style;

/// `world` (absolute block coordinates) relative to the camera of this pass, as the world matrix
/// of the block entity pass expects.
[[nodiscard]] glm::vec3 relative(BaseActorRenderContext& ctx, std::array<double, 3> const& world);
/// Draws `stack` with `transform` (camera-relative, see `relative`) applied to the canonical item.
/// Only during the world render passes (block entity / entity hooks).
void drawItem(BaseActorRenderContext& ctx, ItemStack const& stack, glm::mat4 const& transform, Style style = Style::Facing);
/// The matrix of a display transform (core::itemrender::Transform), camera-relative.
[[nodiscard]] glm::mat4 matrixOf(BaseActorRenderContext& ctx, core::itemrender::Transform const& t);
/// Whether the game draws the stack as a block cube (Create's "gui 3d" items).
[[nodiscard]] bool isBlockItem(ItemStack const& stack);

/// Display items currently received (selftests / scenarios).
[[nodiscard]] std::size_t displayItemCount();

} // namespace lv::client::render

#endif // LV_CLIENT
