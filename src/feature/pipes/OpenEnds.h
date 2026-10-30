#pragma once

// Open pipe ends and world fluids (internal to the pipes unit).

#include <cstdint>
#include <optional>
#include <string>

#include "feature/common/FluidRegistry.h"
#include "feature/pipes/Pipes.h"

namespace lv::feature::pipes {

/// A fluid block in the world.
struct WorldFluid {
    std::string id;
    bool        source = false;
};
[[nodiscard]] std::optional<WorldFluid> worldFluidAt(BlockSource& region, BlockPos const& pos);
/// Create's `VanillaFluidTargets.canProvideFluidWithoutCapability` (beehives, cauldrons).
[[nodiscard]] bool canProvideFluidWithoutCapability(Block const& block);
/// The block can be waterlogged (Java's WATERLOGGED property): its water detection rule allows water.
[[nodiscard]] bool canContainWater(Block const& block);
/// The block at `pos` holds a water source in its liquid layer.
[[nodiscard]] bool waterlogged(BlockSource& region, BlockPos const& pos);

/// Create's `OpenEndedPipe.removeFluidFromSpace` / `provideFluidToSpace`.
cp::Fluid removeFluidFromSpace(BlockSource& region, BlockPos const& out, bool simulate);
bool      provideFluidToSpace(BlockSource& region, BlockPos const& out, cp::Fluid const& fluid, bool simulate);

/// Create's open end fluid handler (1000 mB buffer) in front of `at`.
int openEndFill(BlockSource& region, cp::BlockFace const& at, cp::OpenEnd& state, cp::Fluid const& fluid, bool simulate, std::int64_t gameTime);
cp::Fluid openEndDrain(BlockSource& region, cp::BlockFace const& at, cp::OpenEnd& state, cp::Fluid const& request, bool simulate);
/// The area an open end affects.
[[nodiscard]] common::fluidtypes::Area openEndArea(cp::BlockFace const& at);

/// Create's `FluidReactions.handlePipeFlowCollision`.
void flowCollision(BlockSource& region, BlockPos const& pos, cp::Fluid const& a, cp::Fluid const& b);

} // namespace lv::feature::pipes
